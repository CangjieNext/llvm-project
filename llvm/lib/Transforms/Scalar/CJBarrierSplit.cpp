//===- CJBarrierSplit.cpp ---------------------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This pass optimizes the barriers, including splitting the struct barrier.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJBarrierSplit.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/CJIntrinsics.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/SafepointIRVerifier.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/InsertCJTBAA.h"
#include <cstdint>

using namespace llvm;
using namespace cangjie;

namespace {
class SplitBarrier {
public:
  struct SplitPosion {
    // the split begin position relative to read/write start.
    uint64_t SplitBeginPos;
    // the split end position relative to read/write start.
    uint64_t SplitEndPos;
    // the split size.
    uint64_t SplitSize;
    // the split reference position relative to read/write start.
    SmallVector<uint64_t, 8> SplitRefPos;

    SplitPosion(uint64_t BaseBeginPos, uint64_t BaseEndPos,
                SmallVector<uint64_t, 8> &AllRefPos) {
      SplitBeginPos = 0;
      SplitEndPos = BaseEndPos - BaseBeginPos;
      SplitSize = BaseEndPos - BaseBeginPos;
      for (auto RefPos : AllRefPos) {
        SplitRefPos.push_back(RefPos - BaseBeginPos);
      }
    }
  };
  explicit SplitBarrier(CallInst *CI)
      : M(CI->getModule()), C(CI->getContext()), IRB(CI),
        DL(CI->getModule()->getDataLayout()), Barrier(cast<IntrinsicInst>(CI)) {
    auto ID = CI->getIntrinsicID();
    if (ID == Intrinsic::cj_gcread_struct ||
        ID == Intrinsic::cj_gcwrite_struct) {
      Src = getSource(CI);
      BaseObj = getBaseObj(CI);
      Dst = getDest(CI);
      Size = cast<ConstantInt>(getSize(CI))->getZExtValue();
    } else if (ID == Intrinsic::cj_gcread_static_struct ||
               ID == Intrinsic::cj_gcwrite_static_struct) {
      Src = getSource(CI);
      Dst = getDest(CI);
      Size = cast<ConstantInt>(getSize(CI))->getZExtValue();
    } else {
      report_fatal_error("Unsupport barrier type!");
    }
    if (Size == 0) {
      report_fatal_error("The size of read barrier is 0!");
    }
    IsRead = Barrier->isCJStructGCRead();
  }

  ~SplitBarrier() = default;

  std::pair<uint64_t, Type *> getValueOffsetAndType(Value *Ptr) {
    APInt Offset(DL.getIndexSizeInBits(0), 0);
    Value *PtrBase = Ptr->stripAndAccumulateConstantOffsets(DL, Offset, true);
    return {Offset.getZExtValue(), PtrBase->getType()};
  }

  std::pair<uint64_t, uint64_t>
  findReferencePositions(Value *Ptr, SmallVector<uint64_t, 8> &AllRefPos) {
    auto [Offset, Ty] = getValueOffsetAndType(Ptr);
    uint64_t BeginPos = Offset;
    uint64_t EndPos = BeginPos + Size;
    if (isGCPointerType(Ty)) {
      report_fatal_error("The dst of read struct is gc pointer!");
      return {0, 0};
    }
    StructType *ST = dyn_cast<StructType>(Ty->getNonOpaquePointerElementType());
    if (!ST)
      return {0, 0};

    findReferencePositionInStruct(ST, BeginPos, EndPos, 0, AllRefPos);
    return {BeginPos, EndPos};
  }

  bool doSplit() {
    SmallVector<uint64_t, 8> AllRefPos;
    auto [BeginPos, EndPos] = findReferencePositions(
        IsRead ? Dst : Src, AllRefPos);
    if (EndPos == 0)
      return false;

    SplitPosion Posions(BeginPos, EndPos, AllRefPos);
    switch (AllRefPos.size()) {
    case 0:
      return false;
    case 1:
      splitForOneRef(Posions, 0);
      return true;
    case 2:
      splitForTwoRef(Posions, 0, 1);
      return true;
    case 3:
      splitForThreeRef(Posions, 0, 1, 2);
      return true;
    default:
      // Currently, only the refereces less than 3 is processed.
      return false;
    }
  }

