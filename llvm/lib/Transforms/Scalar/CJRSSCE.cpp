//===- CJRSSCE.cpp ----------------------------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// In Cangjie, struct is value semantics, so a copy is required each time it is
// passed. The CJRSSCE pass eliminate redundant stack struct copy through memory
// define range analysis and alias analysis.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJRSSCE.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/SafepointIRVerifier.h"
#include "llvm/IR/Value.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Scalar.h"
#include <cstddef>
#include <cstdint>
#include <iterator>

#define DEBUG_TYPE "cj-rssce"

using namespace llvm;
static cl::opt<bool> EnableCJRSSCE("enable-cj-rssce", cl::Hidden,
                                   cl::init(true));

using Interval = std::pair<uint64_t, uint64_t>;

static Type *getKnownPointeeType(const Value *V) {
  V = V->stripPointerCasts();
  if (auto *GEP = dyn_cast<GEPOperator>(V))
    return GEP->getSourceElementType();
  if (auto *AI = dyn_cast<AllocaInst>(V))
    return AI->getAllocatedType();
  if (auto *GV = dyn_cast<GlobalVariable>(V))
    return GV->getValueType();
  if (auto *Arg = dyn_cast<Argument>(V)) {
    if (Arg->hasByValAttr())
      return Arg->getParamByValType();
  }
  auto *PT = dyn_cast<PointerType>(V->getType());
  if (!PT || PT->isOpaque())
    return nullptr;
  return PT->getNonOpaquePointerElementType();
}

static std::pair<Value *, uint64_t>
getBasePtrAndOffset(const DataLayout &DL, Value *V, Instruction *I,
                    Instruction **LastClobberI = nullptr) {
  // For example:
  //   %16 = llvm.cj.gcread.ref(%cmp, %15)
  //   store i8 addrspace(1)* %16, %17
  // The Ptr now is %16, and we need to find cmp to check if cmp is same as
  // BasePtr. And if V is a load instruction, we need to ensure that the load
  // pointer is not clobbered starting from here.
  V = V->stripPointerCasts();
  if (isa<StoreInst>(I)) {
    Instruction *StartI = I->getFunction()->getEntryBlock().getFirstNonPHI();
    if (auto *II = dyn_cast<IntrinsicInst>(V)) {
      if (II->getIntrinsicID() == Intrinsic::cj_gcread_ref)
        V = II->getArgOperand(1)->stripPointerCasts();
      else if (II->getIntrinsicID() == Intrinsic::cj_gcread_static_ref)
        V = II->getArgOperand(0)->stripPointerCasts();
      StartI = II;
    } else if (auto *LI = dyn_cast<LoadInst>(V)) {
      V = LI->getPointerOperand()->stripPointerCasts();
      StartI = LI;
    }
    if (LastClobberI)
      *LastClobberI = StartI;
  }
  // V is not a memory object.
  if (!isa<PointerType>(V->getType()))
    return {nullptr, 0};
  const unsigned BitWidth = DL.getIndexSizeInBits(0);
  constexpr uint64_t OffsetRawValue = 0;
  APInt Offset(BitWidth, OffsetRawValue);
  Value *BPtr = V->stripAndAccumulateConstantOffsets(DL, Offset, true);
  return {BPtr, Offset.getZExtValue()};
}

static std::tuple<Value *, Value *, uint64_t>
getSrcDstPtrAndSize(const DataLayout &DL, Value *V) {
  Value *Src = nullptr;
  Value *Dst = nullptr;
  uint64_t Size = 0;
  // Since base loc is an object on the stack, only store or memcpy for it is
  // considered.
  if (auto *SI = dyn_cast<StoreInst>(V)) {
    Dst = SI->getPointerOperand();
    Src = SI->getValueOperand();
    constexpr int Length = 8;
    Size = DL.getTypeSizeInBits(SI->getValueOperand()->getType()) / Length;
  } else if (auto *MCI = dyn_cast<MemCpyInst>(V)) {
    Dst = MCI->getDest();
    Src = MCI->getSource();
    auto *ConstSZ = dyn_cast<ConstantInt>(MCI->getArgOperand(2));
    // We've got an uncertain clobber instruction.
    if (!ConstSZ)
      return {nullptr, nullptr, 0};
    Size = ConstSZ->getZExtValue();
  } else {
    return {nullptr, nullptr, 0};
  }
  return {Src, Dst, Size};
}

static MemoryAccess *findLastDefDominateMA(MemorySSA &MSSA, BasicBlock *BB,
                                           MemoryAccess *MA, bool LastClosure) {
  auto *DefLists = MSSA.getBlockDefs(BB);
  if (!DefLists)
    return nullptr;
  for (auto Iter = DefLists->rbegin(); Iter != DefLists->rend(); ++Iter) {
    if (MSSA.dominates(&*Iter, MA)) {
      if (&*Iter == MA && !LastClosure)
        continue;
      return &const_cast<MemoryAccess &>(*Iter);
    }
  }
  return nullptr;
}

bool llvm::hasMemoryDefBetween(MemorySSA &MSSA, DominatorTree &DT,
                               const DataLayout &DL, Value *UnderObj,
                               Instruction *FirstI, MemoryAccess *LastMA,
                               bool LastClosure, bool EnableDiffBB) {
  assert(isa<MemoryUseOrDef>(LastMA));
  Instruction *LastI = cast<MemoryUseOrDef>(LastMA)->getMemoryInst();
  if (FirstI->getParent() == LastI->getParent() && !FirstI->comesBefore(LastI))
    return false;
  if (!EnableDiffBB && FirstI->getParent() != LastI->getParent())
    return true;
  MemoryAccess *FirstDef = MSSA.getMemoryAccess(FirstI);
  // Find last MemoryDef that dominates LastMA.
  MemoryAccess *LastDef =
      findLastDefDominateMA(MSSA, LastI->getParent(), LastMA, LastClosure);
  assert((LastDef || EnableDiffBB) && "Cannot be false at the same time");
  if (!LastDef) // Now, LastDef is MemoryUse
    LastDef = cast<MemoryUseOrDef>(LastMA)->getDefiningAccess();
  SmallVector<const Value *, 8> UnderObjs;
  getUnderlyingObjects(UnderObj, UnderObjs);
  for (auto *V : UnderObjs) {
    auto *ST = dyn_cast_or_null<StructType>(getKnownPointeeType(V));
    // Loc may be load inst, and the clobber we find may not accurate.
    MemoryLocation Loc =
        ST ? MemoryLocation(V, LocationSize::precise(
                                   DL.getStructLayout(ST)->getSizeInBytes()))
           : MemoryLocation::getAfter(V);
    // MSSA uses SimpleCaptureInfo by default. To make the result more accurate,
    // consider using BatchAA for more analysis of MA.
    MemoryAccess *MA =
        MSSA.getWalker()->getClobberingMemoryAccess(LastDef, Loc);
    // if FirstDef is equal to LastDef, we known FirstDef does not clobber
    // source.This is based on the premise that we've aligned it by the offset
    // of source and dest.
    if (MA == LastDef && LastDef != FirstDef)
      return true;
    if (MA == FirstDef)
      continue;
    // If the MA is MemoryPhi, it is impossible for the MA to satisfy
    // FirstI->MA->LastI because FirstI and LastI are in the same BB.
    if (isa<MemoryPhi>(MA) || MSSA.isLiveOnEntryDef(MA))
      continue;
    assert((!EnableDiffBB || !isa<MemoryPhi>(MA)) && // Optimize it in future
           "EnableDiffBB support only single predecessor");
    Instruction *DefI = cast<MemoryDef>(MA)->getMemoryInst();
    assert(
        DefI != FirstI &&
        "FirstI is last clobber inst of memcpy dest, therefore, def of source "
        "cannot be equal to FirstI");
    if (DT.dominates(FirstI, DefI) && MSSA.dominates(MA, LastMA))
      return true;
  }
  return false;
}

