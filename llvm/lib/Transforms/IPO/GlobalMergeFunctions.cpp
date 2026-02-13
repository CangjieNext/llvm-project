//===- GlobalMergeFunctions.cpp - Merge functions with const differences --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass finds functions with identical structure that differ only in a
// subset of constant operands, then outlines that common body into an internal
// merged function and rewrites originals into thunks.
//
// This is a local (single-module) GMF-lite implementation: no LTO index and no
// CGData are required.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/GlobalMergeFunctions.h"
#include "llvm/Transforms/IPO.h"
#include <cassert>
#include <string>
#include <utility>

#define DEBUG_TYPE "global-merge-func"

using namespace llvm;

STATISTIC(NumMergedFunctions, "Number of functions converted to thunks");
STATISTIC(NumMergedGroups, "Number of merged function groups");
STATISTIC(NumAnalyzedFunctions, "Number of functions analyzed");
STATISTIC(NumEligibleFunctions, "Number of functions eligible for merging");

namespace {

struct OpLoc {
  unsigned InstIndex = 0;
  unsigned OperandIndex = 0;
};

struct CandidateFunction {
  Function *F = nullptr;
  SmallVector<Instruction *, 32> Instructions;
};

/// Returns true if the \p OpIdx operand of \p CI is the callee operand.
static bool isCalleeOperand(const CallBase *CI, unsigned OpIdx) {
  return &CI->getCalledOperandUse() == &CI->getOperandUse(OpIdx);
}

static bool canParameterizeCallOperand(const CallBase *CI, unsigned OpIdx) {
  if (CI->isInlineAsm())
    return false;

  Function *Callee =
      CI->getCalledOperand()
          ? dyn_cast_or_null<Function>(
                CI->getCalledOperand()->stripPointerCasts())
          : nullptr;
  if (Callee) {
    if (Callee->isIntrinsic())
      return false;
    StringRef Name = Callee->getName();
    // objc_msgSend stubs must be called and cannot have their address taken.
    if (Name.startswith("objc_msgSend$"))
      return false;
    // Calls to dtrace probes must generate unique patchpoints.
    if (Name.startswith("__dtrace"))
      return false;
  }

  if (isCalleeOperand(CI, OpIdx)) {
    // If the callee is already signed, we cannot add another ptrauth bundle.
    if (CI->getOperandBundle(LLVMContext::OB_ptrauth).hasValue())
      return false;
  } else {
    // ARC attached call targets must remain constants.
    if (CI->isOperandBundleOfType(LLVMContext::OB_clang_arc_attachedcall,
                                  OpIdx))
      return false;
  }
  return true;
}

/// Returns true if function \p F is eligible for merging.
static bool isEligibleFunction(Function *F) {
  ++NumAnalyzedFunctions;

  if (F->isDeclaration())
    return false;
  // Keep the lite version conservative for linkage semantics.
  if (!F->hasLocalLinkage())
    return false;
  if (F->getFunctionType()->isVarArg())
    return false;
  if (F->hasFnAttribute(Attribute::NoMerge) ||
      F->hasFnAttribute(Attribute::AlwaysInline))
    return false;
  if (F->getCallingConv() == CallingConv::SwiftTail)
    return false;
  if (!F->hasName())
    return false;

  // If the function contains musttail, parameterizing constants can invalidate
  // the musttail constraints.
  for (const BasicBlock &BB : *F) {
    if (BB.hasAddressTaken())
      return false;
    for (const Instruction &I : BB) {
      const CallBase *CB = dyn_cast<CallBase>(&I);
      if (CB && CB->isMustTailCall())
        return false;
    }
  }

  ++NumEligibleFunctions;
  return true;
}

static bool isEligibleInstructionForConstantSharing(const Instruction *I) {
  switch (I->getOpcode()) {
  case Instruction::Load:
  case Instruction::Store:
  case Instruction::Call:
  case Instruction::Invoke:
    return true;
  default:
    return false;
  }
}

static bool ignoreOp(const Instruction *I, unsigned OpIdx) {
  if (OpIdx >= I->getNumOperands())
    return false;
  if (!isEligibleInstructionForConstantSharing(I))
    return false;
  if (!isa<Constant>(I->getOperand(OpIdx)))
    return false;

  const CallBase *CB = dyn_cast<CallBase>(I);
  if (CB)
    return canParameterizeCallOperand(CB, OpIdx);

  return true;
}

static std::string getConstantText(const Constant *C) {
  std::string S;
  raw_string_ostream OS(S);
  C->printAsOperand(OS, true);
  return OS.str();
}

static bool constantsEqual(const Constant *LHS, const Constant *RHS) {
  if (LHS == RHS)
    return true;
  if (!LHS || !RHS)
    return false;
  if (LHS->getType() != RHS->getType())
    return false;
  return getConstantText(LHS) == getConstantText(RHS);
}

static Value *createCast(IRBuilder<> &Builder, Value *V, Type *DestTy) {
  Type *SrcTy = V->getType();
  if (SrcTy->isStructTy()) {
    assert(DestTy->isStructTy());
    assert(SrcTy->getStructNumElements() == DestTy->getStructNumElements());
    Value *Result = PoisonValue::get(DestTy);
    for (unsigned I = 0, E = SrcTy->getStructNumElements(); I != E; ++I) {
      Value *Element =
          createCast(Builder, Builder.CreateExtractValue(V, {I}),
                     DestTy->getStructElementType(I));
      Result = Builder.CreateInsertValue(Result, Element, {I});
    }
    return Result;
  }

  assert(!DestTy->isStructTy());
  if (SrcTy->isIntegerTy() && DestTy->isPointerTy())
    return Builder.CreateIntToPtr(V, DestTy);
  if (SrcTy->isPointerTy() && DestTy->isIntegerTy())
    return Builder.CreatePtrToInt(V, DestTy);
  return Builder.CreateBitCast(V, DestTy);
}

static void collectInstructions(Function &F,
                                SmallVectorImpl<Instruction *> &Instructions) {
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (!I.isDebugOrPseudoInst())
        Instructions.push_back(&I);
    }
  }
}

