//===- CJLoopFloatOpt.cpp ---------------------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This pass optimizes the scene where a top-level loop only involves
// straightforward floating-point computations.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJLoopFloatOpt.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/InitializePasses.h"
#include "llvm/Transforms/Scalar.h"

using namespace llvm;

static double convertToDouble(Value *V) {
  ConstantFP *FPVal = cast<ConstantFP>(V);
  const APFloat &APF = FPVal->getValueAPF();
  return APF.convertToDouble();
}

static int64_t convertToInt64(Value *V) {
  return static_cast<int64_t>(convertToDouble(V));
}

static ConstantInt *getConstIntByDouble(Value *V) {
  // 64: 64bit
  APInt IntVal(64, convertToInt64(V), true);
  return ConstantInt::get(V->getContext(), IntVal);
}

static bool isConstantDouble(Value *V) {
  if (!V->getType()->isDoubleTy()) {
    return false;
  }
  ConstantFP *FPVal = dyn_cast<ConstantFP>(V);
  if (!FPVal) {
    return false;
  }
  const APFloat &APF = FPVal->getValueAPF();
  if (!APF.isIEEE() || !APF.isFinite()) {
    return false;
  }
  return true;
}

static bool isNoDecimalDouble(Value *V) {
  if (!isConstantDouble(V)) {
    return false;
  }
  ConstantFP *FPVal = cast<ConstantFP>(V);
  const APFloat &APF = FPVal->getValueAPF();
  double doubleVal = APF.convertToDouble();
  if (doubleVal > (double)INT32_MAX || doubleVal < (double)INT32_MIN) {
    return false;
  }
  return APF.isInteger();
}

static constexpr char fpExceptionBbName[] = "fp.convert.exception";

static bool checkFPUse(PHINode *PHI, Loop *L, Value *LatchVal) {
  bool hasSimpleFloatComputation = false;
  for (auto Use : PHI->users()) {
    auto *Inst = dyn_cast<Instruction>(Use);
    if (!Inst) {
      return false;
    }
    if (!L->contains(Inst->getParent())) {
      return false;
    }
    switch (Inst->getOpcode()) {
    case Instruction::Call: {
      auto *CI = dyn_cast<IntrinsicInst>(Inst);
      if (!CI || (CI->getIntrinsicID() != Intrinsic::fptosi_sat &&
                  CI->getIntrinsicID() != Intrinsic::fptoui_sat)) {
        return false;
      }
      break;
    }
    case Instruction::FPToSI: {
      if (!Inst->getType()->isIntegerTy(64)) { // 64: 64bit integer
        return false;
      }
      auto *CI = dyn_cast<IntrinsicInst>(Inst->getNextNode());
      if (!CI || CI->getIntrinsicID() != Intrinsic::cj_get_fp_state) {
        return false;
      }
      auto *AndInst = dyn_cast<Instruction>(CI->getNextNode());
      if (!AndInst || AndInst->getOpcode() != Instruction::And) {
        return false;
      }
      auto *CmpInst = dyn_cast<ICmpInst>(AndInst->getNextNode());
      if (!CmpInst) {
        return false;
      }
      auto *BRInst = dyn_cast<BranchInst>(CmpInst->getNextNode());
      if (!BRInst || !BRInst->isConditional()) {
        return false;
      }
      auto SuccBBName0 = BRInst->getSuccessor(0)->getName();
      auto SuccBBName1 = BRInst->getSuccessor(1)->getName();
      if (!SuccBBName0.startswith(fpExceptionBbName) &&
          !SuccBBName1.startswith(fpExceptionBbName)) {
        return false;
      }
      break;
    }
    case Instruction::BitCast: {
      if (!Inst->getParent()->getName().startswith(fpExceptionBbName)) {
        return false;
      }
      break;
    }
    case Instruction::FCmp: {
      auto BBName = Inst->getParent()->getName();
      if (!BBName.startswith("not.inf.nan") &&
          !BBName.startswith("lower.bound.ok")) {
        return false;
      }
      break;
    }
    case Instruction::FAdd:
    case Instruction::FSub: {
      // First operand must be PHI.
      if (Inst->getOperand(0) != PHI) {
        return false;
      }
      // Only support single fadd or fsub.
      if (Inst != LatchVal) {
        return false;
      }
      // Another operand can be constantFP or selectVal.
      if (auto *SelectI = dyn_cast<SelectInst>(Inst->getOperand(1))) {
        if (!isNoDecimalDouble(SelectI->getTrueValue()) ||
            !isNoDecimalDouble(SelectI->getFalseValue())) {
          return false;
        }
      } else if (isa<ConstantFP>(Inst->getOperand(1))) {
        if (!isNoDecimalDouble(Inst->getOperand(1))) {
          return false;
        }
      } else {
        return false;
      }
      hasSimpleFloatComputation = true;
      break;
    }
    default:
      return false;
    }
  }
  return hasSimpleFloatComputation;
}

