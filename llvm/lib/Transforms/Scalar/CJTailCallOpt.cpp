//===- CJTailCallOpt.cpp - Cangjie tail call optimization -----------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Mark safe tail-position Cangjie calls as tail calls before
// CJRewriteStatepoint converts them to statepoints.  The pass intentionally
// stays conservative: it refuses candidates that still pass current-frame stack
// objects to the callee, and only forwards Cangjie Unit-style empty sret slots.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJTailCallOpt.h"

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Scalar.h"

using namespace llvm;

#define DEBUG_TYPE "cj-tail-call-opt"

static cl::opt<bool>
    DisableCJTailCallOpt("disable-cj-tail-call-opt", cl::init(false),
                         cl::Hidden,
                         cl::desc("Do not run CJTailCallOpt"));

namespace {

static bool isLifetimeIntrinsic(const Instruction &I) {
  const auto *II = dyn_cast<IntrinsicInst>(&I);
  if (!II)
    return false;

  return II->getIntrinsicID() == Intrinsic::lifetime_start ||
         II->getIntrinsicID() == Intrinsic::lifetime_end;
}

static bool isSkippableAfterTailCall(const Instruction &I) {
  return isa<DbgInfoIntrinsic>(&I) || isLifetimeIntrinsic(I);
}

static CallInst *getTailPositionCall(ReturnInst &RI) {
  Instruction *I = RI.getPrevNode();
  while (I && isSkippableAfterTailCall(*I))
    I = I->getPrevNode();

  return dyn_cast_or_null<CallInst>(I);
}

static bool returnMatchesCallResult(const Function &F, const CallInst &CI,
                                    const ReturnInst &RI) {
  if (CI.getType()->isVoidTy())
    return RI.getReturnValue() == nullptr;

  return F.getReturnType() == CI.getType() && RI.getReturnValue() == &CI;
}

static int getSRetArgIndex(const CallBase &CB) {
  int SRetIdx = -1;
  for (unsigned I = 0, E = CB.arg_size(); I != E; ++I) {
    if (!CB.paramHasAttr(I, Attribute::StructRet))
      continue;
    if (SRetIdx != -1)
      return -2;
    SRetIdx = static_cast<int>(I);
  }
  return SRetIdx;
}

static Argument *getSRetArgument(Function &F) {
  Argument *SRetArg = nullptr;
  for (Argument &Arg : F.args()) {
    if (!Arg.hasStructRetAttr())
      continue;
    if (SRetArg)
      return nullptr;
    SRetArg = &Arg;
  }
  return SRetArg;
}

static bool isEmptyStructType(Type *Ty) {
  auto *STy = dyn_cast_or_null<StructType>(Ty);
  return STy && STy->getNumElements() == 0;
}

static bool isAllowedSRetAllocaUser(Value *V, User *U, const CallInst &CI,
                                    unsigned SRetIdx,
                                    SmallVectorImpl<Value *> &Worklist,
                                    SmallPtrSetImpl<Value *> &Seen) {
  if (U == &CI)
    return CI.getArgOperand(SRetIdx) == V;

  if (auto *I = dyn_cast<Instruction>(U)) {
    if (isa<DbgInfoIntrinsic>(I) || isLifetimeIntrinsic(*I))
      return true;

    if (isa<BitCastInst>(I) || isa<AddrSpaceCastInst>(I)) {
      if (Seen.insert(I).second)
        Worklist.push_back(I);
      return true;
    }
  }

  return false;
}

static bool isOnlyUsedAsCallSRet(AllocaInst &AI, const CallInst &CI,
                                 unsigned SRetIdx) {
  SmallVector<Value *, 4> Worklist;
  SmallPtrSet<Value *, 4> Seen;
  Worklist.push_back(&AI);
  Seen.insert(&AI);

  while (!Worklist.empty()) {
    Value *V = Worklist.pop_back_val();
    for (Use &U : V->uses()) {
      if (!isAllowedSRetAllocaUser(V, U.getUser(), CI, SRetIdx, Worklist,
                                   Seen))
        return false;
    }
  }

  return true;
}

static bool tryForwardEmptySRetSlot(Function &F, CallInst &CI,
                                    unsigned SRetIdx, Argument &CallerSRet) {
  Type *CallerSRetTy = CallerSRet.getParamStructRetType();
  Type *CallSRetTy = CI.getParamStructRetType(SRetIdx);
  if (!CallerSRetTy || CallerSRetTy != CallSRetTy ||
      !isEmptyStructType(CallSRetTy))
    return false;

  Value *CallSRetArg = CI.getArgOperand(SRetIdx);
  if (CallSRetArg == &CallerSRet)
    return true;

  if (CallSRetArg->getType() != CallerSRet.getType())
    return false;

  if (CI.paramHasAttr(SRetIdx, Attribute::NoAlias) &&
      !CallerSRet.hasAttribute(Attribute::NoAlias))
    return false;

  auto *AI = dyn_cast<AllocaInst>(getUnderlyingObject(CallSRetArg));
  if (!AI || AI->getFunction() != &F || AI->getAllocatedType() != CallSRetTy)
    return false;

  if (!isOnlyUsedAsCallSRet(*AI, CI, SRetIdx))
    return false;

  LLVM_DEBUG(dbgs() << "CJTailCallOpt: forward empty sret slot in "
                    << F.getName() << "\n");
  CI.setArgOperand(SRetIdx, &CallerSRet);
  return true;
}

static bool prepareSRetForTailCall(Function &F, CallInst &CI,
                                   unsigned SRetIdx) {
  Argument *CallerSRet = getSRetArgument(F);
  if (!CallerSRet)
    return false;

  return tryForwardEmptySRetSlot(F, CI, SRetIdx, *CallerSRet);
}

static bool pointsToLocalAlloca(Value *V, const Function &F) {
  if (!V->getType()->isPointerTy())
    return false;

  auto *AI = dyn_cast<AllocaInst>(getUnderlyingObject(V));
  return AI && AI->getFunction() == &F;
}

static bool isZeroSizeAlloca(const AllocaInst &AI, const DataLayout &DL) {
  Optional<TypeSize> Size = AI.getAllocationSizeInBits(DL);
  return Size && !Size->isScalable() && Size->getFixedSize() == 0;
}

static bool hasNonZeroLocalAlloca(const Function &F) {
  const DataLayout &DL = F.getParent()->getDataLayout();
  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB)
      if (const auto *AI = dyn_cast<AllocaInst>(&I))
        if (!isZeroSizeAlloca(*AI, DL))
          return true;