static void collectParameterizableLocations(const CandidateFunction &CF,
                                            SmallVectorImpl<OpLoc> &Locs) {
  for (unsigned InstIndex = 0, InstCount = CF.Instructions.size();
       InstIndex != InstCount; ++InstIndex) {
    Instruction *Inst = CF.Instructions[InstIndex];
    for (unsigned OpIdx = 0, E = Inst->getNumOperands(); OpIdx != E; ++OpIdx) {
      if (ignoreOp(Inst, OpIdx))
        Locs.push_back({InstIndex, OpIdx});
    }
  }
}

static bool areValuesEquivalent(
    const Value *LHS, const Value *RHS,
    const DenseMap<const Value *, const Value *> &ValueMap) {
  DenseMap<const Value *, const Value *>::const_iterator It = ValueMap.find(LHS);
  if (It != ValueMap.end())
    return It->second == RHS;

  const Constant *LC = dyn_cast<Constant>(LHS);
  const Constant *RC = dyn_cast<Constant>(RHS);
  if (LC || RC)
    return LC && RC && constantsEqual(LC, RC);

  return LHS == RHS;
}

static bool
areEquivalentIgnoringParameterizableConstants(const CandidateFunction &LHS,
                                              const CandidateFunction &RHS) {
  if (LHS.F->getFunctionType() != RHS.F->getFunctionType())
    return false;
  if (LHS.F->getCallingConv() != RHS.F->getCallingConv())
    return false;
  if (LHS.F->getAttributes() != RHS.F->getAttributes())
    return false;
  if (LHS.F->hasPersonalityFn() != RHS.F->hasPersonalityFn())
    return false;
  if (LHS.F->hasPersonalityFn() &&
      !constantsEqual(LHS.F->getPersonalityFn(), RHS.F->getPersonalityFn()))
    return false;
  if (LHS.F->hasGC() != RHS.F->hasGC())
    return false;
  if (LHS.F->hasGC() && LHS.F->getGC() != RHS.F->getGC())
    return false;
  if (LHS.F->size() != RHS.F->size())
    return false;
  if (LHS.Instructions.size() != RHS.Instructions.size())
    return false;

  DenseMap<const Value *, const Value *> ValueMap;

  Function::const_arg_iterator RArgIt = RHS.F->arg_begin();
  for (Function::const_arg_iterator LArgIt = LHS.F->arg_begin(),
                                    LArgEnd = LHS.F->arg_end();
       LArgIt != LArgEnd; ++LArgIt, ++RArgIt) {
    ValueMap[&*LArgIt] = &*RArgIt;
  }

  Function::const_iterator RBBIt = RHS.F->begin();
  for (Function::const_iterator LBBIt = LHS.F->begin(), LBBEnd = LHS.F->end();
       LBBIt != LBBEnd; ++LBBIt, ++RBBIt) {
    ValueMap[&*LBBIt] = &*RBBIt;
  }

  for (unsigned I = 0, E = LHS.Instructions.size(); I != E; ++I) {
    ValueMap[LHS.Instructions[I]] = RHS.Instructions[I];
  }

  for (unsigned I = 0, E = LHS.Instructions.size(); I != E; ++I) {
    const Instruction *LI = LHS.Instructions[I];
    const Instruction *RI = RHS.Instructions[I];

    DenseMap<const Value *, const Value *>::const_iterator ParentIt =
        ValueMap.find(LI->getParent());
    if (ParentIt == ValueMap.end() || ParentIt->second != RI->getParent())
      return false;

    if (!LI->isSameOperationAs(RI))
      return false;

    if (LI->getNumOperands() != RI->getNumOperands())
      return false;

    for (unsigned OpIdx = 0, OpEnd = LI->getNumOperands(); OpIdx != OpEnd;
         ++OpIdx) {
      Value *LOp = LI->getOperand(OpIdx);
      Value *ROp = RI->getOperand(OpIdx);

      bool LIgnore = ignoreOp(LI, OpIdx);
      bool RIgnore = ignoreOp(RI, OpIdx);
      if (LIgnore || RIgnore) {
        if (!(LIgnore && RIgnore))
          return false;
        if (!isa<Constant>(LOp) || !isa<Constant>(ROp))
          return false;
        continue;
      }

      if (!areValuesEquivalent(LOp, ROp, ValueMap))
        return false;
    }
  }

  return true;
}