static bool checkInt64Bound(unsigned Opcode, double Base, double Num,
                            uint64_t LoopCount) {
  double Val = 0;
  if (Opcode == Instruction::FAdd) {
    Val = Base + Num * LoopCount;
  } else {
    Val = Base - Num * LoopCount;
  }
  if (!std::isfinite(Val)) {
    return false;
  }
  return Val < (double)INT64_MAX && Val > (double)INT64_MIN;
}

static bool checkIntOverflow(PHINode *PHI, Loop *L, uint64_t LoopCount,
                             Value *InitialVal) {
  double Base = convertToDouble(InitialVal);
  for (auto Use : PHI->users()) {
    auto *Inst = cast<Instruction>(Use);
    unsigned Opcode = Inst->getOpcode();
    if (Opcode == Instruction::FAdd || Opcode == Instruction::FSub) {
      auto *SelectI = dyn_cast<SelectInst>(Inst->getOperand(1));
      if (SelectI) {
        double TrueValue = convertToDouble(SelectI->getTrueValue());
        double FalseValue = convertToDouble(SelectI->getFalseValue());
        return checkInt64Bound(Opcode, Base, TrueValue, LoopCount) &&
               checkInt64Bound(Opcode, Base, FalseValue, LoopCount);
      }
      double Val = convertToDouble(Inst->getOperand(1));
      return checkInt64Bound(Opcode, Base, Val, LoopCount);
    }
  }
  return false;
}

static PHINode *replacePHI(PHINode *PHI, Value *InitialVal) {
  ConstantInt *Val = getConstIntByDouble(InitialVal);
  IRBuilder<> Builder(PHI);
  Type *Int64Type = Type::getInt64Ty(PHI->getContext());
  PHINode *NewPHI = Builder.CreatePHI(Int64Type, 2);
  if (isa<ConstantFP>(PHI->getIncomingValue(0))) {
    NewPHI->addIncoming(Val, PHI->getIncomingBlock(0));
  } else {
    NewPHI->addIncoming(Val, PHI->getIncomingBlock(1));
  }
  return NewPHI;
}

static void replaceFloatToInt(PHINode *PHI,
                              SetVector<Instruction *> &RemoveInsts,
                              Value *InitialVal) {
  PHINode *NewPHI = replacePHI(PHI, InitialVal);
  Value *ComputationResult = nullptr;

  for (auto Use : PHI->users()) {
    auto *Inst = cast<Instruction>(Use);
    switch (Inst->getOpcode()) {
    case Instruction::Call:
      RemoveInsts.insert(Inst); // fptosi.sat or fptoui.sat
      Inst->replaceAllUsesWith(NewPHI);
      break;
    case Instruction::FPToSI: {
      auto *CI = Inst->getNextNode();
      auto *AndI = CI->getNextNode();
      auto *CmpI = AndI->getNextNode();
      auto *BR = cast<BranchInst>(CmpI->getNextNode());
      RemoveInsts.insert(BR);   // BRInst
      RemoveInsts.insert(CmpI); // CmpInst
      RemoveInsts.insert(AndI); // AndInst
      RemoveInsts.insert(CI);   // GetFpState
      RemoveInsts.insert(Inst); // FPToSI
      IRBuilder<> Builder(BR);
      if (BR->getSuccessor(0)->getName().startswith(fpExceptionBbName)) {
        Builder.CreateBr(BR->getSuccessor(1));
      } else {
        Builder.CreateBr(BR->getSuccessor(0));
      }
      Inst->replaceAllUsesWith(NewPHI);
      break;
    }
    case Instruction::BitCast:
    case Instruction::FCmp:
      // remove later by DCE
      break;
    case Instruction::FAdd:
    case Instruction::FSub: {
      Value *Val = nullptr;
      IRBuilder<> Builder(Inst);
      auto *SelectI = dyn_cast<SelectInst>(Inst->getOperand(1));
      if (SelectI) {
        ConstantInt *TrueValue = getConstIntByDouble(SelectI->getTrueValue());
        ConstantInt *FalseValue = getConstIntByDouble(SelectI->getFalseValue());
        Val = Builder.CreateSelect(SelectI->getCondition(), TrueValue,
                                   FalseValue);
      } else {
        Val = getConstIntByDouble(Inst->getOperand(1));
      }
      if (Inst->getOpcode() == Instruction::FAdd) {
        ComputationResult = Builder.CreateAdd(NewPHI, Val);
      } else {
        ComputationResult = Builder.CreateSub(NewPHI, Val);
      }
      auto *SItoFPInst =
          Builder.CreateSIToFP(ComputationResult, Inst->getType());
      Inst->replaceAllUsesWith(SItoFPInst);
      RemoveInsts.insert(Inst);
      if (SelectI)
        RemoveInsts.insert(SelectI);
      break;
    }
    default:
      assert(false && "Unsupported instr opcode in CJLoopFloatOpt");
      return;
    }
  }
  if (isa<ConstantFP>(PHI->getIncomingValue(0))) {
    NewPHI->addIncoming(ComputationResult, PHI->getIncomingBlock(1));
  } else {
    NewPHI->addIncoming(ComputationResult, PHI->getIncomingBlock(0));
  }
}

