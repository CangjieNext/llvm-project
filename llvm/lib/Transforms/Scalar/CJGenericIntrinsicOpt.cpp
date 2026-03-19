//===- CJInstanceOfOpt.cpp --------------------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This pass mainly implements the generic intrinsics optimization of cangjie.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJGenericIntrinsicOpt.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/MemorySSAUpdater.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CJIntrinsics.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/SafepointIRVerifier.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/CJFillMetadata.h"
#include "llvm/Transforms/Scalar/CJRSSCE.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"

#define DEBUG_TYPE "cj-generic-intrinsic-opt"

using namespace llvm;
using namespace cangjie;

static cl::opt<bool>
    EnableCopyOpt("enable-generic-copy-opt", cl::init(true), cl::Hidden,
                  cl::desc("Enable generic copy optimization"));

static void updateMSSA(MemorySSAUpdater &MSSAU, Instruction *Old,
                       Instruction *New) {
  auto *MA = cast<MemoryUseOrDef>(MSSAU.getMemorySSA()->getMemoryAccess(Old));
  auto *NewMA =
      MSSAU.createMemoryAccessBefore(New, MA->getDefiningAccess(), MA);
  MA->replaceAllUsesWith(NewMA);
  MSSAU.removeMemoryAccess(Old);
}

static bool isStackToStackCopy(Value *Src, Value *Dst) {
  Type *DstBaseTy = findMemoryBasePointer(Dst)->getType();
  Type *SrcBaseTy = findMemoryBasePointer(Src)->getType();
  if (!isGCPointerType(SrcBaseTy) && !isGCPointerType(DstBaseTy))
    return true;

  return false;
}

static void lowerToMemcpy(CallInst *CI, Value *SizeOp,
                          MemorySSAUpdater &MSSAU) {
  IRBuilder<> IRB(CI);
  SmallVector<Value *> Idxs;
  Instruction *NewInst = CI;
  Type *I8Ty = IRB.getInt8Ty();
  Type *I32Ty = IRB.getInt32Ty();

  switch (CI->getIntrinsicID()) {
  default:
    report_fatal_error("Unsupported process type!");
    break;
  case Intrinsic::cj_assign_generic: {
    Idxs.push_back(ConstantInt::get(I32Ty, 8)); // 8: typeinfo* size
    Value *DstPtr = IRB.CreateGEP(I8Ty, CI->getArgOperand(0), {Idxs});
    Value *SrcPtr = IRB.CreateGEP(I8Ty, CI->getArgOperand(1), {Idxs});
    NewInst = IRB.CreateMemCpy(DstPtr, Align(8), SrcPtr, Align(8), SizeOp);
    break;
  }
  case Intrinsic::cj_gcread_generic: {
    Idxs.push_back(ConstantInt::get(I32Ty, 8)); // 8: typeinfo* size
    Value *DstPtr = IRB.CreateGEP(I8Ty, CI->getArgOperand(0), {Idxs});
    NewInst = IRB.CreateMemCpy(DstPtr, Align(8), CI->getArgOperand(2), Align(8),
                               SizeOp);
    break;
  }
  case Intrinsic::cj_gcwrite_generic: {
    Idxs.push_back(ConstantInt::get(I32Ty, 8)); // 8: typeinfo* size
    Value *SrcPtr = IRB.CreateGEP(I8Ty, CI->getArgOperand(2), {Idxs});
    NewInst = IRB.CreateMemCpy(CI->getArgOperand(1), Align(8), SrcPtr, Align(8),
                               SizeOp);
    break;
  }
  }
  if (MDNode *MD = CI->getMetadata(LLVMContext::MD_tbaa)) {
    NewInst->setMetadata(LLVMContext::MD_tbaa, MD);
  }
  NewInst->setDebugLoc(CI->getDebugLoc());
  updateMSSA(MSSAU, CI, NewInst);
  CI->replaceAllUsesWith(NewInst);
  CI->eraseFromParent();
}

static bool lowerToMemcpy(CallInst *CI, uint64_t Size,
                          MemorySSAUpdater &MSSAU) {
  if (Size == 0)
    return false;
  auto *SizeOp = ConstantInt::get(Type::getInt64Ty(CI->getContext()), Size);
  lowerToMemcpy(CI, SizeOp, MSSAU);
  return true;
}