  bool isCompleteRefInRange(uint64_t RefPos, uint64_t RefSize,
                            uint64_t BeginPos, uint64_t EndPos) {
    return RefPos >= BeginPos && RefPos <= EndPos &&
           RefSize <= EndPos - RefPos;
  }

  uint64_t getRefSize() { return DL.getPointerSize(1); }

  void splitForOneRef(SplitPosion &Pos, uint64_t RefIdx) {
    uint64_t RefSize = getRefSize();
    if (Pos.SplitSize == RefSize) {
      IsRead ? createReadRefByOffset(Pos.SplitBeginPos) :
               createWriteRefByOffset(Pos.SplitBeginPos);
      return;
    }
    // RefPos: the read/write reference position relative to base
    uint64_t RefPos = Pos.SplitRefPos[RefIdx];
    if (RefPos == Pos.SplitBeginPos) {
      // Reference is at the front of struct
      IsRead ? createReadRefByOffset(RefPos) : createWriteRefByOffset(RefPos);
      createMemoryByOffset(RefPos + RefSize, Pos.SplitEndPos);
    } else if (RefPos + RefSize == Pos.SplitEndPos) {
      // Reference is at the end of struct
      createMemoryByOffset(Pos.SplitBeginPos, RefPos);
      IsRead ? createReadRefByOffset(RefPos) : createWriteRefByOffset(RefPos);
    } else {
      // Reference is at the middle of struct
      createMemoryByOffset(Pos.SplitBeginPos, RefPos);
      IsRead ? createReadRefByOffset(RefPos) : createWriteRefByOffset(RefPos);
      createMemoryByOffset(RefPos + RefSize, Pos.SplitEndPos);
    }
  }

  void splitForTwoRef(SplitPosion &Pos, uint64_t RefIdx1, uint64_t RefIdx2) {
    // RefPos1 location must be before RefPos2
    uint64_t RefSize = getRefSize();
    uint64_t RefPos1 = Pos.SplitRefPos[RefIdx1];
    uint64_t RefPos2 = Pos.SplitRefPos[RefIdx2];
    if (RefPos1 == Pos.SplitBeginPos) {
      // Ref1 is at the front of struct
      IsRead ? createReadRefByOffset(RefPos1) : createWriteRefByOffset(RefPos1);
      Pos.SplitSize = Pos.SplitSize - RefSize;
      Pos.SplitBeginPos = RefPos1 + RefSize;
      splitForOneRef(Pos, RefIdx2);
    } else if (RefPos2 + RefSize == Pos.SplitEndPos) {
      // Ref2 is at the end of struct
      Pos.SplitSize = Pos.SplitSize - RefSize;
      Pos.SplitEndPos = RefPos2;
      splitForOneRef(Pos, RefIdx1);
      IsRead ? createReadRefByOffset(RefPos2) : createWriteRefByOffset(RefPos2);
    } else {
      // Ref1 and Ref2 is at the middle of struct
      createMemoryByOffset(Pos.SplitBeginPos, RefPos1);
      IsRead ? createReadRefByOffset(RefPos1) : createWriteRefByOffset(RefPos1);
      Pos.SplitSize = Pos.SplitSize - RefSize - (RefPos1 - Pos.SplitBeginPos);
      Pos.SplitBeginPos = RefPos1 + RefSize;
      splitForOneRef(Pos, RefIdx2);
    }
  }

  // ref1, ref2 and ref3 must be in order.
  void splitForThreeRef(SplitPosion &Pos, uint64_t RefIdx1, uint64_t RefIdx2,
                        uint64_t RefIdx3) {
    uint64_t RefSize = getRefSize();
    uint64_t RefPos1 = Pos.SplitRefPos[RefIdx1];
    uint64_t RefPos3 = Pos.SplitRefPos[RefIdx3];
    if (RefPos1 == Pos.SplitBeginPos) {
      // Ref1 is at the front of struct
      IsRead ? createReadRefByOffset(RefPos1) : createWriteRefByOffset(RefPos1);
      Pos.SplitSize = Pos.SplitSize - RefSize;
      Pos.SplitBeginPos = RefPos1 + RefSize;
      splitForTwoRef(Pos, RefIdx2, RefIdx3);
    } else if (RefPos3 + RefSize == Pos.SplitEndPos) {
      // Ref3 is at the end of struct
      Pos.SplitSize = Pos.SplitSize - RefSize;
      Pos.SplitEndPos = RefPos3;
      splitForTwoRef(Pos, RefIdx1, RefIdx2);
      IsRead ? createReadRefByOffset(RefPos3) : createWriteRefByOffset(RefPos3);
    } else {
      // Ref1, Ref2 and Ref3 is at the middle of struct
      createMemoryByOffset(Pos.SplitBeginPos, RefPos1);
      IsRead ? createReadRefByOffset(RefPos1) : createWriteRefByOffset(RefPos1);
      Pos.SplitSize = Pos.SplitSize - RefSize - (RefPos1 - Pos.SplitBeginPos);
      Pos.SplitBeginPos = RefPos1 + RefSize;
      splitForTwoRef(Pos, RefIdx2, RefIdx3);
    }
  }