static void eraseInsts(SetVector<Instruction *> &RemoveInsts) {
  if (!RemoveInsts.empty()) {
    for (auto *I : RemoveInsts) {
      I->eraseFromParent();
    }
  }
  RemoveInsts.clear();
}

/**
 * Optimize the scene where a top-level loop only involves
 * straightforward floating-point computations without any decimal operations.
 *
 * for example:
 *
 * ## before opt
 * ===========================================================
 * loop.header:
 * %fpVal = phi double [ 1.000000e+00, %preheader ], [ %faddVal, %loop.body ]
 * ...
 * %26 = fptosi double %fpVal to i64
 * %27 = call i64 @llvm.cj.get.fp.state.i64(i64 %26)
 * %28 = and i64 %27, 1
 * %29 = icmp eq i64 %28, 0
 * br i1 %29, label %loop.body, label %fp.convert.exception
 *
 * fp.convert.exception:
 * %30 = bitcast double %fpVal to i64
 *
 * not.inf.nan:
 * %f2i.lt.min = fcmp ogt double %fpVal, 0xC3E0000000000001
 *
 * lower.bound.ok:
 * %f2i.gt.max = fcmp olt double %fpVal, 0x43E0000000000000
 *
 * loop.body:
 * %selectVal = select i1 %icmpeq, double 1.000000e+00, double 2.000000e+00
 * %faddVal = fadd double %fpVal, %selectVal
 * %exitcond.not = icmp eq i64 %25, 3000000
 * br i1 %exitcond.not, label %exit, label %loop.header
 *
 * ## after opt
 * ===========================================================
 * loop.header:
 * %25 = phi i64 [ 1, %preheader ], [ %34, %loop.body ]
 * br label %loop.body
 *
 * loop.body:
 * %33 = select i1 %icmpeq, i64 1, i64 2
 * %34 = add i64 %25, %33
 * %35 = sitofp i64 %34 to double
 * %exitcond.not = icmp eq i64 %26, 3000000
 * br i1 %exitcond.not, label %exit, label %loop.header
 */
