//===- CJSimpleRangeAnalysis.cpp --------------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This pass implements simple range analysis for integers and floats whose
// values are integers, and optimizes CFG based on the analysis result.
//
//===----------------------------------------------------------------------===//
#include "llvm/Transforms/Scalar/CJSimpleRangeAnalysis.h"

#include "llvm/Analysis/DomTreeUpdater.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/InitializePasses.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"
#include <cstdint>

using namespace llvm;
static cl::opt<bool> EnableCJSimpleRA("enable-cj-simple-ra", cl::Hidden,
                                      cl::init(true));

constexpr unsigned BIT_WIDTH_64 = 64;

// Get constant int if valid.
std::pair<bool, int64_t>
CJSimpleRangeAnalysisPass::getValidConstantInteger(Value *V) {
  if (auto *CI = dyn_cast<ConstantInt>(V))
    return {true, CI->getValue().getSExtValue()};
  if (auto *CFP = dyn_cast<ConstantFP>(V)) {
    APFloat APF = CFP->getValueAPF();
    if (!APF.isIEEE() || APF.isInfinity() || !APF.isInteger() || APF.isNaN() ||
        !V->getType()->isDoubleTy())
      return {false, 0};
    double Value = APF.convertToDouble();
    // The integers represented by double are also inaccurate up to a certain
    // point. Conservatively, only double in the int32 range is considered.
    if (Value > static_cast<double>(INT32_MAX) ||
        Value < static_cast<double>(INT32_MIN))
      return {false, 0};
    return {true, static_cast<int64_t>(Value)};
  }
  return {false, 0};
}

// Calculate the difference of LHS based on base value and return base.
Value *CJSimpleRangeAnalysisPass::getRangeFromInitValue(
    Value *V, APInt &APValue, SmallSet<Value *, 8> &Visited) {
  Value *OPLHS;
  ConstantInt *OPConstInt;
  if (!Visited.insert(V).second)
    return V;
  // Phi needs to ensure that all incoming values have the same base and
  // difference.
  if (auto *Phi = dyn_cast<PHINode>(V)) {
    Value *OriginBV = nullptr;
    APInt OriginDiff = APValue;
    // Phi in loop header, stop.
    auto *L = LI->getLoopFor(Phi->getParent());
    if (L && L->getHeader() == Phi->getParent())
      return V;
    // Be sure all incoming values are from same base value.
    for (unsigned I = 0, E = Phi->getNumIncomingValues(); I != E; ++I) {
      APInt TmpDiff = APValue;
      auto *BV =
          getRangeFromInitValue(Phi->getIncomingValue(I), TmpDiff, Visited);
      if (!OriginBV) {
        OriginBV = BV;
        OriginDiff = TmpDiff;
      } else if (OriginBV != BV || OriginDiff != TmpDiff) {
        return V;
      }
    }
    APValue = OriginDiff;
    return OriginBV;
  }
  using namespace PatternMatch;
  if (match(V, m_Add(m_Value(OPLHS), m_ConstantInt(OPConstInt)))) {
    APValue += OPConstInt->getSExtValue();
    return getRangeFromInitValue(OPLHS, APValue, Visited);
  }
  if (match(V, m_Sub(m_Value(OPLHS), m_ConstantInt(OPConstInt)))) {
    APValue -= OPConstInt->getSExtValue();
    return getRangeFromInitValue(OPLHS, APValue, Visited);
  }
  ConstantFP *OPConstFP;
  if (match(V, m_FAdd(m_Value(OPLHS), m_ConstantFP(OPConstFP)))) {
    auto [Valid, IV] = getValidConstantInteger(OPConstFP);
    if (Valid) {
      APValue += IV;
      return getRangeFromInitValue(OPLHS, APValue, Visited);
    }
  }
  if (match(V, m_FSub(m_Value(OPLHS), m_ConstantFP(OPConstFP)))) {
    auto [Valid, IV] = getValidConstantInteger(OPConstFP);
    if (Valid) {
      APValue -= IV;
      return getRangeFromInitValue(OPLHS, APValue, Visited);
    }
  }
  return V;
}

