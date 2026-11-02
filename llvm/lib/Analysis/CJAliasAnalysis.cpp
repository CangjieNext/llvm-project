//===- CJAliasAnalysis.cpp --------------------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares a simple AliasAnalysis using special knowledge
// of Cangjie to enhance other optimization passes which rely on the Alias
// Analysis infrastructure.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/CJAliasAnalysis.h"
#include "llvm/ADT/Optional.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/CJIntrinsics.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/SafepointIRVerifier.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;
using namespace cangjie;

namespace llvm {
extern cl::opt<bool> CJPipeline;
cl::opt<bool> EnableCJAA("enbale-cangjie-alias-analysis",
                         cl::desc("enbale/disable cangjie alias analysis"),
                         cl::init(CJPipeline), cl::Hidden);
} // namespace llvm

static const unsigned MaxLookupSearchDepth = 6;

bool CJAAResult::invalidate(Function &Fn, const PreservedAnalyses &PA,
                            FunctionAnalysisManager::Invalidator &Inv) {
  return DT && Inv.invalidate<DominatorTreeAnalysis>(Fn, PA);
}

// An alloca produced by PEA stackification carries the "ea_val" metadata (set
// by CJPartialEscapeAnalysis::setEscapeMeta). Its body may hold GC pointers,
// and gcwrite may store an as1-cast of such an alloca into a heap object's
// field; a later load from that field then returns a value whose type is
// addrspace(1) but that points back to the stack alloca at run time. The
// stack-vs-heap NoAlias rule below would misclassify that pair as NoAlias.
// Treat any side whose underlying object is a PEA-degraded alloca as
// MayAlias so the NoAlias fast path is not taken. The caller must pass an
// already-resolved underlying object (i.e. the result of getUnderlyingObject);
// this helper does not re-resolve.
static bool isPEADegradedAlloca(const Value *O) {
  if (const auto *AI = dyn_cast<AllocaInst>(O))
    return AI->getMetadata("ea_val") != nullptr;
  return false;
}

static bool isGCPointerOrGlobalBase(const Value *V,
                                    unsigned MaxLookup = MaxLookupSearchDepth) {
  V = getUnderlyingObject(V, 0);
  if (isa<AllocaInst>(V))
    return false;
  if (isa<GlobalValue>(V))
    return true;
  if (isa<Argument>(V) || isa<LoadInst>(V))
    return isGCPointerType(V->getType());
  if (auto *II = dyn_cast<IntrinsicInst>(V)) {
    switch (II->getIntrinsicID()) {
    case Intrinsic::cj_gcread_ref:
      V = II->getArgOperand(1);
      break;
    case Intrinsic::cj_gcread_static_ref:
      V = II->getArgOperand(0);
      break;
    default:
      return false;
    }
    return isGCPointerOrGlobalBase(V, --MaxLookup);
  }
  if (auto *CB = dyn_cast<CallBase>(V)) {
    if (CB->getCalledFunction() &&
        CB->getCalledFunction()->hasFnAttribute("cj-heapmalloc"))
      return true;
    else
      return false;
  }
  // Otherwise, no handling is required.
  return false;
}

// Checking the alias relationship Between cj.malloc and V
static AliasResult checkHeapMallocAlias(const CallBase *CB, const Value *O,
                                        const DominatorTree *DT) {
  if (CB->getCalledFunction() &&
      CB->getCalledFunction()->hasFnAttribute("cj-heapmalloc")) {
    if (auto *I = dyn_cast<Instruction>(O); I && DT->dominates(I, CB))
      return AliasResult::NoAlias;
    if (isa<Argument>(O))
      return AliasResult::NoAlias;
  }
  return AliasResult::MayAlias;
}