bool loopFPComputationOpt(Function &F, LoopInfo &LI, ScalarEvolution &SE) {
  bool Changed = false;
  SetVector<Instruction *> RemoveInsts;
  for (Loop *L : LI) {
    if (!L->getSubLoops().empty()) {
      continue;
    }

    const SCEV *MaxTrips = SE.getConstantMaxBackedgeTakenCount(L);
    if (isa<SCEVCouldNotCompute>(MaxTrips)) {
      return false;
    }

    BasicBlock *HeaderBB = L->getHeader();
    for (Instruction &Inst : *HeaderBB) {
      if (auto *PHI = dyn_cast<PHINode>(&Inst)) {
        // 2: only support two coming BB
        if (PHI->getNumIncomingValues() != 2) {
          continue;
        }
        Value *LatchVal = nullptr;
        Value *InitialVal = nullptr;
        BasicBlock *Pred0 = PHI->getIncomingBlock(0);
        BasicBlock *Pred1 = PHI->getIncomingBlock(1);
        // Firstly: match the scene.
        if (isNoDecimalDouble(PHI->getIncomingValue(0)) &&
            !L->contains(Pred0) && L->contains(Pred1)) {
          InitialVal = PHI->getIncomingValue(0);
          LatchVal = PHI->getIncomingValue(1);
        } else if (isNoDecimalDouble(PHI->getIncomingValue(1)) &&
                   !L->contains(Pred1) && L->contains(Pred0)) {
          InitialVal = PHI->getIncomingValue(1);
          LatchVal = PHI->getIncomingValue(0);
        } else {
          continue;
        }

        // Check the use of FP.
        if (!checkFPUse(PHI, L, LatchVal)) {
          continue;
        }

        // Secondly: check for integer Overflow.
        APInt MaxCount = SE.getUnsignedRange(MaxTrips).getUnsignedMax();
        if (!checkIntOverflow(PHI, L, MaxCount.getZExtValue() + 1,
                              InitialVal)) {
          continue;
        }

        // Thirdly: replace the fp instruction.
        replaceFloatToInt(PHI, RemoveInsts, InitialVal);
        Changed = true;
      } else {
        break;
      }
    }
  }
  eraseInsts(RemoveInsts);
  return Changed;
}

static bool checkLoopEarlyOut(Loop *L) {
  for (BasicBlock *BB : L->getBlocks()) {
    for (Instruction &I : *BB) {
      if (I.mayThrow() || !I.willReturn()) {
        return true;
      }
    }
  }
  return false;
}

// Main optimization concept: fadd(PHI, >=PHI) >= fmul(PHI, 2)
// Locate the fadd instruction and examine its operands:
// If the first operand is the PHI node and the second operand is greater than
// or equal to the PHI node, the minimum value of fadd is equivalent to
// fmul(PHI, 2).
static bool checkLatchInst(Instruction *LatchInst, PHINode *PHI) {
  // Only support FAdd Inst.
  if (LatchInst->getOpcode() != Instruction::FAdd) {
    return false;
  }

  Value *LatchInstOp = nullptr;
  // One of FAdd operands must be PHI.
  if (LatchInst->getOperand(0) == PHI) {
    LatchInstOp = LatchInst->getOperand(1);
  } else if (LatchInst->getOperand(1) == PHI) {
    LatchInstOp = LatchInst->getOperand(0);
  } else {
    return false;
  }

  // One of FAdd operands must be SelectInst or FpInst.
  Instruction *OpInst = nullptr;
  auto *SelectI = dyn_cast<SelectInst>(LatchInstOp);
  if (SelectI) {
    // One of SelectInst operands must be PHI.
    Value *SelectOp = nullptr;
    if (SelectI->getTrueValue() == PHI) {
      SelectOp = SelectI->getFalseValue();
    } else if (SelectI->getFalseValue() == PHI) {
      SelectOp = SelectI->getTrueValue();
    } else {
      return false;
    }

    OpInst = dyn_cast<BinaryOperator>(SelectOp);
  } else {
    OpInst = dyn_cast<BinaryOperator>(LatchInstOp);
  }

  if (!OpInst) {
    return false;
  }

  // One of OpInst operands must be PHI.
  Value *InstOp = nullptr;
  if (OpInst->getOperand(0) == PHI) {
    InstOp = OpInst->getOperand(1);
  } else if (OpInst->getOperand(1) == PHI) {
    InstOp = OpInst->getOperand(0);
  } else {
    return false;
  }

  if (!isConstantDouble(InstOp)) {
    return false;
  }

  double doubleVal = convertToDouble(InstOp);

  // Make sure the result of OpInst is greater than or equal to PHI.
  switch (OpInst->getOpcode()) {
  case Instruction::FAdd:
    return doubleVal >= 0;
  case Instruction::FSub:
    return OpInst->getOperand(0) == PHI && doubleVal <= 0;
  case Instruction::FMul:
    return doubleVal >= 1;
  case Instruction::FDiv:
    return OpInst->getOperand(0) == PHI && doubleVal > 0 && doubleVal <= 1;
  default:
    return false;
  }
}

static bool checkFPInfinity(uint64_t LoopCount, Value *InitialVal) {
  double Val = convertToDouble(InitialVal);
  for (uint64_t I = 0; I < LoopCount; I++) {
    Val = Val * 2; // 2: multiplier
  }
  return std::isinf(Val);
}