std::pair<Value *, int64_t>
CJSimpleRangeAnalysisPass::checkAndGetConditionBaseDiff(BranchInst *BI,
                                                        APInt &Diff) {
  if (auto *Cmp =
          dyn_cast<CmpInst>(BI->getCondition())) { // Now only support cmp.
    Value *CmpLHS = Cmp->getOperand(0);
    Value *CmpRHS = Cmp->getOperand(1);
    // Currently, only deal with (cmp pred %x, constant integer).
    auto [Tag, IV] = getValidConstantInteger(CmpRHS);
    if (!Tag)
      return {nullptr, 0};
    SmallSet<Value *, 8> Visited;
    return {getRangeFromInitValue(CmpLHS, Diff, Visited), IV};
  }
  return {nullptr, 0};
}

// This function does exactly what its name implies, and in addition
// calculates the difference between the cmp comparator and its base value.
CJSimpleRangeAnalysisPass::ConditionState
CJSimpleRangeAnalysisPass::findMostRecentConditionBr(BasicBlock *BB,
                                                     BasicBlock *Succ) {
  // multi prodecessor, handle this in the future.
  //      1   2
  //       \ / \
  //        3   4
  //        |
  //        5 (curr BB)
  //       / \
  //      6   7
  while (BB->getSingleSuccessor()) {
    // BB is not which we want, just seach above.
    auto *Pred = BB->getSinglePredecessor();
    // Must be single, not unique.
    if (!Pred)
      return {nullptr, APInt(BIT_WIDTH_64, 0),
              CmpInst::Predicate::BAD_ICMP_PREDICATE, ConditionResult::UNKNOWN,
              0};
    Succ = BB;
    BB = Pred;
  }
  APInt Diff(64, 0);
  auto *BI = dyn_cast<BranchInst>(BB->getTerminator());
  if (!BI || !isa<CmpInst>(BI->getCondition()))
    return {nullptr, Diff, CmpInst::Predicate::BAD_ICMP_PREDICATE,
            ConditionResult::UNKNOWN, 0};
  assert(BI->isConditional());
  auto [Base, IV] = checkAndGetConditionBaseDiff(BI, Diff);
  ConditionResult CR = BI->getSuccessor(0) == Succ ? ConditionResult::TRUE
                                                   : ConditionResult::FALSE;
  return {Base, Diff, cast<CmpInst>(BI->getCondition())->getPredicate(), CR,
          IV};
}

// Return Prev CR and Curr CR.
std::pair<ConstantRange, ConstantRange>
CJSimpleRangeAnalysisPass::getCRs(const ConditionState &Prev,
                                  const ConditionState &Curr) {
  auto UnsignedLessCheck = [](const ConditionState &CS, ConstantRange &CR) {
    if (CS.Pred == CmpInst::ICMP_ULT || CS.Pred == CmpInst::ICMP_ULE ||
        CS.Pred == CmpInst::FCMP_ULT || CS.Pred == CmpInst::FCMP_ULE)
      CR = ConstantRange(APInt(BIT_WIDTH_64, -CS.Diff.getSExtValue()),
                         CR.getUpper());
  };
  DenseMap<unsigned, CmpInst::Predicate> FP2Int = {
      {static_cast<unsigned>(CmpInst::FCMP_OLE), CmpInst::ICMP_SLE},
      {static_cast<unsigned>(CmpInst::FCMP_OLT), CmpInst::ICMP_SLT},
      {static_cast<unsigned>(CmpInst::FCMP_OGE), CmpInst::ICMP_SGE},
      {static_cast<unsigned>(CmpInst::FCMP_OGT), CmpInst::ICMP_SGT},
      {static_cast<unsigned>(CmpInst::FCMP_OEQ), CmpInst::ICMP_EQ},
      {static_cast<unsigned>(CmpInst::FCMP_ONE), CmpInst::ICMP_NE}};
  int64_t PrevValue = Prev.CI - Prev.Diff.getSExtValue();
  int64_t CurrValue = Curr.CI - Curr.Diff.getSExtValue();
  // The pred types of prev and curr must be the same. For FP, since we are
  // guaranteed to be able to cast to an integer, we map FP pred to Integer pred
  // processing.
  if (Prev.Pred >= CmpInst::FCMP_OEQ && Prev.Pred <= CmpInst::FCMP_ONE &&
      Curr.Pred >= CmpInst::FCMP_OEQ && Curr.Pred <= CmpInst::FCMP_ONE) {
    ConstantRange CurrCR = ConstantRange::makeExactICmpRegion(
        FP2Int[Curr.Pred], APInt(64, CurrValue));
    UnsignedLessCheck(Curr, CurrCR);
    ConstantRange PrevCR = ConstantRange::getFull(64);
    if (BBValueCRs.count(Prev.BB) && BBValueCRs[Prev.BB].count(Prev.BV)) {
      PrevCR = Prev.PredCR == ConditionResult::FALSE
                   ? BBValueCRs[Prev.BB][Prev.BV].FCR
                   : BBValueCRs[Prev.BB][Prev.BV].TCR;
    } else {
      PrevCR = ConstantRange::makeExactICmpRegion(
          FP2Int[Prev.Pred], APInt(BIT_WIDTH_64, PrevValue));
      UnsignedLessCheck(Prev, PrevCR);
      if (Prev.PredCR == ConditionResult::FALSE)
        PrevCR = PrevCR.inverse();
    }
    return {PrevCR, CurrCR};
  }
  if (Prev.Pred >= CmpInst::ICMP_EQ && Prev.Pred <= CmpInst::ICMP_SLE &&
      Curr.Pred >= CmpInst::ICMP_EQ && Curr.Pred <= CmpInst::ICMP_SLE) {
    ConstantRange CurrCR = ConstantRange::makeExactICmpRegion(
        Curr.Pred, APInt(BIT_WIDTH_64, CurrValue));
    UnsignedLessCheck(Curr, CurrCR);
    ConstantRange PrevCR = ConstantRange::getFull(64);
    if (BBValueCRs.count(Prev.BB) && BBValueCRs[Prev.BB].count(Prev.BV)) {
      PrevCR = Prev.PredCR == ConditionResult::FALSE
                   ? BBValueCRs[Prev.BB][Prev.BV].FCR
                   : BBValueCRs[Prev.BB][Prev.BV].TCR;
    } else {
      PrevCR = ConstantRange::makeExactICmpRegion(
          Prev.Pred, APInt(BIT_WIDTH_64, PrevValue));
      UnsignedLessCheck(Prev, PrevCR);
      if (Prev.PredCR == ConditionResult::FALSE)
        PrevCR = PrevCR.inverse();
    }

    return {PrevCR, CurrCR};
  }

  return {ConstantRange::getFull(64), ConstantRange::getFull(64)};
}