  void findReferencePositionInStruct(StructType *ST, uint64_t BeginPos,
                                     uint64_t EndPos, uint64_t CurPos,
                                     SmallVector<uint64_t, 8> &AllRefPos) {
    for (uint64_t i = 0; i < ST->getNumElements(); i++) {
      uint64_t Offset = DL.getStructLayout(ST)->getElementOffset(i) + CurPos;

      // 1. ST = {i64, {i64, {i64, i8*, i64}}, i64}, Begin = 16, End = 40
      // 2. ST = {i64, [5 * EST]}
      //    EST = {i64, i8*, i64}
      //    Begin = 32, End = 64
      if (i != ST->getNumElements() - 1) {
        uint64_t NextOffset = DL.getStructLayout(ST)->getElementOffset(i + 1);
        if (Offset < BeginPos && NextOffset < BeginPos)
          continue;
      }
      if (Offset >= EndPos)
        return;
      Type *EleTy = ST->getElementType(i);
      if (isGCPointerType(EleTy)) {
        uint64_t RefSize = DL.getTypeStoreSize(EleTy);
        if (isCompleteRefInRange(Offset, RefSize, BeginPos, EndPos))
          AllRefPos.push_back(Offset);
      } else if (auto *EST = dyn_cast<StructType>(EleTy)) {
        findReferencePositionInStruct(EST, BeginPos, EndPos, Offset, AllRefPos);
      } else if (auto *EAT = dyn_cast<ArrayType>(EleTy)) {
        findReferencePositionInArray(EAT, BeginPos, EndPos, Offset, AllRefPos);
      }
    }
  }

  void findReferencePositionInArray(ArrayType *AT, uint64_t BeginPos,
                                    uint64_t EndPos, uint64_t CurPos,
                                    SmallVector<uint64_t, 8> &AllRefPos) {
    uint64_t EleNum = AT->getNumElements();
    Type *EleType = AT->getElementType();
    if (isa<PointerType>(EleType) && isGCPointerType(EleType)) {
      uint64_t EleSize = DL.getTypeAllocSize(EleType);
      for (uint64_t Idx = 0; Idx < EleNum; Idx++) {
        uint64_t RefPos = CurPos + EleSize * Idx;
        if (isCompleteRefInRange(RefPos, EleSize, BeginPos, EndPos))
          AllRefPos.push_back(RefPos);
      }
    } else if (auto *ArrayStruct = dyn_cast<StructType>(EleType)) {
      uint64_t EleSize = DL.getStructLayout(ArrayStruct)->getSizeInBytes();
      for (uint64_t Idx = 0; Idx < EleNum; Idx++) {
        findReferencePositionInStruct(ArrayStruct, BeginPos, EndPos,
                                      CurPos + Idx * EleSize, AllRefPos);
      }
    }
  }

  void createMemoryByOffset(uint64_t BeginPos, uint64_t EndPos) {
    if (EndPos <= BeginPos)
      report_fatal_error("The offset of read barrier splitting is error!");

    uint64_t MemcpySize = EndPos - BeginPos;
    Value *DstPtr = Dst;
    Value *SrcPtr = Src;
    if (BeginPos > 0) {
      SmallVector<Value *> Idxs;
      Idxs.push_back(ConstantInt::get(IRB.getInt32Ty(), BeginPos));
      DstPtr = IRB.CreateGEP(IRB.getInt8Ty(), Dst, {Idxs}, "", true);
      SrcPtr = IRB.CreateGEP(IRB.getInt8Ty(), Src, {Idxs}, "", true);
    }
    CallInst *CI =
        IRB.CreateMemCpy(DstPtr, Align(8), SrcPtr, Align(8), MemcpySize);
    prepareCJTBAA(CI->getModule()->getDataLayout(), CI, CI->getArgOperand(0),
                  nullptr, true);
    CI->setDebugLoc(Barrier->getDebugLoc());
  }