static void replaceResultToInf(Instruction *LatchInst, Loop *L) {
  auto *New = ConstantFP::getInfinity(
      Type::getDoubleTy(LatchInst->getContext()), false);
  LatchInst->replaceUsesWithIf(New, [L](Use &U) {
    auto *I = dyn_cast<Instruction>(U.getUser());
    return !L->contains(I->getParent());
  });
}

/**
 * If the result of a floating-point computation is infinity, we replace it
 * with infinity immediately after the loop.
 *
 * for example:
 *
 * before opt:
 * ===========================================================
 * loop.header:
 * %f3.04 = phi double [ 1.000000e+00, %bb59 ], [ %f3.1, %bb64 ]
 * ...
 *
 * loop.body:
 * %2 = fmul double %f3.04, 2.000000e+00
 * %f3.0.pn = select i1 %icmpeq, double %f3.04, double %2
 * %f3.1 = fadd double %f3.04, %f3.0.pn
 * ...
 *
 * loop.exit:
 * call void @"_ZN8std.core7printlnEd"(double %f3.1)
 *
 * after opt:
 * ===========================================================
 * loop.header:
 * same as before
 *
 * loop.body:
 * same as before
 *
 * loop.exit:
 * call void @"_ZN8std.core7printlnEd"(double 0x7FF0000000000000) // infinity
 */
bool loopFPInfinityOpt(Function &F, LoopInfo &LI, PostDominatorTree &PDT,
                       ScalarEvolution &SE) {
  bool Changed = false;
  for (Loop *L : LI) {
    if (!L->getSubLoops().empty())
      continue;

    if (checkLoopEarlyOut(L))
      continue;

    const SCEV *MaxTrips = SE.getConstantMaxBackedgeTakenCount(L);
    if (isa<SCEVCouldNotCompute>(MaxTrips))
      return false;

    BasicBlock *HeaderBB = L->getHeader();
    for (Instruction &Inst : *HeaderBB) {
      if (auto *PHI = dyn_cast<PHINode>(&Inst)) {
        // 2: only support two coming BB
        if (PHI->getNumIncomingValues() != 2)
          continue;

        Value *LatchVal = nullptr;
        Value *InitialVal = nullptr;
        BasicBlock *LatchBB = nullptr;
        BasicBlock *Pred0 = PHI->getIncomingBlock(0);
        BasicBlock *Pred1 = PHI->getIncomingBlock(1);
        // Firstly: match the scene.
        if (isConstantDouble(PHI->getIncomingValue(0)) && !L->contains(Pred0) &&
            L->contains(Pred1)) {
          InitialVal = PHI->getIncomingValue(0);
          LatchVal = PHI->getIncomingValue(1);
          LatchBB = PHI->getIncomingBlock(1);
        } else if (isConstantDouble(PHI->getIncomingValue(1)) &&
                   !L->contains(Pred1) && L->contains(Pred0)) {
          InitialVal = PHI->getIncomingValue(1);
          LatchVal = PHI->getIncomingValue(0);
          LatchBB = PHI->getIncomingBlock(0);
        } else {
          continue;
        }

        if (convertToDouble(InitialVal) < 0)
          continue;

        auto *LatchInst = dyn_cast<Instruction>(LatchVal);
        if (!LatchInst)
          continue;

        // Make sure LatchInst will be executed in every iteration.
        if (!PDT.dominates(LatchInst->getParent(), HeaderBB))
          continue;

        if (!PDT.dominates(LatchBB, LatchInst->getParent()))
          continue;

        // Check if LatchInst is equal to fmul(PHI, 2).
        if (!checkLatchInst(LatchInst, PHI))
          continue;

        // Secondly: check fp is Infinity.
        APInt MaxCount = SE.getUnsignedRange(MaxTrips).getUnsignedMax();
        if (!checkFPInfinity(MaxCount.getZExtValue() + 1, InitialVal))
          continue;

        // Thirdly: replace result of FAdd to Infinity.
        replaceResultToInf(LatchInst, L);
        Changed = true;
      }
    }
  }
  return Changed;
}

PreservedAnalyses CJLoopFloatOpt::run(Function &F,
                                      FunctionAnalysisManager &FAM) const {
  LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);
  ScalarEvolution &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
  PostDominatorTree &PDT = FAM.getResult<PostDominatorTreeAnalysis>(F);

  bool Changed = false;
  Changed |= loopFPComputationOpt(F, LI, SE);
  Changed |= loopFPInfinityOpt(F, LI, PDT, SE);
  if (Changed)
    return PreservedAnalyses::none();

  return PreservedAnalyses::all();
}