void CJSimpleRangeAnalysisPass::rewriteUses(
    BasicBlock *BB, BasicBlock *NewBB,
    DenseMap<Instruction *, Instruction *> &InstMapping) {
  SmallVector<Use *, 8> RewriteInsts;
  for (auto BI = BB->begin(), BE = std::prev(BB->end()); BI != BE; ++BI) {
    for (auto &U : BI->uses()) {
      auto *User = dyn_cast<Instruction>(U.getUser());
      if (User && User->getParent() != BB)
        RewriteInsts.push_back(&U);
    }
    SSAUpdater SSAUpdate;
    SSAUpdate.Initialize(BI->getType(), BI->getName());
    SSAUpdate.AddAvailableValue(BB, &*BI);
    SSAUpdate.AddAvailableValue(NewBB, InstMapping[&*BI]);
    while (!RewriteInsts.empty())
      SSAUpdate.RewriteUse(*RewriteInsts.pop_back_val());
  }
}

// The initial state is Pred->BB->Succ, after this function, the state is
// Pred->New->Succ.
void CJSimpleRangeAnalysisPass::createJumpBB(BasicBlock *Pred, BasicBlock *BB,
                                             BasicBlock *Succ) {
  BasicBlock *NewBB = BasicBlock::Create(
      BB->getContext(), BB->getName() + ".split", BB->getParent(), BB);
  DenseMap<Instruction *, Instruction *> InstMapping;
  auto BI = BB->begin();
  auto BE = std::prev(BB->end());
  // Clone phi node of BB into NewBB.
  for (; PHINode *PN = dyn_cast<PHINode>(BI); ++BI) {
    auto *NewPN =
        PHINode::Create(PN->getType(), 1, PN->getName() + ".split", NewBB);
    NewPN->addIncoming(PN->getIncomingValueForBlock(Pred), Pred);
    InstMapping[PN] = NewPN;
  }
  // Clone other instructions. Some instructions may be redundant, which will be
  // removed in later pass.
  for (; BI != BE; ++BI) { // The last instruction is terminator.
    Instruction *NewI = BI->clone();
    NewBB->getInstList().push_back(NewI);
    InstMapping[&*BI] = NewI;
    // Update operand of NewI.
    for (unsigned I = 0, E = BI->getNumOperands(); I != E; ++I) {
      auto *Inst = dyn_cast<Instruction>(BI->getOperand(I));
      if (Inst == nullptr) {
        continue;
      }
      auto Iter = InstMapping.find(Inst);
      if (Iter != InstMapping.end())
        NewI->setOperand(I, Iter->second);
    }
  }
  // Create terminator instruction of NewBB, and add incoming value of phi in
  // Succ.
  BranchInst::Create(Succ, NewBB);
  for (auto &PN : Succ->phis()) {
    Value *V = PN.getIncomingValueForBlock(BB);
    if (auto *I = dyn_cast<Instruction>(V)) {
      auto Iter = InstMapping.find(I);
      if (Iter != InstMapping.end())
        V = Iter->second;
    }
    PN.addIncoming(V, NewBB);
  }
  // Last, update terminator of Pred, jump to NewBB instead of BB. This must be
  // done at the end, because doing it early will cause the phi information in
  // the BB to be lost.
  auto *TI = Pred->getTerminator();
  for (unsigned I = 0, E = TI->getNumSuccessors(); I != E; ++I) {
    if (TI->getSuccessor(I) == BB) {
      BB->removePredecessor(Pred, true); // Must be true, rewrite need it.
      TI->setSuccessor(I, NewBB);
    }
  }
  // Rewrite uses of instructions in BB outside.
  rewriteUses(BB, NewBB, InstMapping);
  // Do a quick simplify, delete dead instruction.
  SimplifyInstructionsInBlock(NewBB);
}