// void @llvm.cj.copy.generic (i8 AS(1)* dst, i8 AS(1)* src, i8* typeinfo)
// void @llvm.cj.gcread.generic (i8 AS(1)* dstPtr, i8 AS(1)* basePtr, i8 AS(1)*
// fieldPtr, i32 size)
// void @llvm.cj.gcwrite.generic (i8 AS(1)* basePtr, i8 AS(1)* fieldPtr, i8
// AS(1)* srcPtr, i32 size)
// If the copied memory do not contain ref, we can safely replace with memcpy.
static bool lowerGenericBarrier(CallInst *CI, GlobalVariable *GV,
                                MemorySSAUpdater &MSSAU) {
  if (GV->hasInitializer()) {
    TypeInfo Klass(GV);
    // Reference type does not call a GenericBarrier in actual.
    if (Klass.isReferenceType()) {
      return false;
    }
    // If it contains ref field, need use barrier intrinsic.
    if (Klass.hasRefField())
      return false;

    return lowerToMemcpy(CI, Klass.getSize(), MSSAU);
  }

  if (isPrimitiveTypeInfo(GV))
    return lowerToMemcpy(CI, getValueTypeSize(GV), MSSAU);

  StructType *ST = getTypeLayoutType(GV);
  assert(ST && "typeinfo layout info missing!");
  if (containsGCPtrType(ST)) {
    return false;
  }

  const DataLayout &DL = CI->getModule()->getDataLayout();
  uint64_t MemSize = DL.getStructLayout(ST)->getSizeInBytes();
  return lowerToMemcpy(CI, MemSize, MSSAU);
}

// llvm.cj.gcread.generic (i8 AS(1)* dstPtr, i8 AS(1)* basePtr,
// i8 AS(1)* fieldPtr, i32 size)
// llvm.cj.gcwrite.generic (i8 AS(1)* basePtr, i8 AS(1)* fieldPtr,
// i8 AS(1)* srcPtr, i32 size)
static bool processGenericBarrier(CallInst *CI, Value *DstOp, Value *SrcOp,
                                  Value *GenericVar, MemorySSAUpdater &MSSAU) {
  if (isStackToStackCopy(SrcOp, DstOp)) {
    lowerToMemcpy(CI, getSize(CI), MSSAU);
    return true;
  }
  RefValue Ref(GenericVar);
  if (GlobalVariable *TI = Ref.findTypeInfoGV()) {
    return lowerGenericBarrier(CI, TI, MSSAU);
  }
  return false;
}

// void @llvm.cj.assign.generic (i8 AS(1)* dst, i8 AS(1)* src, i8* typeinfo)
static bool processAssignGeneric(CallInst *CI, MemorySSAUpdater &MSSAU) {
  Value *TIOp = CI->getArgOperand(2); // 2: TypeInfo operand
  if (isStackToStackCopy(getSource(CI), getDest(CI))) {
    if (GlobalVariable *TI = findTypeInfoGV(TIOp)) {
      uint64_t Size = 0;
      if (isPrimitiveTypeInfo(TI)) {
        Size = getValueTypeSize(TI);
      } else {
        StructType *ST = getTypeLayoutType(TI);
        assert(ST && "typeinfo layout info missing!");
        const DataLayout &DL = CI->getModule()->getDataLayout();
        Size = DL.getStructLayout(ST)->getSizeInBytes();
      }
      return lowerToMemcpy(CI, Size, MSSAU);
    } else {
      auto *TIType = StructType::getTypeByName(CI->getContext(), "TypeInfo");
      IRBuilder<> IRB(CI);
      Value *TIPtr = IRB.CreateBitCast(TIOp, TIType->getPointerTo());
      SmallVector<Value *> Idxs;
      Idxs.push_back(IRB.getInt32(0));
      Idxs.push_back(IRB.getInt32(CIT_SIZE));
      Value *SizePtr = IRB.CreateGEP(TIType, TIPtr, Idxs);
      Value *Size = IRB.CreateLoad(IRB.getInt32Ty(), SizePtr);
      lowerToMemcpy(CI, Size, MSSAU);
    }
    return true;
  }
  if (auto *TI = findTypeInfoGV(TIOp))
    return lowerGenericBarrier(CI, TI, MSSAU);
  return false;
}