AliasResult CJAAResult::alias(const MemoryLocation &LocA,
                              const MemoryLocation &LocB, AAQueryInfo &AAQI) {
  const Value *O1 = getUnderlyingObject(LocA.Ptr, MaxLookupSearchDepth);
  const Value *O2 = getUnderlyingObject(LocB.Ptr, MaxLookupSearchDepth);
  // A PEA-degraded alloca can be read back through a heap field (gcwrite stores
  // an as1-cast of it, a later load returns a value typed as1 but pointing to
  // the stack). Do not apply the stack-vs-heap NoAlias rule when either side
  // is such an alloca; fall back to the default (MayAlias) instead. Check this
  // on the already-resolved O1/O2 (reuses the getUnderlyingObject results
  // above) and before computing IsStackValue, so the fast path is skipped
  // entirely for the affected pair.
  if (isPEADegradedAlloca(O1) || isPEADegradedAlloca(O2))
    return AAResultBase::alias(LocA, LocB, AAQI);
  // In cangjie, stack space and heap space has different addrspace, and they
  // can never be aliased.
  bool IsStackValue1 = isa<AllocaInst>(O1) ||
                       (isa<Argument>(O1) && !isGCPointerType(O1->getType()));
  bool IsStackValue2 = isa<AllocaInst>(O2) ||
                       (isa<Argument>(O2) && !isGCPointerType(O2->getType()));
  if ((IsStackValue1 && isGCPointerOrGlobalBase(O2)) ||
      (IsStackValue2 && isGCPointerOrGlobalBase(O1)))
    return AliasResult::NoAlias;
  bool IsNotGV1 = isa<AllocaInst>(O1) || isGCPointerType(O1->getType());
  bool IsNotGV2 = isa<AllocaInst>(O2) || isGCPointerType(O2->getType());
  if ((IsNotGV1 && isa<GlobalValue>(O2)) || (IsNotGV2 && isa<GlobalValue>(O1)))
    return AliasResult::NoAlias;

  // %0 = load ...
  // %1 = call void CJ_MCC_New(...)
  // %2 = load %0
  // %3 = load %1
  // If one of the MemoryLocations is new, any MemoryLocations before it will
  // not be aliased to it.
  if (O1 != O2) {
    if (auto *Call1 = dyn_cast<CallBase>(O1);
        Call1 && checkHeapMallocAlias(Call1, O2, DT) == AliasResult::NoAlias)
      return AliasResult::NoAlias;
    if (auto *Call2 = dyn_cast<CallBase>(O2);
        Call2 && checkHeapMallocAlias(Call2, O1, DT) == AliasResult::NoAlias)
      return AliasResult::NoAlias;
  }

  return AAResultBase::alias(LocA, LocB, AAQI);
}

std::tuple<Optional<MemoryLocation>, Optional<MemoryLocation>,
           Optional<MemoryLocation>>
getLocInfoForCJIntrinsics(const IntrinsicInst *II,
                          const TargetLibraryInfo &TLI) {
  Optional<MemoryLocation> Src = None;
  Optional<MemoryLocation> Dst = None;
  Optional<MemoryLocation> Base = None;
  switch (II->getIntrinsicID()) {
  default:
    break;
  case Intrinsic::cj_gcwrite_struct:
    Src = MemoryLocation::getCJMemTransferSource(II);
    Dst = MemoryLocation::getForDest(II, TLI);
    Base = MemoryLocation::getAfter(getBaseObj(II));
    break;
  case Intrinsic::cj_gcread_struct:
    Src = MemoryLocation::getCJMemTransferSource(II);
    Dst = MemoryLocation::getForDest(II, TLI);
    Base = MemoryLocation::getAfter(getBaseObj(II));
    break;
  case Intrinsic::cj_gcwrite_generic:
    Src = MemoryLocation::getAfter(getSource(II));
    Dst = MemoryLocation::getAfter(getDest(II));
    Base = MemoryLocation::getAfter(getBaseObj(II));
    break;
  case Intrinsic::cj_gcread_generic:
    Src = MemoryLocation::getAfter(getSource(II));
    Dst = MemoryLocation::getAfter(getDest(II));
    Base = MemoryLocation::getAfter(getBaseObj(II));
    break;
  case Intrinsic::cj_assign_generic:
  case Intrinsic::cj_assign_local_generic:
    Src = MemoryLocation::getAfter(getSource(II));
    Dst = MemoryLocation::getAfter(getDest(II));
    break;
  }
  return {Src, Dst, Base};
}