// This optimization achieves an effect similar to MemCpyOpt.
//   copy(a <- b)
//   call func(a)
// If func is readonly for a, a can be safely replaced with b. "copy" consists
// of multiple define instructions with the same dest and source, including
// store and memcpy.
struct SourcePropagationState {
  AliasAnalysis &AA;
  EarliestEscapeInfo EI;
  BatchAAResults BatchAA;
  MemorySSA &MSSA;

  DominatorTree &DT;
  LoopInfo &LI;
  const DataLayout &DL;

  SmallPtrSet<const Value *, 32> EphValues;

  struct IVInfo {
    Interval Src;
    Interval Dst;
    Instruction *DefI;
    GetElementPtrInst *GEP = nullptr;
  };

  SourcePropagationState(const DataLayout &DL, AliasAnalysis &AA,
                         MemorySSA &MSSA, DominatorTree &DT, LoopInfo &LI)
      : AA(AA), EI(DT, LI, EphValues), BatchAA(AA, &EI), MSSA(MSSA), DT(DT),
        LI(LI), DL(DL) {}

  void filterCallArgs(CallBase *CB, SetVector<unsigned> &CallIndexs) {
    for (unsigned I = 0; I < CB->getNumOperands(); ++I) {
      auto *AI = dyn_cast<AllocaInst>(CB->getOperand(I));
      if (AI == nullptr)
        continue;
      if (!AI->getAllocatedType()->isStructTy())
        continue;
      CallIndexs.insert(I);
    }
  }

  // When instructions such as load are encountered, continue to search upwards.
  bool isBaseAllocaValue(Value *V, SetVector<AllocaInst *> &AllocaBase,
                         SetVector<Value *> &Cache) {
    if (Cache.contains(V))
      return true;
    Cache.insert(V);
    if (isa<Argument>(V))
      return false;
    if (isa<AllocaInst>(V))
      return true;
    if (auto *GEP = dyn_cast<GetElementPtrInst>(V))
      return isBaseAllocaValue(GEP->getPointerOperand()->stripPointerCasts(),
                               AllocaBase, Cache);
    if (auto *LI = dyn_cast<LoadInst>(V)) {
      // Check whether LI's pointer operand is a stack value, so Base has to be
      // on a stack to be possible.
      if (auto *AI = dyn_cast<AllocaInst>(
              getUnderlyingObject(LI->getPointerOperand()))) {
        AllocaBase.insert(AI);
        return true;
      }
      return false;
    }
    if (auto *SI = dyn_cast<SelectInst>(V))
      return isBaseAllocaValue(SI->getTrueValue()->stripPointerCasts(),
                               AllocaBase, Cache) &&
             isBaseAllocaValue(SI->getFalseValue()->stripPointerCasts(),
                               AllocaBase, Cache);
    if (auto *PHI = dyn_cast<PHINode>(V)) {
      bool IsAlloca = true;
      for (auto &Value : PHI->incoming_values()) {
        auto *I = dyn_cast<Instruction>(&Value);
        if (I == nullptr)
          return false;
        IsAlloca &=
            isBaseAllocaValue(I->stripPointerCasts(), AllocaBase, Cache);
      }
      return IsAlloca;
    }
    return false;
  }

  // Scan uses of the src of value copy.
  // Record Instructions which writes to it.
  // When write it elsewhere, end directly.
  bool getBaseUses(Value *Val, SetVector<Value *> &BaseUses,
                   SetVector<Instruction *> &BaseStoreMemcpy) {
    bool IsSrcOrBarrier = false;
    for (auto *Use : Val->users()) {
      auto *V = dyn_cast<Value>(Use);
      if (BaseUses.contains(V))
        continue;
      BaseUses.insert(V);

      // We check Src, not the contents of Src.
      if (isa<LoadInst>(V))
        continue;

      if (isStoreMemcpyInst(V, Val, IsSrcOrBarrier)) {
        if (IsSrcOrBarrier)
          return false;
        BaseStoreMemcpy.insert(dyn_cast<Instruction>(V));
      } else {
        if (auto *CB = dyn_cast<CallBase>(V)) {
          if (CB->getIntrinsicID() == Intrinsic::cj_gcread_ref ||
              CB->getIntrinsicID() == Intrinsic::cj_gcread_static_ref)
            continue;
          for (unsigned I = 0; I < CB->getNumOperands(); ++I) {
            if (CB->getArgOperand(I) == Val &&
                (CB->getParamAttr(I, Attribute::StructRet).isValid() ||
                 !CB->getParamAttr(I, Attribute::ReadOnly).isValid()))
              return false;
          }
        }
      }

      if (!getBaseUses(V, BaseUses, BaseStoreMemcpy))
        return false;
    }
    return true;
  }

  bool isStoreMemcpyInst(Value *V, Value *Base, bool &IsSrcOrBarrier) {
    auto *I = dyn_cast<Instruction>(V);
    if (I == nullptr)
      return false;
    if (auto *SI = dyn_cast<StoreInst>(I)) {
      if (SI->getValueOperand() == Base)
        IsSrcOrBarrier = true;
      return true;
    }
    if (auto *CB = dyn_cast<CallBase>(I)) {
      if (CB->getCalledFunction() == nullptr)
        return false;
      Intrinsic::ID IID = CB->getIntrinsicID();
      if (isCJMemcpyIntrinsic(IID))
        return true;
      if (isCJWriteBarrierIntrinsic(IID) || isCJAtomicIntrinsic(IID)) {
        IsSrcOrBarrier = true;
        return true;
      }
    }
    return false;
  }

  bool isKlassGlobal(GlobalVariable *GV) {
    auto *ST = dyn_cast_or_null<StructType>(GV ? GV->getValueType() : nullptr);
    return ST && ST->getStructName().isCangjieTypeInfo();
  }

  bool isStackValueSrc(SetVector<Instruction *> &BaseStoreMemcpy) {
    for (auto *I : BaseStoreMemcpy) {
      Value *Src = nullptr;
      if (auto *SI = dyn_cast<StoreInst>(I))
        Src = SI->getValueOperand();
      else
        Src = dyn_cast<CallBase>(I)->getArgOperand(1);

      if (isa<ConstantInt>(Src))
        continue;
      auto *GV = dyn_cast<GlobalVariable>(Src->stripPointerCasts());
      if (GV != nullptr && isInt8AS0Pty(Src) && isKlassGlobal(GV))
        continue;
      if (auto *AI = dyn_cast<AllocaInst>(Src->stripPointerCasts()))
        if (isa<StructType>(AI->getAllocatedType()))
          continue;
      return false;
    }
    return true;
  }