static bool eliminateReferenceChecking(Function &F, MemorySSAUpdater &MSSAU) {
  SmallVector<LoadInst *> LoadTIs;
  SmallVector<Instruction *> RemoveInsts;
  for (Instruction &I : instructions(F)) {
    if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
      if (LI->getMetadata("ti_load"))
        LoadTIs.push_back(LI);
      continue;
    }
  }
  for (LoadInst *LI : LoadTIs) {
    auto *Expr = dyn_cast<ConstantExpr>(LI->getPointerOperand());
    if (!Expr)
      continue;

    GlobalVariable *GV = dyn_cast<GlobalVariable>(Expr->getOperand(0));
    if (!GV || !GV->isCJTypeInfo())
      continue;

    Value *Res = TypeInfo::getReferenceType(GV);
    if (!Res)
      continue;

    for (User *U : LI->users()) {
      ICmpInst *ICMP = dyn_cast<ICmpInst>(U);
      if (!ICMP) {
        report_fatal_error("Load TypeInfo is used for icmp slt 0!");
      }
      // %x = icmp slt i8 %kind, 0
      if (ICMP->getSignedPredicate() != CmpInst::ICMP_SLT) {
        report_fatal_error("Load TypeInfo is used for icmp slt 0!");
      }
      ConstantInt *Var = dyn_cast<ConstantInt>(ICMP->getOperand(1));
      if (!Var->isNullValue()) {
        report_fatal_error("Load TypeInfo is used for icmp slt 0!");
      }
      ICMP->replaceAllUsesWith(Res);
      RemoveInsts.push_back(ICMP);
    }
    RemoveInsts.push_back(LI);
  }
  if (RemoveInsts.empty())
    return false;

  for (Instruction *I : RemoveInsts) {
    MSSAU.removeMemoryAccess(I);
    I->eraseFromParent();
  }
  return true;
}

static bool simplifyGenericInst(Function &F, MemorySSAUpdater &MSSAU) {
  bool Changed = false;
  if (!F.hasCangjieGC())
    return Changed;

  // Deleted the invalid reference type checking and unreachable branch
  // so that barrier optimization.
  Changed |= eliminateReferenceChecking(F, MSSAU);

  for (auto I = inst_begin(F); I != inst_end(F);) {
    auto *CI = dyn_cast<CallInst>(&*I++);
    if (!CI)
      continue;

    unsigned ID = CI->getIntrinsicID();
    switch (ID) {
    default:
      break;
    case Intrinsic::cj_assign_generic: {
      Changed |= processAssignGeneric(CI, MSSAU);
      break;
    }
    case Intrinsic::cj_gcread_generic: {
      Changed |= processGenericBarrier(CI, getDest(CI), getSource(CI),
                                       getDest(CI), MSSAU);
      break;
    }
    case Intrinsic::cj_gcwrite_generic: {
      Changed |= processGenericBarrier(CI, getDest(CI), getSource(CI),
                                       getSource(CI), MSSAU);
      break;
    }
    }
  }
  return Changed;
}

struct GenericCopyOpt {
  MemorySSA &MSSA;
  MemorySSAUpdater &MSSAU;
  DominatorTree &DT;
  AliasAnalysis &AA;
  const DataLayout &DL;

  DenseSet<Value *> Analyzed;

  Function *F;

  GenericCopyOpt(Function &F, MemorySSA &MSSA, MemorySSAUpdater &MSSAU,
                 DominatorTree &DT, AliasAnalysis &AA)
      : MSSA(MSSA), MSSAU(MSSAU), DT(DT), AA(AA),
        DL(F.getParent()->getDataLayout()), F(&F) {}

  bool canEnableDiffTIOpt(Value *TI0, Value *TI1) {
    auto *CI0 = dyn_cast_or_null<CallInst>(TI0);
    auto *CI1 = dyn_cast<CallInst>(TI1);
    return CI0 && CI1 && CI0 != CI1 && CI0->getCalledFunction() &&
           CI1->getCalledFunction() &&
           CI0->getCalledFunction() == CI1->getCalledFunction() &&
           CI0->getCalledFunction()->getName() == "CJ_MCC_GetOrCreateTypeInfo";
  }

  bool sameTIOpt(Value *TI, Value *TI1) {
    if (canEnableDiffTIOpt(TI, TI1)) {
      auto *CI0 = cast<CallInst>(TI->stripPointerCasts());
      auto *CI1 = cast<CallInst>(TI1->stripPointerCasts());
      CallInst *DeadCI = nullptr;
      if (DT.dominates(CI0, CI1)) {
        CI1->replaceAllUsesWith(CI0);
        DeadCI = CI1;
      } else if (DT.dominates(CI1, CI0)) {
        CI0->replaceAllUsesWith(CI1);
        DeadCI = CI0;
      }
      if (DeadCI) {
        TI = TI->stripPointerCasts() == DeadCI ? TI1 : TI;
        MSSAU.removeMemoryAccess(DeadCI);
        DeadCI->eraseFromParent();
        return true;
      }
    }
    return false;
  }