static void clearFunctionBody(Function *F) {
  for (BasicBlock &BB : *F)
    BB.dropAllReferences();
  while (!F->empty())
    F->begin()->eraseFromParent();
}

static std::string createMergedFunctionName(Function &F) {
  std::string BaseName = F.getName().str() + ".Tgm";
  std::string NewName = BaseName;
  unsigned Suffix = 0;
  Module *M = F.getParent();
  while (M->getFunction(NewName) != nullptr)
    NewName = BaseName + "." + std::to_string(++Suffix);
  return NewName;
}

static Function *createMergedFunction(CandidateFunction &RootCF,
                                      ArrayRef<OpLoc> ParamLocs) {
  Function *Root = RootCF.F;
  Module *M = Root->getParent();

  FunctionType *OrigTy = Root->getFunctionType();
  SmallVector<Type *, 8> ParamTypes(OrigTy->param_begin(), OrigTy->param_end());
  for (unsigned I = 0, E = ParamLocs.size(); I != E; ++I) {
    const OpLoc &Loc = ParamLocs[I];
    Constant *C = cast<Constant>(
        RootCF.Instructions[Loc.InstIndex]->getOperand(Loc.OperandIndex));
    ParamTypes.push_back(C->getType());
  }

  FunctionType *MergedTy =
      FunctionType::get(OrigTy->getReturnType(), ParamTypes, false);
  std::string NewName = createMergedFunctionName(*Root);

  Function *Merged = Function::Create(MergedTy, Root->getLinkage(),
                                      Root->getAddressSpace(), NewName);
  if (DISubprogram *SP = Root->getSubprogram())
    Merged->setSubprogram(SP);
  Merged->copyAttributesFrom(Root);
  if (auto EC = Root->getEntryCount())
    Merged->setEntryCount(*EC);
  Merged->setDLLStorageClass(GlobalValue::DefaultStorageClass);
  Merged->setLinkage(GlobalValue::InternalLinkage);
  if (Merged->hasComdat())
    Merged->setComdat(nullptr);
  Merged->addFnAttr(Attribute::NoInline);

  M->getFunctionList().insert(Root->getIterator(), Merged);

  // Move Root body into Merged.
  Merged->getBasicBlockList().splice(Merged->begin(),
                                     Root->getBasicBlockList());

  Function::arg_iterator NewArgIt = Merged->arg_begin();
  for (Argument &RootArg : Root->args()) {
    Argument &MergedArg = *NewArgIt++;
    RootArg.replaceAllUsesWith(&MergedArg);
  }

  unsigned NumOrigArgs = Root->arg_size();
  for (unsigned ParamIdx = 0, ParamEnd = ParamLocs.size(); ParamIdx != ParamEnd;
       ++ParamIdx) {
    const OpLoc &Loc = ParamLocs[ParamIdx];
    Instruction *Inst = RootCF.Instructions[Loc.InstIndex];
    Argument *ParamArg = Merged->getArg(NumOrigArgs + ParamIdx);
    Value *OldValue = Inst->getOperand(Loc.OperandIndex);
    if (OldValue->getType() != ParamArg->getType()) {
      IRBuilder<> Builder(Inst);
      Inst->setOperand(Loc.OperandIndex,
                       createCast(Builder, ParamArg, OldValue->getType()));
    } else {
      Inst->setOperand(Loc.OperandIndex, ParamArg);
    }
  }

  return Merged;
}