  std::tuple<SmallVector<Value *, 8>, StructType *, uint64_t>
  getGEPArrayIndicesAndOffset(GetElementPtrInst *GEP) {
    if (!isa<StructType>(GEP->getSourceElementType()))
      return {{}, nullptr, 0};
    uint64_t Offset = 0;
    Type *OriginTy = GEP->getSourceElementType();
    Type *LastTy = OriginTy;
    SmallVector<Value *, 8> ArrayIndices;
    for (gep_type_iterator GTI = ++gep_type_begin(GEP), GTE = gep_type_end(GEP);
         GTI != GTE; ++GTI) {
      Type *Ty = GTI.getIndexedType();
      if (auto *ATy = dyn_cast<ArrayType>(LastTy)) {
        Offset = 0;
        OriginTy = ATy->getElementType();
        ArrayIndices.push_back(GTI.getOperand());
      } else if (auto *STy = GTI.getStructTypeOrNull()) {
        auto *ConstIdx = dyn_cast<ConstantInt>(GTI.getOperand());
        assert(ConstIdx && "struct indices must be constant");
        auto *SL = DL.getStructLayout(STy);
        Offset += SL->getElementOffset(ConstIdx->getZExtValue());
      }
      LastTy = Ty;
    }
    if (auto *ST = dyn_cast<StructType>(OriginTy))
      return {ArrayIndices, ST, Offset};
    return {{}, nullptr, 0};
  }

  // Check if Base is getelementptr inst, and if it is, save indices and find
  // true base. If new indices and argument indices are different, it means they
  // are from two different bases.
  Value *maybeGEPBaseAndCheckWith(Value *&Base, uint64_t &BaseOff,
                                  SmallVectorImpl<Value *> &Indices) {
    if (auto *GEP = dyn_cast<GetElementPtrInst>(Base)) {
      auto [ArrayIndices, ETy, Offset] = getGEPArrayIndicesAndOffset(GEP);
      if (!ETy)
        return nullptr;
      assert(ArrayIndices.size() != 0 &&
             "ETy != nullptr and ArrayIndices must not be empty");
      BaseOff = Offset;
      Value *Ret = Base;
      Base = GEP->getPointerOperand()->stripPointerCasts();
      if (!Indices.empty())
        return Indices == ArrayIndices ? Ret : nullptr;
      Indices = ArrayIndices;
      return Ret;
    }
    return Base;
  }

  bool isIntervalsCovered(Value *UnderObj, StructType *ST,
                          SmallVectorImpl<IVInfo> &Intervals) {
    if (Intervals.empty())
      return false;
    sort(Intervals, [](const IVInfo &L, const IVInfo &R) {
      return L.Src.first < R.Src.first;
    });
    uint64_t SR = Intervals[0].Src.second;
    uint64_t DR = Intervals[0].Dst.second;
    for (unsigned I = 1; I < Intervals.size(); ++I) {
      if (Intervals[I].Src.first != SR || Intervals[I].Dst.first != DR)
        return false;
      SR = Intervals[I].Src.second;
      DR = Intervals[I].Dst.second;
    }
    auto FirstE = Intervals.front();
    auto LastE = Intervals.back();
    auto Size = DL.getStructLayout(ST)->getSizeInBytes();
    if (LastE.Src.second - FirstE.Src.first != Size ||
        LastE.Dst.second - FirstE.Dst.first != Size)
      return false;
    return true;
  }

  bool filterValueCopySrc(Value *Src) {
    SetVector<AllocaInst *> AllocaBase;
    SetVector<Value *> Cache;
    if (!isBaseAllocaValue(Src, AllocaBase, Cache))
      return false;
    for (auto *AI : AllocaBase) {
      SetVector<Value *> BaseUses;
      SetVector<Instruction *> BaseStoreMemcpy;
      if (!getBaseUses(AI, BaseUses, BaseStoreMemcpy))
        return false;
      if (!isStackValueSrc(BaseStoreMemcpy))
        return false;
    }
    return true;
  }

  Instruction *createIntervalBeginGEP(IRBuilder<> &IRB, Value *UnderObj,
                                      IVInfo &FirstE) {
    if (FirstE.GEP)
      return FirstE.GEP;
    Value *BC = IRB.CreateBitCast(
        UnderObj,
        IRB.getInt8PtrTy(UnderObj->getType()->getPointerAddressSpace()));
    auto *GEP = IRB.CreateGEP(
        IRB.getInt8Ty(), BC,
        {ConstantInt::get(IRB.getInt32Ty(), FirstE.Src.first)}, "", true);
    // We do not remove possibly redundant def instructions, because their use
    // needs to be analyzed. Therefore, they are removed in subsequent passes.
    return cast<Instruction>(GEP);
  }