  std::pair<BasicBlock *, Value *>
  getPHINodeTranslate(MemoryPhi *Phi, unsigned Index, Value *V) {
    auto *BB = Phi->getIncomingBlock(Index);
    if (auto *PN = dyn_cast<PHINode>(V)) {
      for (unsigned I = 0, E = PN->getNumIncomingValues(); I != E; ++I) {
        if (PN->getIncomingBlock(I) == BB)
          return {BB, PN->getIncomingValue(I)};
      }
      return {nullptr, nullptr};
    }
    if (isa<Argument>(V) || isa<GlobalVariable>(V))
      return {BB, V};
    if (isa<Instruction>(V) &&
        DT.dominates(cast<Instruction>(V), BB->getTerminator()))
      return {BB, V};
    return {nullptr, nullptr};
  }

  // 0: base ptr, 1: derived ptr, 2: size
  std::tuple<Value *, Value *, Value *>
  processMemoryPhi(MemoryPhi *Phi, MemoryAccess *End, MemoryAccess *InitEnd,
                   MemoryLocation &Loc, Value *TI, bool &Changed) {
    SmallVector<Value *, 4> Bases;
    SmallVector<Value *, 4> Deriveds;
    SmallVector<Value *, 4> Sizes;
    SmallVector<BasicBlock *, 4> BBs;
    for (unsigned I = 0, E = Phi->getNumIncomingValues(); I != E; ++I) {
      auto [BB, EQV] =
          getPHINodeTranslate(Phi, I, const_cast<Value *>(Loc.Ptr));
      if (!BB || !EQV)
        return {nullptr, nullptr, nullptr};
      BasicBlock *IncomingBB = Phi->getIncomingBlock(I);
      if (isPotentiallyReachable(Phi->getBlock(), IncomingBB))
        return {nullptr, nullptr, nullptr}; // Maybe this is a loop.
      auto [TmpB, TmpD, TmpS] = findPotentialEqualMem(
          IncomingBB->getTerminator(),
          cast<MemoryAccess>(Phi->getIncomingValue(I)), InitEnd,
          MemoryLocation::getAfter(EQV), nullptr, TI, nullptr, Changed);
      if (!TmpD) {
        Bases.push_back(nullptr);
        Deriveds.push_back(EQV);
        Sizes.push_back(nullptr);
      } else {
        Bases.push_back(TmpB);
        Deriveds.push_back(TmpD);
        Sizes.push_back(TmpS);
      }
      BBs.push_back(BB);
    }
    // Check if all bases is nullptr, or non-null.
    Value *T = Bases.front();
    for (unsigned I = 1; I < Bases.size(); ++I)
      if ((T && !Bases[I]) || (!T && Bases[I]))
        return {nullptr, nullptr, nullptr};
    // Get available value for base, derived and size.
    SSAUpdater DerivedSSA, BaseSSA, SizeSSA;
    Value *AB = nullptr, *AS = nullptr;
    if (T) {
      assert(Sizes.front() != nullptr);
      BaseSSA.Initialize(Loc.Ptr->getType(), "");
      SizeSSA.Initialize(Sizes.front()->getType(), "");
      for (unsigned I = 0; I < Bases.size(); ++I) {
        BaseSSA.AddAvailableValue(BBs[I], Bases[I]);
        SizeSSA.AddAvailableValue(BBs[I], Sizes[I]);
      }
      AB = BaseSSA.GetValueInMiddleOfBlock(Phi->getBlock());
      AS = SizeSSA.GetValueInMiddleOfBlock(Phi->getBlock());
    }
    DerivedSSA.Initialize(Loc.Ptr->getType(), "");
    for (unsigned I = 0; I < Deriveds.size(); ++I)
      DerivedSSA.AddAvailableValue(BBs[I], Deriveds[I]);
    Value *AV = DerivedSSA.GetValueInMiddleOfBlock(Phi->getBlock());
    return {AB, AV, AS};
  }