// return
//  - 0: condition of curr is always false
//  - 1: condition of curr is always true
//  - 2: condition of curr is unknown
CJSimpleRangeAnalysisPass::ConditionResult
CJSimpleRangeAnalysisPass::getConditionResult(const ConditionState &Prev,
                                              const ConditionState &Curr) {
  // Optimization is possible only when the base of the compared value is the
  // same.
  if (Prev.BV != Curr.BV)
    return ConditionResult::UNKNOWN;
  auto [PrevCR, CurrCR] = getCRs(Prev, Curr);
  assert(Prev.PredCR != ConditionResult::UNKNOWN);
  if (PrevCR.isFullSet() || CurrCR.isFullSet())
    return ConditionResult::UNKNOWN;
  if (CurrCR.contains(PrevCR))
    return ConditionResult::TRUE;
  if (CurrCR.inverse().contains(PrevCR))
    return ConditionResult::FALSE;
  return ConditionResult::UNKNOWN;
}

// For a BB with only one precursor, record the value range info of the BB
// exit. The BB with multiple predecessors and multiple successors is not
// processed because there are too many combinations.
void CJSimpleRangeAnalysisPass::updateBBValueCRs(
    BasicBlock *BB, MapVector<BasicBlock *, ConditionState> &PredInfo,
    ConditionState &CurrCS) {
  auto *Single = BB->getSinglePredecessor();
  if (!Single)
    return;
  for (auto &[PredBB, CS] : PredInfo) {
    if (!CS.BV || CurrCS.BV != CS.BV || PredBB != Single)
      continue;
    auto [PredCR, CurrCR] = getCRs(CS, CurrCS);
    if (PredCR.isFullSet() || CurrCR.isFullSet())
      return;
    BBValueCRs[BB][CurrCS.BV].TCR = CurrCR.intersectWith(PredCR);
    BBValueCRs[BB][CurrCS.BV].FCR = CurrCR.inverse().intersectWith(PredCR);
  }
  return;
}

bool CJSimpleRangeAnalysisPass::processBasicBlock(BasicBlock &BB) {
  auto *BI = dyn_cast<BranchInst>(BB.getTerminator());
  if (!BI || BI->isUnconditional())
    return false;
  // Loop header can not be jumped.
  if (auto *Loop = LI->getLoopFor(&BB))
    if (Loop->getHeader() == &BB)
      return false;
  APInt Diff(64, 0);
  // Get base value and difference of cmp lhs, and constant integer of cmp rhs.
  auto [BV, IV] = checkAndGetConditionBaseDiff(BI, Diff);
  if (!BV)
    return false;
  assert(isa<CmpInst>(BI->getCondition()) &&
         "Base is valid, and condition must be Cmp");
  auto *Cmp = cast<CmpInst>(BI->getCondition());
  CmpInst::Predicate Pred = Cmp->getPredicate();
  // Traverse each edge that reaches BB through conditional br and check
  // whether the condition of BB is true for each edge.
  //            bb0 (we find this basic block)
  //           /  \
  //          bb1  bb2 (maybe create an edge from bb2 to bb5)
  //           \  /
  //            bb3 (BB)
  //           /  \
  //         bb4  bb5
  MapVector<BasicBlock *, ConditionState> PredInfo;
  for (auto *PredBB : predecessors(&BB)) {
    auto *L = LI->getLoopFor(PredBB);
    if (L && L->isLoopLatch(PredBB)) // Latch may refresh all values in Loop.
      continue;
    auto TP = findMostRecentConditionBr(PredBB, &BB);
    TP.BB = PredBB;
    PredInfo[PredBB] = TP;
  }
  bool Changed = false;
  ConditionState CurrCS = {BV, Diff, Pred, ConditionResult::UNKNOWN, IV, &BB};
  for (auto &[PredBB, CS] : PredInfo) {
    if (!CS.BV)
      continue;
    ConditionResult CR = getConditionResult(CS, CurrCS);
    if (CR == ConditionResult::UNKNOWN)
      continue;
    createJumpBB(PredBB, &BB, BI->getSuccessor(static_cast<unsigned>(CR) == 0));
    Changed = true;
  }
  updateBBValueCRs(&BB, PredInfo, CurrCS);
  return Changed;
}