  Instruction *getCompleteClobbers(MemoryLocation &Loc, MemoryAccess *OriginMA,
                                   StructType *ST, uint64_t Limits = 5) {
    assert(isa<MemoryUseOrDef>(OriginMA));
    CaptureInfo *CI = &EI;
    auto *StartDef = dyn_cast<MemoryDef>(
        cast<MemoryUseOrDef>(OriginMA)->getDefiningAccess());
    if (!StartDef || !StartDef->getMemoryInst())
      return nullptr;
    auto *MemI = StartDef->getMemoryInst();
    MemoryDef *Current = dyn_cast<MemoryDef>(
        MSSA.getWalker()->getClobberingMemoryAccess(StartDef, Loc));

    SmallVector<IVInfo, 8> Intervals;
    Value *UnderObj = nullptr;
    Instruction *LastClobberI = nullptr;
    SmallVector<Value *, 8> ArrayIndices;
    // If 2 steps has covered, and 3th step is repeat.
    bool IntervalsValid = false;
    for (; Current && !MSSA.isLiveOnEntryDef(Current) && Limits--;
         Current = dyn_cast<MemoryDef>(Current->getDefiningAccess())) {
      Instruction *I = Current->getMemoryInst();
      if (!I || (I != MemI && !DT.dominates(I, MemI)) || I->mayThrow() ||
          !I->willReturn())
        break;
      if (!LastClobberI)
        LastClobberI = Current->getMemoryInst();
      // Get ModRefInfo between I and Loc.Ptr, we only deal with MustMod cases.
      ModRefInfo MR = BatchAA.getModRefInfo(I, Loc);
      if (isNoModRef(MR) || !isModSet(MR))
        continue;

      auto [Src, Dst, Size] = getSrcDstPtrAndSize(DL, I);
      // Use find under object to instead of must alias analysis. And if memset
      // or cj.memset is displayed (Src and Dst is nullptr), stop the analysis.
      if (!Src || !Dst)
        break;
      if (!isMustSet(MR) && getUnderlyingObject(Dst) != Loc.Ptr) {
        if (CI->isNotCapturedBeforeOrAt(Loc.Ptr, I))
          continue;
        break;
      }
      assert((isa<StoreInst>(I) || isa<MemCpyInst>(I)) &&
             "Only support to deal store or memcpy");
      auto [SrcBPtr, SrcOff] = getBasePtrAndOffset(DL, Src, I, &LastClobberI);
      auto [DstBPtr, DstOff] = getBasePtrAndOffset(DL, Dst, I);
      if (!SrcBPtr || !DstBPtr)
        break;

      // %0 = gep %base, i64 0, i32 1, i64 %231, i32 0, i32 0
      // %1 = gcread.ref(%base, %0)
      // %2 = gep %dest, 0
      // store i8 addrspace(1)* %1, %dest
      // %3 = gep %base, i64 0, i32 1, i64 %231, i32 0, i32 1
      // %4 = gep %dest, 8
      // memcpy(%4, %3, 24)
      // Dealing with the situation.
      GetElementPtrInst *GEP = nullptr;
      if (auto *R = maybeGEPBaseAndCheckWith(SrcBPtr, SrcOff, ArrayIndices))
        GEP = dyn_cast<GetElementPtrInst>(R);
      else
        break;

      // The source values of the loc are from different memory locations. In
      // this scenario, optimization also cannot be performed. Because the AA
      // result is MustAlias, DstBasePtr does not need to be verified.
      if (!UnderObj)
        UnderObj = SrcBPtr;
      if (UnderObj != SrcBPtr)
        break;

      Intervals.push_back(
          {{SrcOff, SrcOff + Size}, {DstOff, DstOff + Size}, I, GEP});
      if (isIntervalsCovered(UnderObj, ST, Intervals)) {
        IntervalsValid = true;
        break;
      }
    }
    if (!IntervalsValid && !isIntervalsCovered(UnderObj, ST, Intervals))
      return nullptr;
    if (hasMemoryDefBetween(MSSA, DT, DL, UnderObj, LastClobberI, OriginMA))
      return nullptr;
    if (!filterValueCopySrc(UnderObj))
      return nullptr;
    IRBuilder<> IRB(MemI);
    return createIntervalBeginGEP(IRB, UnderObj, Intervals.front());
  }

  Instruction *getClobberMemoryAccess(CallBase *CB, unsigned Index) {
    MSSA.ensureOptimizedUses();
    MemoryAccess *OriginMA = MSSA.getMemoryAccess(CB);
    Instruction *I = cast<Instruction>(CB->getArgOperand(Index));
    auto *AI = dyn_cast<AllocaInst>(I);
    assert(AI != nullptr);
    auto Loc = MemoryLocation(
        I, LocationSize::precise(DL.getTypeAllocSize(AI->getAllocatedType())));
    StructType *ST = cast<StructType>(AI->getAllocatedType());
    Instruction *NewI = getCompleteClobbers(Loc, OriginMA, ST);
    return NewI;
  }

  void replaceCallArg(CallBase *CB, unsigned ArgIndex, Instruction *NewI) {
    IRBuilder<> Builder(cast<Instruction>(CB));
    auto *Arg = CB->getArgOperand(ArgIndex);
    Value *CastI = NewI;
    if (NewI->getType()->getPointerAddressSpace() != 0) {
      CastI = Builder.CreateAddrSpaceCast(NewI,
                                          PointerType::get(Builder.getContext(), 0));
    }
    Value *Cast = Builder.CreateBitCast(CastI, Arg->getType());
    CB->setArgOperand(ArgIndex, Cast);
  }

  bool optimizeCallInst(CallBase *CB) {
    bool Changed = false;
    Function *Func = CB->getCalledFunction();
    if (Func == nullptr || !Func->hasFnAttribute(Attribute::ReadOnly))
      return Changed;
    SetVector<unsigned> CallIndexs;
    filterCallArgs(CB, CallIndexs);
    for (auto Index : CallIndexs) {
      if (Instruction *NewI = getClobberMemoryAccess(CB, Index)) {
        replaceCallArg(CB, Index, NewI);
        Changed = true;
      }
    }
    return Changed;
  }
};

// This optimization achieves the following copy elimination.
//   copy(b, a, size)
//   copy(c, a, size)
//   use(c)
// When a is a GCPtr, source propagation cannot be applied. In this case, try
// replacing c with b.
// It is worth mentioning that in this optimization, "copy" consists of multiple
// define instructions, including memcpy/gcwrite.struct/store. Their dests must
// be the same, but their sources may be different.
struct DestPropagationState {
  Function &F;

  AliasAnalysis &AA;
  EarliestEscapeInfo EI;
  BatchAAResults BatchAA;
  MemorySSA &MSSA;

  DominatorTree &DT;
  LoopInfo &LI;
  const DataLayout &DL;
  uint32_t TotalVersion = 0;

  SmallPtrSet<const Value *, 32> EphValues;

  struct Copy {
    Value *Dst = nullptr;
    Interval IV;
    Instruction *I = nullptr;
    uint32_t Version = UINT32_MAX;
    Copy() : Dst(nullptr), IV({0, 0}), I(nullptr) {}
    Copy(Value *Dst, Interval IV, Instruction *I, uint32_t TotalVersion)
        : Dst(Dst), IV(IV), I(I), Version(TotalVersion) {}
    Copy(const Copy &C) : Dst(C.Dst), IV(C.IV), I(C.I), Version(C.Version) {}
  };
  struct Compare {
    // Be careful, LHS < RHS false + RHS < LHS false => LHS == RHS.
    bool operator()(const Copy *LHS, const Copy *RHS) const {
      if (LHS->Dst == RHS->Dst && LHS->IV == RHS->IV && LHS->I == RHS->I)
        return false;
      if (LHS->IV.first == RHS->IV.first) {
        if (LHS->IV.second == RHS->IV.second) {
          if (LHS->I == RHS->I)
            llvm::report_fatal_error("error");
          return LHS->Version < RHS->Version;
        }
        return LHS->IV.second > RHS->IV.second;
      }
      return LHS->IV.first < RHS->IV.first;
    }
  };
  DenseMap<BasicBlock *, DenseMap<Value *, SmallSet<Copy *, 0, Compare>>>
      SourceCopys;
  SmallSet<Copy *, 16> MemoryLeakSet;

  struct Define {
    Value *Source = nullptr;
    Copy *C;
    Interval IV;
  };
  struct ClobberInfo {
    SmallVector<Define, 8> Defines;
    // For the source on the destination, record the last define instruction
    // from the source.
    DenseMap<Value *, Instruction *> Sources;
    // Records the last define instruction of the destination.
    Instruction *LastClobberI = nullptr;
  };
  // 1: Records all defined information after combination.
  DenseMap<BasicBlock *, DenseMap<Value *, ClobberInfo>> Clobbers;
  // size_t: index of ClobberInfo.Defines.
  DenseMap<BasicBlock *, unsigned> Successors; // Single predecessor successors