  // Return the Value after strip.
  Value *getDerivedOffset(Value *Ptr,
                          SmallVector<std::pair<Value *, bool>, 4> &Offsets) {
    Ptr = Ptr->stripPointerCasts();
    GetElementPtrInst *GEP = nullptr;
    while ((GEP = dyn_cast<GetElementPtrInst>(Ptr))) {
      if (GEP->getNumIndices() == 1) {
        Offsets.push_back({GEP->getOperand(1), true});
        Ptr = GEP->getPointerOperand();
      }
      break;
    }
    return GEP ? GEP->getPointerOperand() : nullptr;
  }

  bool isConstantEqual(Value *LHS, Value *RHS) {
    if (!isa<ConstantInt>(LHS) || !isa<ConstantInt>(RHS))
      return false;
    return cast<ConstantInt>(LHS)->getSExtValue() ==
           cast<ConstantInt>(RHS)->getSExtValue();
  }

  // IsWrite: offset should be logical negation
  // For example:
  // gcwrite.generic(v1, v1.off, v0, s1)
  // gcread.generic(v2, v1, v1.off, s1)
  // When gcread.generic is analyzed, Offsets = {v1.off - v1}, then
  // gcwrite.generic is analyzed, and TmpOffsets = {v1.off - v1}. However, for
  // gcwrite.generic, the real TmpOffsets should be {-(v1.off - v1)}.
  void
  pushAndPopSameOffset(SmallVector<std::pair<Value *, bool>, 4> &Offsets,
                       const ArrayRef<std::pair<Value *, bool>> &TmpOffsets,
                       bool IsWrite = false) {
    for_each(TmpOffsets.begin(), TmpOffsets.end(),
             [&Offsets, IsWrite, this](const std::pair<Value *, bool> &VB) {
               bool Tag = IsWrite ? !VB.second : VB.second;
               if (!Offsets.empty() && VB.first == Offsets.back().first &&
                   Tag != Offsets.back().second)
                 Offsets.pop_back();
               else if (!Offsets.empty() &&
                        isConstantEqual(VB.first, Offsets.back().first) &&
                        Tag != Offsets.back().second)
                 Offsets.pop_back();
               else
                 Offsets.push_back({VB.first, Tag});
             });
  }