  std::pair<Value*, Value*> maybeCreateGEPByOffset(uint64_t Off) {
    Value *DstPtr = Dst;
    Value *SrcPtr = Src;
    if (Off > 0) {
      SmallVector<Value *> Idxs;
      Idxs.push_back(ConstantInt::get(IRB.getInt32Ty(), Off));
      DstPtr = IRB.CreateGEP(IRB.getInt8Ty(), Dst, {Idxs}, "", true);
      SrcPtr = IRB.CreateGEP(IRB.getInt8Ty(), Src, {Idxs}, "", true);
    }
    return {DstPtr, SrcPtr};
  }

  bool isPtrEnumOrOption(Value *Ptr) {
    if (AllocaInst *AI = dyn_cast<AllocaInst>(Ptr)) {
      Type *AllocatedType = AI->getAllocatedType();
 
      if (StructType *ST = dyn_cast<StructType>(AllocatedType)) {
        StringRef Name = ST->getName();
        return Name.contains("Option") || Name.contains("enum");
      }
    }
 
    return false;
  }

  void createReadRefByOffset(uint64_t RefPos) {
    auto [DstPtr, SrcPtr] = maybeCreateGEPByOffset(RefPos);
    Type *Int8AS1Ty = IRB.getInt8PtrTy(1);
    CallInst *ReadRef = nullptr;
    Value *SrcCast = nullptr;
    if (BaseObj) {
      SrcCast = IRB.CreateBitCast(SrcPtr, Int8AS1Ty->getPointerTo(1));
      Function *Callee = Intrinsic::getDeclaration(M, Intrinsic::cj_gcread_ref);
      ReadRef = IRB.CreateCall(Callee, {BaseObj, SrcCast});
    } else {
      SrcCast = IRB.CreateBitCast(SrcPtr, Int8AS1Ty->getPointerTo(0));
      Function *Callee =
          Intrinsic::getDeclaration(M, Intrinsic::cj_gcread_static_ref);
      ReadRef = IRB.CreateCall(Callee, {SrcCast});
    }

    if (isPtrEnumOrOption(getUnderlyingObject(DstPtr))) {
      ReadRef->setMetadata(LLVMContext::MD_untrusted_ref,
                           MDNode::get(ReadRef->getContext(), {}));
    }
    updateTBAA(DL, ReadRef);

    ReadRef->setDebugLoc(Barrier->getDebugLoc());
    Value *DstCast = IRB.CreateBitCast(DstPtr, Int8AS1Ty->getPointerTo(0));
    StoreInst *SI = IRB.CreateStore(ReadRef, DstCast);
    SI->setDebugLoc(Barrier->getDebugLoc());
    // Since we know the type of Dst, this doesn't fail.
    prepareCJTBAA(DL, SI, DstCast, Int8AS1Ty);
  }

  void createWriteRefByOffset(uint64_t RefPos) {
    auto [DstPtr, SrcPtr] = maybeCreateGEPByOffset(RefPos);
    // Create load
    Type *Int8AS1Ty = IRB.getInt8PtrTy(1);
    Value *SrcCast = IRB.CreateBitCast(SrcPtr, Int8AS1Ty->getPointerTo());
    LoadInst *LI = LI = IRB.CreateLoad(Int8AS1Ty, SrcCast);
    LI->setDebugLoc(Barrier->getDebugLoc());
    // Since we know the type of Src, this doesn't fail.
    prepareCJTBAA(DL, LI, SrcCast, Int8AS1Ty);

    // Create gcwrite.ref
    Value *DstCast = nullptr;
    CallInst *WriteRef = nullptr;
    if (BaseObj) {
      DstCast = IRB.CreateBitCast(DstPtr, Int8AS1Ty->getPointerTo(1));
      Function *Callee =
          Intrinsic::getDeclaration(M, Intrinsic::cj_gcwrite_ref);
      WriteRef = IRB.CreateCall(Callee, {LI, BaseObj, DstCast});
    } else {
      DstCast = IRB.CreateBitCast(DstPtr, Int8AS1Ty->getPointerTo());
      Function *Callee =
          Intrinsic::getDeclaration(M, Intrinsic::cj_gcwrite_static_ref);
      WriteRef = IRB.CreateCall(Callee, {LI, DstCast});
    }
    WriteRef->setDebugLoc(Barrier->getDebugLoc());
    updateTBAA(DL, WriteRef);
  }

private:
  Module *M;
  LLVMContext &C;
  IRBuilder<> IRB;
  const DataLayout &DL;
  IntrinsicInst *Barrier = nullptr;
  bool IsRead = false;
  Value *Src = nullptr;
  Value *Dst = nullptr;
  Value *BaseObj = nullptr;
  uint64_t Size = 0;
};

} // namespace