  DestPropagationState(Function &F, AliasAnalysis &AA, MemorySSA &MSSA,
                       DominatorTree &DT, LoopInfo &LI)
      : F(F), AA(AA), EI(DT, LI, EphValues), BatchAA(AA, &EI), MSSA(MSSA),
        DT(DT), LI(LI), DL(F.getParent()->getDataLayout()) {}

#ifndef NDEBUG
  void insertNewMem(Copy *C) { MemoryLeakSet.insert(C); }

  void eraseReleaseMem(Copy *C) {
    assert(MemoryLeakSet.count(C));
    MemoryLeakSet.erase(C);
  }

  void checkMemLeak() {
    if (!MemoryLeakSet.empty()) {
      LLVM_DEBUG(dbgs() << F.getName() << ", memory leakage occurs"
                        << "\n");
      for (auto *C : MemoryLeakSet)
        LLVM_DEBUG(dbgs() << C << "\n");
    }
  }
#endif

  // Check whether I is store, memcpy, memmove, or gcread.struct, and check
  // whether define source is an on-stack struct.
  bool isStackSTDefine(Instruction *I) {
    Value *Ptr = nullptr;
    if (auto *SI = dyn_cast<StoreInst>(I))
      Ptr = getUnderlyingObject(SI->getPointerOperand());
    else if (auto *MCI = dyn_cast<MemCpyInst>(I))
      Ptr = getUnderlyingObject(MCI->getDest());
    else if (auto *MMI = dyn_cast<MemMoveInst>(I))
      Ptr = getUnderlyingObject(MMI->getDest());
    else if (auto *II = dyn_cast<IntrinsicInst>(I);
             II && (II->getIntrinsicID() == Intrinsic::cj_gcread_struct ||
                    II->getIntrinsicID() == Intrinsic::cj_gcread_static_struct))
      Ptr = getUnderlyingObject(II->getOperand(0));
    if (auto *AI = dyn_cast_or_null<AllocaInst>(Ptr))
      return isa<StructType>(AI->getAllocatedType());
    return false;
  }

  bool isMemoryInstruction(Instruction *I) {
    if (isa<StoreInst>(I) || isa<LoadInst>(I) || isa<MemIntrinsic>(I))
      return true;
    if (auto *II = dyn_cast<IntrinsicInst>(I);
        II && (II->isCJRefGCRead() || II->isCJRefGCWrite() ||
               II->isCJStructGCRead() || II->isCJStructGCWrite() ||
               II->getIntrinsicID() == Intrinsic::cj_memset))
      return true;
    return false;
  }

  void shrinkLeft(Define &Def, int64_t Diff, BasicBlock *BB) {
    Def.IV.first += Diff;
    SourceCopys[BB][Def.Source].erase(Def.C);
    Def.C->IV.first += Diff;
    SourceCopys[BB][Def.Source].insert(Def.C);
  }

  void shrinkRight(Define &Def, int64_t Diff, BasicBlock *BB) {
    Def.IV.second -= Diff;
    SourceCopys[BB][Def.Source].erase(Def.C);
    Def.C->IV.second -= Diff;
    SourceCopys[BB][Def.Source].insert(Def.C);
  }

  std::pair<Define, Define> splitTwo(Define &Def, Interval IV, BasicBlock *BB) {
    if (IV.first == Def.IV.first || IV.second == Def.IV.second)
      llvm::report_fatal_error("error");
    Interval OldDIV = Def.IV;
    Interval OldSIV = Def.C->IV;
    shrinkRight(Def, Def.IV.second - IV.first, BB);
    Copy *C = new Copy;
#ifndef NDEBUG
    insertNewMem(C);
#endif
    C->Dst = Def.C->Dst;
    C->IV = {OldSIV.first + IV.second - OldDIV.first, OldSIV.second};
    C->I = Def.C->I;
    C->Version = Def.C->Version;
    SourceCopys[BB][Def.Source].insert(C);
    return {Def, {Def.Source, C, {IV.second, OldDIV.second}}};
  }

  // true: RD is push_back to Splited.
  // Discard: release RD.C
  // In addition, when the function returns, LD is always not needed.
  bool trySplit(Define &LD, Define &RD, SmallVectorImpl<Define> &Splited,
                Instruction *I, bool &Discard) {
    // case0: No split needed.
    if (!(LD.IV.second >= RD.IV.first && LD.IV.first <= RD.IV.second)) {
      if (LD.IV.first < RD.IV.first) {
        Splited.push_back(LD);
        Splited.push_back(RD);
      } else {
        Splited.push_back(RD);
        Splited.push_back(LD);
      }
      return true;
    }
    auto *BB = I->getParent();
    // case1: LD is full covered by RD, just delete from SourceCopys.
    if (LD.IV.first >= RD.IV.first && LD.IV.second <= RD.IV.second) {
      SourceCopys[BB][LD.Source].erase(LD.C);
      delete LD.C;
#ifndef NDEBUG
      eraseReleaseMem(LD.C);
#endif
      return false;
    }
    // Quick check whether LD.Source is still valid.
    if (LD.Source != RD.Source && // This case has been checked.
        hasMemoryDefBetween(MSSA, DT, DL, LD.Source, LD.C->I,
                            MSSA.getMemoryAccess(I), true, true)) {
      SourceCopys[BB][LD.Source].erase(LD.C);
      delete LD.C;
#ifndef NDEBUG
      eraseReleaseMem(LD.C);
#endif
      return false;
    }
    // case2: RD is full covered by LD.
    if (LD.IV.first < RD.IV.first && LD.IV.second > RD.IV.second) {
      // Sources are different, therefore, LD should be splited or LD and RD are
      // from same source but mappings are different, LD also need to be
      // splited.
      if (LD.Source != RD.Source ||
          LD.C->IV.first - LD.IV.first != RD.C->IV.first - RD.IV.first) {
        auto [Def1, Def2] = splitTwo(LD, {RD.IV.first, RD.IV.second}, BB);
        Splited.push_back(Def1);
        Splited.push_back(RD);
        Splited.push_back(Def2);
        return true;
      }
      // Both source and mapping are same, just keep LD and discard RD.
      Splited.push_back(LD);
      Discard = true; // RD is no needed.
      return true;
    }
    // case3: LD is left covered by RD.
    if (LD.IV.second > RD.IV.second) {
      // Sources are different or mappings are different.
      if (LD.Source != RD.Source ||
          LD.C->IV.first - LD.IV.first != RD.C->IV.first - RD.IV.first) {
        Splited.push_back(RD);
        shrinkLeft(LD, RD.IV.second - LD.IV.first, BB);
        Splited.push_back(LD);
        return true;
      }
      // Merge LD and RD.
      shrinkLeft(LD, -(LD.IV.first - RD.IV.first), BB);
      Splited.push_back(LD);
      Discard = true; // RD is no needed.
      return true;
    }
    // case4: LD is right covered by RD.
    if (LD.IV.first < RD.IV.first) {
      if (LD.Source != RD.Source ||
          LD.C->IV.first - LD.IV.first != RD.C->IV.first - RD.IV.first) {
        shrinkRight(LD, LD.IV.second - RD.IV.first, BB);
        Splited.push_back(LD);
        return false;
      }
      shrinkLeft(RD, -(RD.IV.first - LD.IV.first), BB);
      SourceCopys[BB][LD.Source].erase(LD.C); // LD is always in SourceCopys
      delete LD.C;
#ifndef NDEBUG
      eraseReleaseMem(LD.C);
#endif
      return false;
    }
    llvm::report_fatal_error("unreachable");
    return false;
  }