  return false;
}

static bool passesLocalAllocaToCallee(const Function &F, const CallInst &CI,
                                      int IgnoredArgIdx = -1) {
  for (unsigned I = 0, E = CI.arg_size(); I != E; ++I) {
    if (static_cast<int>(I) == IgnoredArgIdx)
      continue;
    if (pointsToLocalAlloca(CI.getArgOperand(I), F))
      return true;
  }

  return false;
}

static bool canMarkTailCall(Function &F, CallInst &CI, ReturnInst &RI) {
  if (CI.isTailCall() || CI.isNoTailCall())
    return false;

  if (F.getFnAttribute("disable-tail-calls").getValueAsBool())
    return false;

  if (CI.hasOperandBundles() || CI.isInlineAsm())
    return false;

  if (CI.getIntrinsicID() != Intrinsic::not_intrinsic)
    return false;

  if (!returnMatchesCallResult(F, CI, RI))
    return false;

  if (hasNonZeroLocalAlloca(F))
    return false;

  int SRetIdx = getSRetArgIndex(CI);
  if (SRetIdx < -1)
    return false;

  if (passesLocalAllocaToCallee(F, CI, SRetIdx))
    return false;

  if (SRetIdx != -1 &&
      !prepareSRetForTailCall(F, CI, static_cast<unsigned>(SRetIdx)))
    return false;

  return true;
}

static bool runOnFunction(Function &F) {
  if (F.isDeclaration() || F.empty() || !F.hasCangjieGC())
    return false;

  bool Changed = false;
  for (BasicBlock &BB : F) {
    auto *RI = dyn_cast<ReturnInst>(BB.getTerminator());
    if (!RI)
      continue;

    CallInst *CI = getTailPositionCall(*RI);
    if (!CI || !canMarkTailCall(F, *CI, *RI))
      continue;

    LLVM_DEBUG(dbgs() << "CJTailCallOpt: mark tail call in " << F.getName()
                      << "\n");
    CI->setTailCallKind(CallInst::TCK_Tail);
    Changed = true;
  }

  return Changed;
}

static bool runCJTailCallOpt(Module &M) {
  if (DisableCJTailCallOpt)
    return false;

  bool Changed = false;
  for (Function &F : M)
    Changed |= runOnFunction(F);

  return Changed;
}

class CJTailCallOptLegacyPass : public ModulePass {
public:
  static char ID;

  CJTailCallOptLegacyPass() : ModulePass(ID) {
    initializeCJTailCallOptLegacyPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override { return runCJTailCallOpt(M); }
};

} // namespace

PreservedAnalyses CJTailCallOpt::run(Module &M,
                                     ModuleAnalysisManager &) const {
  if (!runCJTailCallOpt(M))
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}

char CJTailCallOptLegacyPass::ID = 0;

ModulePass *llvm::createCJTailCallOptLegacyPass() {
  return new CJTailCallOptLegacyPass();
}

INITIALIZE_PASS(CJTailCallOptLegacyPass, "cj-tail-call-opt",
                "Cangjie Tail Call Optimization", false, false)