  std::tuple<Value *, Value *, Value *>
  findPotentialEqualMem(Instruction *Inst, MemoryAccess *End,
                        MemoryAccess *InitEnd, MemoryLocation Loc, Value *Base,
                        Value *TI, Value *Size, bool &Changed) {
    // MemoryPhi may appear in gcread.ref
    if (isa<MemoryPhi>(End))
      return {nullptr, nullptr, nullptr};
    // Offset of Loc.Ptr relative to Base, true: positive, false: negative
    SmallVector<std::pair<Value *, bool>, 4> Offsets;
    auto *BV = getDerivedOffset(const_cast<Value *>(Loc.Ptr), Offsets);
    if (BV && BV != Base)
      return {nullptr, nullptr, nullptr};
    const Value *InitPtr = Loc.Ptr;
    bool LastBase = Base != nullptr;
#ifndef NDEBUG
    Value *InitSize = Size;
#endif
    Value *LastSize = Size;
    while (true) {
      auto *Clobber = MSSA.getWalker()->getClobberingMemoryAccess(End, Loc);
      if (auto *Phi = dyn_cast<MemoryPhi>(Clobber)) {
        // Currently, when memoryphi is encountered, progressive processing of
        // gcread.generic is not expected, so Base can only be nullptr.
        if (Base || !TI)
          break;
        auto [AB, AV, AS] =
            processMemoryPhi(Phi, End, InitEnd, Loc, TI, Changed);
        if (AV && AV->stripPointerCasts() != Loc.Ptr->stripPointerCasts()) {
          Base = AB;
          Loc = MemoryLocation::getAfter(AV);
          Size = AS;
          if (!LastSize)
            LastSize = Size;
        }
        break;
      }
      if (!isa<MemoryDef>(Clobber) || MSSA.isLiveOnEntryDef(Clobber))
        break;

      auto *Def = cast<MemoryDef>(Clobber);
      if (!isa_and_nonnull<IntrinsicInst>(Def->getMemoryInst()))
        break;
      auto *II = cast<IntrinsicInst>(Def->getMemoryInst());

      bool Stop = false;
      switch (II->getIntrinsicID()) {
      default:
        Stop = true;
        break;
      case Intrinsic::cj_assign_generic: {
        auto *Ptr = getDest(II);
        const Value *V = Base ? Base : Loc.Ptr;
        if (Ptr->stripPointerCasts() != V->stripPointerCasts() ||
            // Obviously, define source connot be changed in between.
            hasMemoryDefBetween(MSSA, DT, DL, getSource(II), II->getNextNode(),
                                InitEnd, false, true))
          Stop = true;
        else {
          // If Offset is set, keep it.
          Loc = MemoryLocation::getAfter(getSource(II));
          Base = nullptr;
          // Unable to get ti again from Loc.Ptr, assuming ti is same, that's
          // right.
          if (TI && sameTIOpt(TI, II->getArgOperand(2))) // 2: TI
            Changed = true;
          TI = II->getArgOperand(2);
        }
      } break;
      case Intrinsic::cj_gcread_generic: {
        auto *Ptr = getDest(II);
        if (!isa<LoadInst>(getSize(II))) {
          Stop = true;
          break;
        }
        auto *SizeFrom = getUnderlyingObject(
            cast<LoadInst>(getSize(II))->getPointerOperand(), 0);
        const Value *V = Base ? Base : Loc.Ptr;
        bool IsPointerNotMatch =
            (TI && SizeFrom != TI->stripPointerCasts()) ||
            Ptr->stripPointerCasts() != V->stripPointerCasts();
        if (IsPointerNotMatch ||
            hasMemoryDefBetween(MSSA, DT, DL, getSource(II), II->getNextNode(),
                                InitEnd, false, true))
          Stop = true;
        else {
          SmallVector<std::pair<Value *, bool>, 4> TmpOffsets;
          auto *BV = getDerivedOffset(getSource(II), TmpOffsets);
          if (BV && BV != getBaseObj(II)) {
            Stop = true;
            break;
          }
          Offsets.append(TmpOffsets.begin(), TmpOffsets.end());
          // -8: This is special because we need to consider skipping typeinfo
          // when we meet two derived ptr. For example:
          //   call void @cj.gcread.generic(%1, %0, %0.off)
          //   call void @cj.gcread.generic(%2, %1, %1.off)
          //   call void @cj.copy.generic(%3, %2)
          //   call void @cj.gcread.generic(%4, %3, %3.off)
          // And we known the last offset is %off.3 - 8 + %off.1 - 8 + %off.0
          if (LastBase)
            pushAndPopSameOffset(
                Offsets,
                {{ConstantInt::get(Type::getInt64Ty(Inst->getContext()), 8),
                  false}});
          LastBase = true;
          Loc = MemoryLocation::getAfter(getSource(II));
          Base = getBaseObj(II);
          Size = getSize(II);
          // gcread.generic(%1, %0, %0.off, %size0)
          // gcread.generic(%2, %1, %1.off, %size1)
          // At last, we need to keep size1.
          if (!LastSize)
            LastSize = Size;
          // TypeInfo for gcread.generic base is different from current TI, TI
          // is sub of TI(base).
          TI = nullptr;
        }
      } break;
      case Intrinsic::cj_gcwrite_generic: {
        auto *BP = getBaseObj(II);
        auto *Derived = getDest(II);
        // This is different from copy.generic and gcread.generic where we can
        // always make sure that we write the full T. But with gcwrite.generic,
        // we don't know when to fill T. For example:
        //  call void @gcread.generic(%m0, %base, %ptr, %size0)
        //  Then we meet gcwrite.generic and its associated define.
        //    store xxx -> %base.off0
        //    gcwrite.generic(%base, %base.off1, %m1, %size1)
        // Since the size of T is unknown, it is not possible to determine when
        // T is written done. Conservatively optimization.
        bool IsPointerNotMatch =
            getSize(II) != Size || BP != Base ||
            Derived->stripPointerCasts() != Loc.Ptr->stripPointerCasts();
        if (IsPointerNotMatch ||
            hasMemoryDefBetween(MSSA, DT, DL, getSource(II), II->getNextNode(),
                                InitEnd, false, true))
          Stop = true;
        else {
          SmallVector<std::pair<Value *, bool>, 4> TmpOffsets;
          auto *BV = getDerivedOffset(getDest(II), TmpOffsets);
          if (BV && BV != BP) {
            Stop = true;
            break;
          }
          pushAndPopSameOffset(Offsets, TmpOffsets, true);
          if (LastBase)
            LastBase = false;
          Base = nullptr;
          TI = nullptr;
          Loc = MemoryLocation::getAfter(getSource(II));
        }
      } break;
      }
      if (Stop || Analyzed.count(II))
        break;
      End = Def->getDefiningAccess();
    }

    if (InitPtr == Loc.Ptr)
      return {nullptr, nullptr, nullptr};

    // Last, get equal value.
    Value *Ptr = const_cast<Value *>(Loc.Ptr);
    if (Base || !Offsets.empty()) {
      // If Inst is gcread.ref, then InitSize is nullptr.
      assert(LastSize == InitSize ||
             LastSize != nullptr && "create gcread.generic need size.");
      IRBuilder<> IRB(Inst);
      if (Offsets.empty())
        Ptr = const_cast<Value *>(Loc.Ptr);
      else {
        Base = Base ? Base : const_cast<Value *>(Loc.Ptr);
        auto *BC =
            IRB.CreateBitOrPointerCast(Base, IRB.getInt8Ty()->getPointerTo(1));
        Value *Offset = Offsets.front().first;
        assert(Offsets.front().second == true);
        for (unsigned I = 1; I < Offsets.size(); ++I)
          Offset = Offsets[I].second ? IRB.CreateAdd(Offset, Offsets[I].first)
                                     : IRB.CreateSub(Offset, Offsets[I].first);
        Ptr = IRB.CreateGEP(IRB.getInt8Ty(), BC, {Offset});
        Changed = true;
      }
    }
    // gcread.generic(...) process by phi
    // copy.generic(...)
    // If we break in process phi, then Base may not be null, and because of
    // Offsets is empty, Ptr is equal to Loc.Ptr. However, for normal case,
    // Offsets is not empty, so Ptr is not equal to Loc.Ptr.
    return {Ptr == Loc.Ptr && !Base ? nullptr : Base, Ptr, LastSize};
  }