  // This function attempts to insert a new interval into an existing interval
  // set, and splits the existing interval or merges the new interval if
  // necessary. This usually happens in the following situations.
  //
  // case1:
  // L1-----R1(S1)
  //               L2-----R2(S2)
  //     NL-----------NR(S3)
  // Now, intervals are [L1, NL](S1), [NL, NR](S3), [NR, R2](S2).
  //
  // case2:
  // L1-----R1(S1)
  //      NL-----NR(S1)
  // Now, new interval is [L1, NR](S1). If one or more sources in case1 are the
  // same, merged in case2 is triggered.
  //
  // case3:
  // L1-----R1(S1)     from     SL1-----SR1
  //        NL-----NR(S1)       from          SL2-----SR2
  // SL2 - NL is not equal to SL1 - L1
  // Note that the intervals do not merge in this case, because this results in
  // S1 not being continuous but acting on consecutive dests.
  //
  // case4:
  // L1-----------R1(S1)
  //     NL---NR(S2)
  // Now, new intervals are [L1, NL](S1), [NL, NR](S2), [NR, R1](S1). The old
  // interval was truncated, similar to case1.
  //
  // return true indicates RD.C should be released.
  bool intervalSplitOrMerge(SmallVectorImpl<Define> &Defines, const Define &New,
                            Instruction *Inst) {
    SmallVector<Define, 8> NewDefines;
    bool Placed = false;
    size_t J = Defines.size();
    bool Discard = false;
    for (size_t I = 0; I < Defines.size(); ++I) {
      if (Defines[I].IV.second < New.IV.first) {
        NewDefines.push_back(Defines[I]);
        continue;
      }
      if (Defines[I].IV.first > New.IV.second) {
        if (!Placed) {
          NewDefines.push_back(New);
          SourceCopys[Inst->getParent()][New.Source].insert(New.C);
          Placed = true;
        }
        NewDefines.push_back(Defines[I]);
        continue;
      }
      J = I;
      break;
    }
    for (; J < Defines.size(); ++J) {
      if (!Placed) {
        if (trySplit(Defines[J], const_cast<Define &>(New), NewDefines, Inst,
                     Discard))
          Placed = true;
        continue;
      }
      NewDefines.push_back(Defines[J]);
    }
    if (!Placed) {
      NewDefines.push_back(New);
      SourceCopys[Inst->getParent()][New.Source].insert(New.C);
    }
    Defines = NewDefines;
    return Discard;
  }

  void invalidClobber(Value *Dest, DenseMap<Value *, ClobberInfo> &Clobbers,
                      DenseMap<Value *, SmallSet<Copy *, 0, Compare>> &Copies) {
    for (auto &Def : Clobbers[Dest].Defines) {
      if (!Copies.count(Def.Source))
        llvm::report_fatal_error("something error between Clobber and Copies");
      Copies[Def.Source].erase(Def.C);
      delete Def.C;
#ifndef NDEBUG
      eraseReleaseMem(Def.C);
#endif
    }
    Clobbers.erase(Dest);
  }

  void invalidSource(Value *Source,
                     DenseMap<Value *, SmallSet<Copy *, 0, Compare>> &Copies,
                     DenseMap<Value *, ClobberInfo> &Clobbers) {
    assert(Copies.count(Source));
    for (auto *C : Copies[Source]) {
      if (!Clobbers.count(C->Dst)) {
        delete C;
#ifndef NDEBUG
        eraseReleaseMem(C);
#endif
        continue;
      }
      SmallVector<Define, 8> NewDefines;
      for (auto &Def : Clobbers[C->Dst].Defines) {
        if (Def.Source == Source)
          continue;
        NewDefines.push_back(Def);
      }
      Clobbers[C->Dst].Sources.erase(Source);
      if (NewDefines.empty())
        Clobbers.erase(C->Dst);
      else
        Clobbers[C->Dst].Defines = NewDefines;
      delete C;
#ifndef NDEBUG
      eraseReleaseMem(C);
#endif
    }
    Copies.erase(Source);
  }

  // This function try to insert and merge a new define. For a new define
  // instruction, find the copy interval of source and dest, and try to merge
  // the old define interval of dest. There may be two defined from different
  // sources, or the two defined have memory intersections. In this case, choose
  // to split the previous define.
  bool tryToInsertNewDefine(Instruction *I) {
    // Find base pointer of src and dst, and get define size.
    auto [Src, Dst, Size] = getSrcDstPtrAndSize(DL, I);
    if (!Src || !Dst)
      return false;
    auto [SrcBPtr, SrcOff] = getBasePtrAndOffset(DL, Src, I);
    auto [DstBPtr, DstOff] = getBasePtrAndOffset(DL, Dst, I);
    if (!SrcBPtr || !DstBPtr)
      return false;
    auto *BB = I->getParent();
    auto &ClobberB = Clobbers[BB][DstBPtr];
    if (ClobberB.Sources.count(SrcBPtr) &&
        hasMemoryDefBetween(MSSA, DT, DL, SrcBPtr, ClobberB.Sources[SrcBPtr],
                            MSSA.getMemoryAccess(I)))
      // SrcBPtr is clobbered, so all Src copy destinations need to be
      // invalidated.
      invalidSource(SrcBPtr, SourceCopys[BB], Clobbers[BB]);
    if (ClobberB.LastClobberI &&
        hasMemoryDefBetween(MSSA, DT, DL, DstBPtr, ClobberB.LastClobberI,
                            MSSA.getMemoryAccess(I), false))
      // DstBPtr is clobbered. Therefore, all clobber states of Dst need to be
      // cleared.
      invalidClobber(DstBPtr, Clobbers[BB], SourceCopys[BB]);
    auto &ClobberA = Clobbers[BB][DstBPtr]; // ClobberB may be invalid, rebind.
    Copy *C = new Copy(DstBPtr, {SrcOff, SrcOff + Size}, I, TotalVersion++);
#ifndef NDEBUG
    insertNewMem(C);
#endif
    if (!intervalSplitOrMerge(ClobberA.Defines,
                              {SrcBPtr, C, {DstOff, DstOff + Size}}, I)) {
      SourceCopys[BB][SrcBPtr].insert(C);
    } else {
      if (SourceCopys[BB][SrcBPtr].count(C)) // May be inserted when merge.
        SourceCopys[BB][SrcBPtr].erase(C);
      // C is discarded, release it.
      delete C;
#ifndef NDEBUG
      eraseReleaseMem(C);
#endif
    }
    ClobberA.Sources[SrcBPtr] = I; // Update Source last define.
    ClobberA.LastClobberI = I;     // Update dest last define.
    return true;
  }