static bool needToSplit(Instruction *I) {
  auto *II = dyn_cast<IntrinsicInst>(I);
  if (!II)
    return false;
  Intrinsic::ID ID = II->getIntrinsicID();
  if (ID == Intrinsic::cj_gcread_struct ||
      ID == Intrinsic::cj_gcread_static_struct ||
      ID == Intrinsic::cj_gcwrite_struct ||
      ID == Intrinsic::cj_gcwrite_static_struct) {
    if (isa<ConstantInt>(getSize(II))) {
      return true;
    }
  }
  return false;
}

static bool simplySplit(Function *F) {
  bool Changed = false;
  SetVector<CallInst *> ToSplits;
  for (auto *I : F->users()) {
    auto *CI = dyn_cast<CallInst>(I);
    if (!CI)
      continue;
    if (needToSplit(CI))
      ToSplits.insert(CI);
  }
  for (auto *I : ToSplits) {
    SplitBarrier SB(I);
    if (SB.doSplit()) {
      I->eraseFromParent();
      Changed = true;
    }
  }

  return Changed;
}

PreservedAnalyses CJBarrierSplit::run(Module &M,
                                      ModuleAnalysisManager &) const {
  auto C = &(M.getContext());
  int LongSize = M.getDataLayout().getPointerSizeInBits();
  auto IntptrTy = Type::getIntNTy(*C, LongSize);

  SmallVector<Function *> AggBarrierIntrinsics = {
      Intrinsic::getDeclaration(&M, Intrinsic::cj_gcread_struct, {IntptrTy}),
      Intrinsic::getDeclaration(&M, Intrinsic::cj_gcwrite_struct,
                                {Type::getInt8PtrTy(M.getContext()), IntptrTy}),
      Intrinsic::getDeclaration(&M, Intrinsic::cj_gcread_static_struct,
                                {IntptrTy}),
      Intrinsic::getDeclaration(&M, Intrinsic::cj_gcwrite_static_struct,
                                {IntptrTy})};
  bool Changed = false;
  for (auto *F : AggBarrierIntrinsics)
    Changed |= simplySplit(F);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

namespace {
class CJBarrierSplitLegacyPass : public ModulePass {
public:
  static char ID;

  explicit CJBarrierSplitLegacyPass() : ModulePass(ID) {
    initializeCJBarrierSplitLegacyPassPass(*PassRegistry::getPassRegistry());
  }
  ~CJBarrierSplitLegacyPass() = default;

  bool runOnModule(Module &M) override {
    SmallVector<Function *> AggBarrierIntrinsics = {
        Intrinsic::getDeclaration(&M, Intrinsic::cj_gcread_struct),
        Intrinsic::getDeclaration(&M, Intrinsic::cj_gcwrite_struct,
                                  {Type::getInt8PtrTy(M.getContext())}),
        Intrinsic::getDeclaration(&M, Intrinsic::cj_gcread_static_struct),
        Intrinsic::getDeclaration(&M, Intrinsic::cj_gcwrite_static_struct)};
    bool Changed = false;
    for (auto *F : AggBarrierIntrinsics)
      Changed |= simplySplit(F);
    return Changed;
  }
};
} // namespace

char CJBarrierSplitLegacyPass::ID = 0;

ModulePass *llvm::createCJBarrierSplitLegacyPass() {
  return new CJBarrierSplitLegacyPass();
}

INITIALIZE_PASS(CJBarrierSplitLegacyPass, "cj-barrier-split",
                "Cangjie Barriers Split", false, false)