// %0 = fadd double %n, -2.000000e+00
// %1 = fadd double %0, -2.000000e+00
// => %1 = fadd double %n, -4.000000e+00
bool CJSimpleRangeAnalysisPass::simplifyBinaryInst(Function &F) {
  bool Changed = false;
  SmallMapVector<Instruction *, std::pair<Value *, int64_t>, 8> DeadInsts;
  // 0: nsw, 1: nuw
  SmallMapVector<Instruction *, std::pair<bool, bool>, 8> NswNuwInfo;
  for (auto &I : instructions(F)) {
    if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      APInt Diff(64, 0);
      SmallSet<Value *, 8> Visited;
      Value *V = getRangeFromInitValue(&I, Diff, Visited);
      if (V && V != &I) {
        DeadInsts[&I] = {V, Diff.getSExtValue()};
        if (BO->getType()->isIntegerTy())
          NswNuwInfo[&I] = {BO->hasNoSignedWrap(), BO->hasNoUnsignedWrap()};
      }
    }
  }
  for (auto [I, Info] : DeadInsts) {
    auto OP = I->getOpcode() == Instruction::FAdd ||
                      I->getOpcode() == Instruction::FSub
                  ? Instruction::FAdd
                  : Instruction::Add;
    auto *CV = I->getOpcode() == Instruction::FAdd ||
                       I->getOpcode() == Instruction::FSub
                   ? ConstantFP::get(Info.first->getType(),
                                     static_cast<double>(Info.second))
                   : ConstantInt::get(Info.first->getType(), Info.second);
    auto *New = BinaryOperator::Create(OP, Info.first, CV, "", I);
    if (New->getType()->isIntegerTy()) {
      New->setHasNoSignedWrap(NswNuwInfo[I].first);
      if (Info.second >= 0) // add nuw %x, -1 -> -1 by instcombine.
        New->setHasNoUnsignedWrap(NswNuwInfo[I].second);
    }
    I->replaceAllUsesWith(New);
    I->eraseFromParent();
    Changed = true;
  }
  return Changed;
}

// Dead blocks may cause pass to enter an infinite loop in
// getRangeFromInitValue, must be removed before analysis. For example, a dead
// loop contains the instruction %n = add i64%n, 1.
static bool removeDeadBlocks(Function &F, DominatorTree *DT) {
  DomTreeUpdater DTU(DT, DomTreeUpdater::UpdateStrategy::Lazy);
  return removeUnreachableBlocks(F, DT ? &DTU : nullptr);
}

bool CJSimpleRangeAnalysisPass::runImpl(Function &F, LoopInfo &LI,
                                        DominatorTree &DT) {
  if (!EnableCJSimpleRA)
    return false;
  this->LI = &LI;
  bool Changed;
  bool EverChanged = false;
  EverChanged |= removeDeadBlocks(F, &DT);
  EverChanged |= simplifyBinaryInst(F);
  do {
    Changed = false;
    for (auto &BB : F) // Simplify RangeAnalysis
      Changed |= processBasicBlock(BB);
    EverChanged |= Changed;
  } while (Changed);
  BBValueCRs.clear(); // Must clear it.
  return EverChanged;
}

PreservedAnalyses CJSimpleRangeAnalysisPass::run(Function &F,
                                                 FunctionAnalysisManager &AM) {
  auto &LI = AM.getResult<LoopAnalysis>(F);
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  bool Changed = runImpl(F, LI, DT);
  if (Changed)
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