  bool rewriteCallArg(CallBase *CB, unsigned ArgNo, Value *Base,
                      unsigned Offset) {
    // Current implementation may create many redundant GEP, which need to be
    // deleted by SimplifyCFG or InstCombine later.
    Value *Ptr = nullptr;
    IRBuilder<> IRB(CB);
    if (Offset == 0) { // Just bitcast
      Ptr = IRB.CreatePointerBitCastOrAddrSpaceCast(
          Base, CB->getArgOperand(ArgNo)->getType());
    } else {
      auto *BC = IRB.CreateBitCast(
          Base, IRB.getInt8PtrTy(Base->getType()->getPointerAddressSpace()));
      auto *GEP =
          IRB.CreateGEP(IRB.getInt8Ty(), BC,
                        {ConstantInt::get(IRB.getInt32Ty(), Offset)}, "", true);
      Ptr = IRB.CreatePointerBitCastOrAddrSpaceCast(
          GEP, CB->getArgOperand(ArgNo)->getType());
    }
    assert(Ptr != nullptr);
    CB->setArgOperand(ArgNo, Ptr);
    return true;
  }

  SmallVector<Define, 8>::iterator
  findSuitableDefine(BasicBlock *BB, Value *Dst, uint64_t Offset) {
    if (!Clobbers[BB].count(Dst))
      return nullptr;
    SmallVector<Define, 8>::iterator DI = nullptr;
    for (auto I = Clobbers[BB][Dst].Defines.begin(),
              E = Clobbers[BB][Dst].Defines.end();
         I != E; ++I) {
      // Find suitable define.
      if (I->IV.first <= Offset && I->IV.second >= Offset) {
        DI = I;
        break;
      }
    }
    return DI;
  }

  MemoryAccess *findLastAccessDominateI(BasicBlock *BB, Instruction *I) {
    if (auto *MA = MSSA.getMemoryAccess(I))
      return MA;
    auto *AccessLists = MSSA.getBlockAccesses(BB);
    if (!AccessLists)
      return nullptr;
    for (auto AI = AccessLists->rbegin(), AE = AccessLists->rend(); AI != AE;
         ++AI) {
      if (auto *MUD = dyn_cast<MemoryUseOrDef>(&*AI);
          MUD && DT.dominates(MUD->getMemoryInst(), I))
        return const_cast<MemoryAccess *>(&*AI);
    }
    return nullptr;
  }

  std::pair<Value *, int64_t> findFullEquivalentValue(Value *Ptr, uint64_t Size,
                                                      BasicBlock *BB,
                                                      Instruction *Use) {
    APInt VOff(DL.getIndexSizeInBits(0u), 0);
    Value *V = Ptr->stripAndAccumulateConstantOffsets(DL, VOff, true);
    auto ContainCheck = [&](SmallVector<Define, 8>::iterator IT) -> int {
      uint64_t LastPos = IT->IV.second;
      if (LastPos >= VOff.getZExtValue() + Size)
        return 1;
      for (auto I = IT + 1, E = Clobbers[BB][V].Defines.end(); I != E; ++I) {
        if (I->IV.first != LastPos)
          return 0;
        LastPos = I->IV.second;
        if (LastPos >= VOff.getZExtValue() + Size)
          return std::distance(IT, I) + 1;
      }
      return 0;
    };
    auto FindCopyBelongDefine = [](ClobberInfo &CI, Copy *C) -> Define * {
      for (auto &D : CI.Defines)
        if (D.C == C)
          return &D;
      return nullptr;
    };
    // Find the definition of V that contains VOff.
    auto DI = findSuitableDefine(BB, V, VOff.getZExtValue());
    if (!DI)
      return {nullptr, 0};
    // Ensure that all consecutive memories between [VOff, VOff + Size] have
    // corresponding define.
    int DefCount = ContainCheck(DI);
    if (!DefCount)
      return {nullptr, 0};
    assert(Clobbers[BB].count(V));
    assert(SourceCopys[BB].count(DI->Source) && "something error");

    struct RAIIInvalidClobbers {
      SmallVector<Value *, 4> InvalidClobbers;
      DestPropagationState *State;
      DenseMap<Value *, ClobberInfo> &Clobbers;
      DenseMap<Value *, SmallSet<Copy *, 0, Compare>> &Copies;
      RAIIInvalidClobbers(
          DestPropagationState *State, DenseMap<Value *, ClobberInfo> &Clobbers,
          DenseMap<Value *, SmallSet<Copy *, 0, Compare>> &Copies)
          : State(State), Clobbers(Clobbers), Copies(Copies) {}
      ~RAIIInvalidClobbers() {
        for (auto *V : InvalidClobbers)
          State->invalidClobber(V, Clobbers, Copies);
      }
    } RAIIInvalidClobbers(this, this->Clobbers[BB], this->SourceCopys[BB]);

    //  Check where the source is copied. For each copy destination(d0), if
    //  the copy range can cover the required range and neither d0 nor source
    //  has clobber instructions between the last copy and current use, then
    //  it is safe to replace it with the last copy.
    auto *MA = findLastAccessDominateI(BB, Use);
    if (MA && hasMemoryDefBetween(MSSA, DT, DL, V, Clobbers[BB][V].LastClobberI,
                                  MA, false, true)) {
      RAIIInvalidClobbers.InvalidClobbers.push_back(V);
      return {nullptr, 0};
    }
    for (auto *C : SourceCopys[BB][DI->Source]) {
      if (C == DI->C) // Try to use the previous equivalent Value.
        break;
      auto &CandiClob = Clobbers[BB][C->Dst];
      // Replace C->I with CandiClob(d0).LastClobberI is safe and can be
      // acceleration because it is confirmed that d0 has not clobbered
      // before d0.LastClobberI when a new define is added to d0. If it is,
      // the state of d0 will be cleared and d0 is not found through source.
      if (MA && hasMemoryDefBetween(      // MA == nullptr, must no clobber.
                    MSSA, DT, DL, C->Dst, // It can be accelerated here.
                    CandiClob.LastClobberI, MA, true, true)) {
        RAIIInvalidClobbers.InvalidClobbers.push_back(C->Dst);
        continue;
      }
      auto *CandiDI = FindCopyBelongDefine(CandiClob, C);
      assert(CandiDI != nullptr);
      int I = 0;
      for (; I < DefCount; ++I) {
        auto *CandiDef =
            CandiDI + I == CandiClob.Defines.end() ? nullptr : CandiDI + I;
        if (!CandiDef)
          break;
        auto *Def = DI + I;
        // I == 0, Copy left can be LE, otherwise, left must be equal.
        // I == DefCount - 1, Copy right can be GE, otherwise, right must be
        // equal.
        bool LeftValid =
            (I == 0 && CandiDef->C->IV.first <= Def->C->IV.first) ||
            CandiDef->C->IV.first == Def->C->IV.first;
        bool RightValid = (I == DefCount - 1 &&
                           CandiDef->C->IV.second >= Def->C->IV.second) ||
                          CandiDef->C->IV.second == Def->C->IV.second;
        // For the same reason, it has been proven safe to replace C->I with
        // d0.Sources[source] when adding a new define from source to d0.
        auto *SMA = MSSA.getMemoryAccess(Clobbers[BB][V].Sources[Def->Source]);
        assert(SMA && "Must has memory access");
        if (LeftValid && RightValid && CandiDef->Source == Def->Source &&
            // Be sure that Source has not been modified between the last use of
            // CandiClob and CurrentClob.
            !hasMemoryDefBetween(MSSA, DT, DL, Def->Source,
                                 CandiClob.Sources[Def->Source], SMA, true,
                                 true)) {
          continue;
        }
        break;
      }
      // All defines of [VOff, VOff + Size] in V have the equivalent define in
      // CandiClob.
      if (I == DefCount) {
        //      L0----------R0 (candidate first define interval)
        // L1---------------R1 (candidate first copy interval)
        //    L2--------R2     (V first copy interval)
        //      L3---R3        (V first define interval)
        //         ^ (P: Ptr offset based on V)
        // The final offset for replacing Ptr with candi is
        // L0 + L2 - L1 + P - L3.
        return {C->Dst, CandiDI->IV.first + DI->C->IV.first - C->IV.first +
                            VOff.getZExtValue() - DI->IV.first};
      }
    }
    return {nullptr, 0};
  }