static void createThunk(Function *Thunk, ArrayRef<Constant *> Params,
                        Function *ToFunc) {
  assert(Thunk->arg_size() + Params.size() ==
         ToFunc->getFunctionType()->getNumParams());

  auto EC = Thunk->getEntryCount();
  clearFunctionBody(Thunk);

  BasicBlock *BB = BasicBlock::Create(Thunk->getContext(), "", Thunk);
  IRBuilder<> Builder(BB);

  SmallVector<Value *, 16> Args;
  FunctionType *ToFuncTy = ToFunc->getFunctionType();
  unsigned ParamIdx = 0;
  for (Argument &AI : Thunk->args()) {
    Args.push_back(createCast(Builder, &AI, ToFuncTy->getParamType(ParamIdx)));
    ++ParamIdx;
  }

  for (unsigned I = 0, E = Params.size(); I != E; ++I, ++ParamIdx) {
    Value *Param = Params[I];
    assert(ParamIdx < ToFuncTy->getNumParams());
    Args.push_back(createCast(Builder, Param, ToFuncTy->getParamType(ParamIdx)));
  }

  CallInst *CI = Builder.CreateCall(ToFunc, Args);
  bool IsSwiftTailCall = ToFunc->getCallingConv() == CallingConv::SwiftTail &&
                         Thunk->getCallingConv() == CallingConv::SwiftTail;
  CI->setTailCallKind(IsSwiftTailCall ? CallInst::TCK_MustTail
                                      : CallInst::TCK_Tail);
  CI->setCallingConv(ToFunc->getCallingConv());
  CI->setAttributes(ToFunc->getAttributes());
  if (Thunk->getReturnType()->isVoidTy())
    Builder.CreateRetVoid();
  else
    Builder.CreateRet(createCast(Builder, CI, Thunk->getReturnType()));

  if (EC)
    Thunk->setEntryCount(*EC);
}

class GlobalMergeFunc {
public:
  bool run(Module &M);

private:
  bool mergeGroup(SmallVectorImpl<CandidateFunction> &Candidates,
                  ArrayRef<unsigned> GroupIndices);
};

static uint64_t getCoarseBucketKey(const CandidateFunction &CF) {
  return static_cast<uint64_t>(
      hash_combine(CF.F->getFunctionType(), CF.F->arg_size(), CF.F->size(),
                   CF.Instructions.size(),
                   static_cast<unsigned>(CF.F->getCallingConv())));
}

bool GlobalMergeFunc::mergeGroup(SmallVectorImpl<CandidateFunction> &Candidates,
                                 ArrayRef<unsigned> GroupIndices) {
  if (GroupIndices.size() < 2)
    return false;

  CandidateFunction &RootCF = Candidates[GroupIndices[0]];

  SmallVector<OpLoc, 16> CandidateLocs;
  collectParameterizableLocations(RootCF, CandidateLocs);
  if (CandidateLocs.empty())
    return false;

  SmallVector<OpLoc, 16> VaryingLocs;
  for (unsigned I = 0, E = CandidateLocs.size(); I != E; ++I) {
    const OpLoc &Loc = CandidateLocs[I];
    const Constant *Base = cast<Constant>(
        Candidates[GroupIndices[0]]
            .Instructions[Loc.InstIndex]
            ->getOperand(Loc.OperandIndex));
    bool Different = false;
    for (unsigned J = 1, JE = GroupIndices.size(); J != JE; ++J) {
      const CandidateFunction &CF = Candidates[GroupIndices[J]];
      const Constant *Curr = cast<Constant>(
          CF.Instructions[Loc.InstIndex]->getOperand(Loc.OperandIndex));
      if (!constantsEqual(Base, Curr)) {
        Different = true;
        break;
      }
    }
    if (Different)
      VaryingLocs.push_back(Loc);
  }

  if (VaryingLocs.empty())
    return false;

  SmallVector<SmallVector<Constant *, 8>, 8> ParamsByFunction;
  ParamsByFunction.reserve(GroupIndices.size());
  for (unsigned I = 0, E = GroupIndices.size(); I != E; ++I) {
    const CandidateFunction &CF = Candidates[GroupIndices[I]];
    SmallVector<Constant *, 8> Params;
    Params.reserve(VaryingLocs.size());
    for (unsigned J = 0, JE = VaryingLocs.size(); J != JE; ++J) {
      const OpLoc &Loc = VaryingLocs[J];
      Params.push_back(cast<Constant>(
          CF.Instructions[Loc.InstIndex]->getOperand(Loc.OperandIndex)));
    }
    ParamsByFunction.push_back(std::move(Params));
  }

  Function *MergedFunc = createMergedFunction(RootCF, VaryingLocs);
  LLVM_DEBUG(dbgs() << "[GlobalMergeFunc] Created merged function "
                    << MergedFunc->getName() << " with " << VaryingLocs.size()
                    << " const parameters.\n");

  for (unsigned I = 0, E = GroupIndices.size(); I != E; ++I) {
    Function *F = Candidates[GroupIndices[I]].F;
    createThunk(F, ParamsByFunction[I], MergedFunc);
    ++NumMergedFunctions;
  }
  ++NumMergedGroups;
  return true;
}