  // 0: base pointer, 1: derived pointer, 2: typeinfo, 3: size
  static std::tuple<Value *, Value *, Value *, Value *>
  getFromPointerInfo(CallInst *CI) {
    switch (CI->getIntrinsicID()) {
    default:
      report_fatal_error("unreachable");
    case Intrinsic::cj_assign_generic:
      return {nullptr, getSource(CI), CI->getArgOperand(2), nullptr};
    case Intrinsic::cj_gcread_generic:
      return {getBaseObj(CI), getSource(CI), nullptr, getSize(CI)};
    case Intrinsic::cj_gcwrite_generic:
      return {nullptr, getSource(CI), nullptr, getSize(CI)};
    case Intrinsic::cj_gcread_ref:
      return {getBaseObj(CI), getPointerArg(CI), nullptr, nullptr};
    }
  }

  bool processGenericCopy(CallInst *CI) {
    // Base: It is valid at gcread/gcwrite.
    // Size: Also valid at gcread/gcwrite.
    // TI: TypeInfo.
    auto IID = CI->getIntrinsicID();
    auto [Base, FPtr, TI, Size] = getFromPointerInfo(CI);
    if (!FPtr)
      return false;
    MemoryLocation Loc = MemoryLocation::getAfter(FPtr);
    bool Changed = false;
    MemoryAccess *MA = IID == Intrinsic::cj_gcread_ref
                           ? cast<MemoryUseOrDef>(MSSA.getMemoryAccess(CI))
                                 ->getDefiningAccess()
                           : MSSA.getMemoryAccess(CI);
    // 0: base ptr, 1: derived ptr, 2: size
    auto [RB, RP, RS] =
        findPotentialEqualMem(CI, MA, MA, Loc, Base, TI, Size, Changed);
    if (!RP || RP == Loc.Ptr || (IID == Intrinsic::cj_gcwrite_generic && RB))
      return Changed;
    Analyzed.insert(CI);
    Function *Callee = nullptr;
    SmallVector<Value *, 4> Args;
    if (RB) {
      if (IID == Intrinsic::cj_assign_generic ||
          IID == Intrinsic::cj_gcread_generic) {
        assert(RS && "analysis error");
        Callee = Intrinsic::getDeclaration(CI->getModule(),
                                           Intrinsic::cj_gcread_generic);
        Args = {CI->getOperand(0), RB, RP, RS};
      } else if (IID == Intrinsic::cj_gcread_ref) {
        IRBuilder<> IRB(CI);
        auto *BC =
            IRB.CreateBitOrPointerCast(RP, CI->getArgOperand(1)->getType());
        CI->setArgOperand(0, RB);
        CI->setArgOperand(1, BC);
      }
    } else {
      if (IID == Intrinsic::cj_assign_generic) {
        Callee = Intrinsic::getDeclaration(CI->getModule(),
                                           Intrinsic::cj_assign_generic);
        Args = {CI->getOperand(0), RP, CI->getArgOperand(2)};
      } else if (IID == Intrinsic::cj_gcwrite_generic) {
        assert(CI->getArgOperand(3) == RS);
        Callee = Intrinsic::getDeclaration(CI->getModule(),
                                           Intrinsic::cj_gcwrite_generic);
        Args = {CI->getArgOperand(0), CI->getArgOperand(1), RP,
                CI->getArgOperand(3)};
      } else if (IID == Intrinsic::cj_gcread_generic &&
                 isa<LoadInst>(getUnderlyingObject(Size, 0))) {
        Callee = Intrinsic::getDeclaration(CI->getModule(),
                                           Intrinsic::cj_assign_generic);
        TI = getUnderlyingObject(
            cast<LoadInst>(getUnderlyingObject(Size, 0))->getPointerOperand());
        IRBuilder<> IRB(CI);
        Args = {CI->getArgOperand(0), RP,
                IRB.CreateBitOrPointerCast(TI, Callee->getArg(2)->getType())};
      }
    }
    if (Callee) {
      IRBuilder<> IRB(CI);
      auto *New = IRB.CreateCall(Callee, Args);
      New->setDebugLoc(CI->getDebugLoc());
      // Update MSSA.
      auto *MA = MSSA.getMemoryAccess(CI);
      auto *NewMA =
          MSSAU.createMemoryAccessBefore(New, MA->getDefiningAccess(), MA);
      MA->replaceAllUsesWith(NewMA);
      MSSAU.removeMemoryAccess(CI);

      CI->replaceAllUsesWith(New);
      CI->eraseFromParent();
      Changed = true;
    }
    return Changed;
  }