  bool tryToReplaceCallArgs(CallBase *CB) {
    bool Changed = false;
    BasicBlock *BB = CB->getParent();
    bool FnReadOnly = CB->hasFnAttr(Attribute::ReadOnly) ||
                      CB->hasFnAttr(Attribute::ReadNone);
    for (auto &Arg : CB->args()) {
      bool ArgReadOnly =
          CB->paramHasAttr(Arg.getOperandNo(), Attribute::ReadOnly) ||
          CB->paramHasAttr(Arg.getOperandNo(), Attribute::ReadNone);
      bool ArgNoCaptured =
          CB->paramHasAttr(Arg.getOperandNo(), Attribute::NoCapture);
      // If readonly is false, argument may be modified and cannot be replaced
      // with another value. If nocaptured is false, the pointer information may
      // be out of scope and cannot be replaced.
      if (!(ArgReadOnly || FnReadOnly) || !ArgNoCaptured ||
          !Arg->getType()->isPointerTy())
        continue;
      Type *Ty = getKnownPointeeType(Arg);
      if (!Ty)
        continue;
      uint64_t Size = Ty->isFunctionTy() ? 8 : DL.getTypeAllocSize(Ty);
      auto [EV, Offset] = findFullEquivalentValue(Arg, Size, BB, CB);
      if (EV)
        Changed |= rewriteCallArg(CB, Arg.getOperandNo(), EV, Offset);
    }
    return Changed;
  }

  void copyBBState(BasicBlock *PredBB, BasicBlock *BB) {
    Clobbers[BB] = Clobbers[PredBB];
    for (auto &[Dst, CI] : Clobbers[BB]) {
      for (auto &Def : CI.Defines) {
        Copy *NewC = new Copy(*Def.C);
#ifndef NDEBUG
        insertNewMem(NewC);
#endif
        Def.C = NewC;
        SourceCopys[BB][Def.Source].insert(NewC);
      }
    }
  }
  void releaseBBState(BasicBlock *BB) {
    for (auto &[Src, Copies] : SourceCopys[BB]) {
      for (auto *C : Copies) {
        delete C;
#ifndef NDEBUG
        eraseReleaseMem(C);
#endif
      }
    }
    SourceCopys.erase(BB);
    Clobbers.erase(BB);
  }

  bool optimizeRSSCInBB(BasicBlock &BB) {
    SourceCopys[&BB].clear();
    Clobbers[&BB].clear();
    // BB has single predecessor, okay, we can use PredBB state to initialize BB
    // state, because all values in PredBB are valid in the BB.
    if (auto *PredBB = BB.getSinglePredecessor()) {
      copyBBState(PredBB, &BB);
      // All successors has been visited, release memory.
      if (--Successors[PredBB] == 0) {
        Successors.erase(PredBB);
        releaseBBState(PredBB);
      }
    }
    bool Changed = false;
    for (Instruction &I : BB) {
      if (isStackSTDefine(&I)) {
        assert(isa<StoreInst>(I) || isa<MemCpyInst>(I) || isa<MemMoveInst>(I) ||
               isa<IntrinsicInst>(I));
        tryToInsertNewDefine(&I);
        continue;
      }
      if (isMemoryInstruction(&I)) // No need to optimize these instruction
        continue;
      if (auto *CB = dyn_cast<CallBase>(&I))
        Changed |= tryToReplaceCallArgs(CB);
    }
    // Calculate the number of successor BBs which contain single predecessor.
    // If empty, release BB state.
    auto *TI = BB.getTerminator();
    for (unsigned N = 0; N < TI->getNumSuccessors(); ++N) {
      if (TI->getSuccessor(N)->getSinglePredecessor())
        ++Successors[&BB];
    }
    if (!Successors.count(&BB))
      releaseBBState(&BB);
    return Changed;
  }
};

static bool destPropagationImpl(Function &F, AliasAnalysis &AA, MemorySSA &MSSA,
                                DominatorTree &DT, LoopInfo &LI) {
  DestPropagationState State(F, AA, MSSA, DT, LI);
  bool Changed = false;
  SmallSet<BasicBlock *, 16> Visited;
  SmallVector<BasicBlock *, 8> Worklist = {&F.getEntryBlock()};
  Visited.insert(&F.getEntryBlock());
  while (!Worklist.empty()) {
    auto *BB = Worklist.pop_back_val();
    Changed |= State.optimizeRSSCInBB(*BB);
    for (auto *Succ : successors(BB)) {
      if (Visited.count(Succ))
        continue;
      Visited.insert(Succ);
      Worklist.push_back(Succ);
    }
  }
#ifndef NDEBUG
  State.checkMemLeak();
#endif

  return Changed;
}

static bool sourcePropagationImpl(Function &F, AliasAnalysis &AA,
                                  MemorySSA &MSSA, DominatorTree &DT,
                                  LoopInfo &LI) {
  SourcePropagationState State(F.getParent()->getDataLayout(), AA, MSSA, DT,
                               LI);
  bool Changed = false;
  for (auto I = inst_begin(F), E = inst_end(F); I != E;) {
    auto *CB = dyn_cast<CallBase>(&*I++);
    if (!CB)
      continue;
    Changed |= State.optimizeCallInst(CB);
  }
  return Changed;
}

static bool implCJRSSCE(Function &F, AliasAnalysis &AA, MemorySSA &MSSA,
                        DominatorTree &DT, LoopInfo &LI) {
  if (!EnableCJRSSCE)
    return false;
  bool Changed = sourcePropagationImpl(F, AA, MSSA, DT, LI);
  // MSSA is not updated, but this does not affect the correctness and
  // optimization effect.
  Changed |= destPropagationImpl(F, AA, MSSA, DT, LI);
  return Changed;
}

PreservedAnalyses CJRSSCEPass::run(Function &F, FunctionAnalysisManager &AM) {
  auto &AA = AM.getResult<AAManager>(F);
  auto &MSSA = AM.getResult<MemorySSAAnalysis>(F).getMSSA();
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &LI = AM.getResult<LoopAnalysis>(F);
  if (implCJRSSCE(F, AA, MSSA, DT, LI))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}