bool GlobalMergeFunc::run(Module &M) {
  SmallVector<CandidateFunction, 32> Candidates;
  for (Function &F : M) {
    if (!isEligibleFunction(&F))
      continue;
    CandidateFunction CF;
    CF.F = &F;
    collectInstructions(F, CF.Instructions);
    Candidates.push_back(std::move(CF));
  }

  if (Candidates.size() < 2)
    return false;

  DenseMap<uint64_t, SmallVector<unsigned, 8> > Buckets;
  SmallVector<uint64_t, 16> BucketOrder;
  for (unsigned I = 0, E = Candidates.size(); I != E; ++I) {
    uint64_t Key = getCoarseBucketKey(Candidates[I]);
    SmallVector<unsigned, 8> &Indices = Buckets[Key];
    if (Indices.empty())
      BucketOrder.push_back(Key);
    Indices.push_back(I);
  }
  llvm::sort(BucketOrder);

  bool Changed = false;
  BitVector Assigned(Candidates.size(), false);
  for (unsigned B = 0, BE = BucketOrder.size(); B != BE; ++B) {
    DenseMap<uint64_t, SmallVector<unsigned, 8> >::iterator BI =
        Buckets.find(BucketOrder[B]);
    assert(BI != Buckets.end());
    SmallVector<unsigned, 8> &Indices = BI->second;
    for (unsigned I = 0, E = Indices.size(); I != E; ++I) {
      unsigned RootIdx = Indices[I];
      if (Assigned[RootIdx])
        continue;

      SmallVector<unsigned, 8> Group;
      Group.push_back(RootIdx);
      Assigned[RootIdx] = true;

      for (unsigned J = I + 1; J != E; ++J) {
        unsigned OtherIdx = Indices[J];
        if (Assigned[OtherIdx])
          continue;
        if (!areEquivalentIgnoringParameterizableConstants(Candidates[RootIdx],
                                                           Candidates[OtherIdx]))
          continue;
        Group.push_back(OtherIdx);
        Assigned[OtherIdx] = true;
      }

      if (Group.size() < 2)
        continue;

      Changed |= mergeGroup(Candidates, Group);
    }
  }

  return Changed;
}

class GlobalMergeFuncPassWrapper : public ModulePass {
public:
  static char ID;

  GlobalMergeFuncPassWrapper() : ModulePass(ID) {
    initializeGlobalMergeFuncPassWrapperPass(*PassRegistry::getPassRegistry());
  }

  StringRef getPassName() const override { return "Global Merge Functions"; }

  bool runOnModule(Module &M) override {
    if (skipModule(M))
      return false;
    return GlobalMergeFunc().run(M);
  }
};

} // end anonymous namespace

char GlobalMergeFuncPassWrapper::ID = 0;
INITIALIZE_PASS(GlobalMergeFuncPassWrapper, "global-merge-func",
                "Global merge functions", false, false)

ModulePass *llvm::createGlobalMergeFuncPass() {
  return new GlobalMergeFuncPassWrapper();
}

PreservedAnalyses GlobalMergeFuncPass::run(Module &M,
                                           ModuleAnalysisManager &AM) {
  (void)AM;
  if (!GlobalMergeFunc().run(M))
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}