  bool runImpl() {
    if (!EnableCopyOpt)
      return false;
    MSSA.ensureOptimizedUses();
    bool Changed = false;
    for (auto I = inst_begin(F), E = inst_end(F); I != E;) {
      auto *CI = dyn_cast<CallInst>(&*I++);
      if (!CI || (CI->getIntrinsicID() != Intrinsic::cj_assign_generic &&
                  CI->getIntrinsicID() != Intrinsic::cj_gcread_generic &&
                  CI->getIntrinsicID() != Intrinsic::cj_gcwrite_generic &&
                  CI->getIntrinsicID() != Intrinsic::cj_gcread_ref))
        continue;
      Changed |= processGenericCopy(CI);
    }
    return Changed;
  }
};

PreservedAnalyses
CJGenericIntrinsicOpt::run(Function &F, FunctionAnalysisManager &AM) const {
  auto &AA = AM.getResult<AAManager>(F);
  auto &MSSA = AM.getResult<MemorySSAAnalysis>(F).getMSSA();
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  MemorySSAUpdater MSSAU(&MSSA);
  bool Changed = simplifyGenericInst(F, MSSAU);
  Changed |= GenericCopyOpt(F, MSSA, MSSAU, DT, AA).runImpl();
  if (Changed) {
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

namespace {
class CJGenericIntrinsicOptLegacyPass : public FunctionPass {
public:
  static char ID;

  explicit CJGenericIntrinsicOptLegacyPass() : FunctionPass(ID) {
    initializeCJGenericIntrinsicOptLegacyPassPass(
        *PassRegistry::getPassRegistry());
  }
  ~CJGenericIntrinsicOptLegacyPass() = default;

  bool runOnFunction(Function &F) override {
    auto &AA = this->getAnalysis<AAResultsWrapperPass>(F).getAAResults();
    auto &MSSA = this->getAnalysis<MemorySSAWrapperPass>(F).getMSSA();
    auto &DT = this->getAnalysis<DominatorTreeWrapperPass>(F).getDomTree();
    MemorySSAUpdater MSSAU(&MSSA);
    bool Changed = simplifyGenericInst(F, MSSAU);

    Changed |= GenericCopyOpt(F, MSSA, MSSAU, DT, AA).runImpl();
    return Changed;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MemorySSAWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<AAResultsWrapperPass>();
  }
};
} // namespace

char CJGenericIntrinsicOptLegacyPass::ID = 0;

FunctionPass *llvm::createCJGenericIntrinsicOptLegacyPass() {
  return new CJGenericIntrinsicOptLegacyPass();
}

INITIALIZE_PASS(CJGenericIntrinsicOptLegacyPass, "cj-generic-intrinsic-opt",
                "Cangjie Generic Intrinsics Optimize", false, false)