ModRefInfo CJAAResult::getModRefInfo(const CallBase *Call,
                                     const MemoryLocation &Loc,
                                     AAQueryInfo &AAQI) {
  if (Call->getCalledFunction() &&
      Call->getCalledFunction()->hasFnAttribute("cj-heapmalloc")) {
    auto AR = getBestAAResults().alias(MemoryLocation::getBeforeOrAfter(Call),
                                       Loc, AAQI);
    if (AR == AliasResult::NoAlias)
      return ModRefInfo::NoModRef;
  }
  auto ID = Call->getIntrinsicID();
  auto MRI = ModRefInfo::ModRef;
  switch (ID) {
  default:
    break;
  case Intrinsic::lifetime_end: {
    auto AR = getBestAAResults().alias(
        MemoryLocation::getBeforeOrAfter(Call->getArgOperand(1)), Loc, AAQI);
    if (AR == AliasResult::NoAlias)
      MRI = ModRefInfo::NoModRef;
  } break;
  case Intrinsic::cj_gcread_ref:
  case Intrinsic::cj_gcread_static_ref: {
    auto AR = getBestAAResults().alias(MemoryLocation::get(Call), Loc, AAQI);
    if (AR == AliasResult::NoAlias)
      MRI = ModRefInfo::NoModRef;
    else if (AR == AliasResult::MustAlias)
      MRI = ModRefInfo::MustRef;
    else
      MRI = ModRefInfo::Ref;
  } break;
  case Intrinsic::cj_gcwrite_ref:
  case Intrinsic::cj_gcwrite_static_ref: {
    auto AR = getBestAAResults().alias(MemoryLocation::get(Call), Loc, AAQI);
    if (AR == AliasResult::NoAlias)
      MRI = ModRefInfo::NoModRef;
    else if (AR == AliasResult::MustAlias)
      MRI = ModRefInfo::MustMod;
    else
      MRI = ModRefInfo::Mod;
  } break;
  case Intrinsic::cj_gcread_static_struct:
  case Intrinsic::cj_gcwrite_static_struct: {
    auto SrcAA = getBestAAResults().alias(
        MemoryLocation::getCJMemTransferSource(cast<IntrinsicInst>(Call))
            .getValue(),
        Loc, AAQI);
    auto DestAA = getBestAAResults().alias(
        MemoryLocation::getForDest(Call, TLI).getValue(), Loc, AAQI);
    MRI = ModRefInfo::NoModRef;
    if (SrcAA != AliasResult::NoAlias)
      MRI = setRef(MRI);
    if (DestAA != AliasResult::NoAlias)
      MRI = setMod(MRI);
  } break;
  case Intrinsic::cj_gcread_struct:
  case Intrinsic::cj_gcwrite_struct:
  case Intrinsic::cj_gcwrite_generic:
  case Intrinsic::cj_gcread_generic:
  case Intrinsic::cj_assign_generic:
  case Intrinsic::cj_assign_local_generic: {
    auto [LocS, LocD, LocB] =
        getLocInfoForCJIntrinsics(cast<IntrinsicInst>(Call), TLI);
    if (!LocS || !LocD)
      report_fatal_error("error");
    auto SrcAA = getBestAAResults().alias(LocS.getValue(), Loc, AAQI);
    auto DstAA = getBestAAResults().alias(LocD.getValue(), Loc, AAQI);
    auto BaseAA = AliasResult::NoAlias;
    if (LocB)
      BaseAA = getBestAAResults().alias(LocB.getValue(), Loc, AAQI);
    if (SrcAA != AliasResult::NoAlias || BaseAA != AliasResult::NoAlias)
      MRI = setRef(MRI);
    if (DstAA != AliasResult::NoAlias)
      MRI = setMod(MRI);
  } break;
  case Intrinsic::cj_memset:
    auto AR = getBestAAResults().alias(
        MemoryLocation::getAfter(Call->getArgOperand(0)), Loc, AAQI);
    if (AR == AliasResult::NoAlias)
      MRI = ModRefInfo::NoModRef;
    else
      MRI = ModRefInfo::Mod;
    break;
  }
  return MRI;
}

AnalysisKey CangjieAA::Key;

CJAAResult CangjieAA::run(Function &F, FunctionAnalysisManager &AM) {
  auto &TLI = AM.getResult<TargetLibraryAnalysis>(F);
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  return CJAAResult(F.getParent()->getDataLayout(), TLI, &DT);
}

CangjieAAWrapperPass::CangjieAAWrapperPass() : FunctionPass(ID) {
  initializeCangjieAAWrapperPassPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(CangjieAAWrapperPass, "cangjie-aa",
                      "Cangjie Alias Analysis", true, true)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(CangjieAAWrapperPass, "cangjie-aa",
                    "Cangjie Alias Analysis", true, true)

char CangjieAAWrapperPass::ID = 0;

FunctionPass *llvm::createCangjieAAWrapperPass() {
  return new CangjieAAWrapperPass();
}

bool CangjieAAWrapperPass::runOnFunction(Function &F) {
  auto &TLIWP = getAnalysis<TargetLibraryInfoWrapperPass>();
  auto &DTWP = getAnalysis<DominatorTreeWrapperPass>();
  Result.reset(new CJAAResult(F.getParent()->getDataLayout(), TLIWP.getTLI(F),
                              &DTWP.getDomTree()));
  return false;
}

void CangjieAAWrapperPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
}
