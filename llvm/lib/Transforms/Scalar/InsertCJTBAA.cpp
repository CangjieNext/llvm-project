//===- InsertCJTBAA.cpp - Insert Cangjie TBAA Metadata ----------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Insert Cangjie TBAA Metadata for load, store, memcpy, and barrier
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/InsertCJTBAA.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CJIntrinsics.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/SafepointIRVerifier.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/CJFillMetadata.h"
#include <cstdint>

using namespace llvm;
using namespace cangjie;

namespace llvm {
extern cl::opt<bool> CJPipeline;
} // namespace llvm

static bool isUnnamedArrayLayout(StructType *ST);

class TBAAProcessUnit {
public:
  TBAAProcessUnit(const DataLayout &DL, MDBuilder &MDB, LLVMContext &Context,
                  Instruction *I, StructType *SrcTy, Type *DstTy,
                  GEPOperator *GEP, bool IsPrimitiveMetadata,
                  uint64_t OriginOff)
      : DL(DL), MDB(MDB), Context(Context), I(I), SrcTy(SrcTy), DstTy(DstTy),
        GEP(GEP), IsPrimitiveMetadata(IsPrimitiveMetadata),
        OriginOff(OriginOff){};
  ~TBAAProcessUnit() = default;

  const DataLayout &DL;
  MDBuilder &MDB;
  LLVMContext &Context;
  Instruction *I;
  StructType *SrcTy;
  Type *DstTy;
  GEPOperator *GEP;
  bool IsPrimitiveMetadata;
  uint64_t OriginOff;
};

static bool getMallocType(TBAAProcessUnit &TBAAUnit, CallBase *CB) {
  auto ID = CB->getIntrinsicID();
  Value *Klass;
  if (ID == Intrinsic::cj_malloc_object) {
    Klass = CB->getArgOperand(0)->stripPointerCasts();
  } else if (ID == Intrinsic::cj_malloc_array) {
    Klass = CB->getArgOperand(1)->stripPointerCasts();
  } else {
    return false;
  }
  auto GV = dyn_cast<GlobalVariable>(Klass);
  if (GV == nullptr || !GV->hasInitializer() ||
      GV->getMetadata("RelatedType") == nullptr) {
    return false;
  }
  TypeInfo TI(GV);
  TBAAUnit.SrcTy = TI.getLayoutType();
  return TBAAUnit.SrcTy == nullptr;
}

static std::string getArrayName(ArrayType *AT) {
  std::string Res;
  Type *ET = AT->getArrayElementType();
  unsigned ElemNum = AT->getArrayNumElements();
  Res = Twine(ElemNum).str() + "*";
  if (auto EAT = dyn_cast<ArrayType>(ET)) {
    return Res + getArrayName(EAT);
  } else {
    return Res + getPrimitiveTypeName(ET);
  }
}

static std::string getTypeString(Type *Ty) {
  if (auto IT = dyn_cast<IntegerType>(Ty)) {
    switch (IT->getBitWidth()) {
    default:
      return "";
    case 1:
      return "char";
    case 8:
      return "i8";
    case 16:
      return "i16";
    case 32:
      return "i32";
    case 64:
      return "i64";
    }
  } else if (Ty->isFloatTy()) {
    return "f32";
  } else if (Ty->isBFloatTy()) {
    return "bfloat";
  } else if (Ty->isHalfTy()) {
    return "half";
  } else if (Ty->isDoubleTy()) {
    return "f64";
  } else if (Ty->isPointerTy()) {
    return "pointer";
  } else if (auto AT = dyn_cast<ArrayType>(Ty)) {
    return getArrayName(AT);
  } else if (auto ST = dyn_cast<StructType>(Ty)) {
    if (ST->hasName()) {
      return ST->getStructName().str();
    }
    return "";
  }
  return "";
}

static Type *getArrayInnerType(ArrayType *AT) {
  Type *ET = AT->getArrayElementType();
  if (auto AET = dyn_cast<ArrayType>(ET)) {
    return getArrayInnerType(AET);
  }
  return ET;
}

static MDNode *getTBAARoot(MDBuilder &MDB, StringRef Name) {
  MDNode *MDRoot = MDB.createTBAARoot("Simple Cangjie TBAA");
  MDNode *MDPointer = MDB.createTBAANode("omnipotent char", MDRoot);
  return MDB.createTBAANode(Name, MDPointer);
}

// Tag: false, expand the array element.
static MDNode *getTBAAMetadata(const DataLayout &DL, MDBuilder &MDB, Type *Ty,
                               bool Tag = true) {
  std::string Name = "";
  Name = getTypeString(Ty);
  if (Name.empty()) {
    return nullptr;
  }
  if (auto ST = dyn_cast<StructType>(Ty)) {
    // MDNode: element type
    // uint64_t: offset of element type
    SmallVector<std::pair<MDNode *, uint64_t>> Fields;
    const StructLayout *SL = DL.getStructLayout(ST);
    for (unsigned It = 0; It < ST->getStructNumElements(); ++It) {
      Type *ET = ST->getStructElementType(It);
      MDNode *Filed = getTBAAMetadata(DL, MDB, ET, Tag);
      if (Filed == nullptr) {
        return nullptr;
      }
      Fields.push_back(
          std::pair<MDNode *, uint64_t>(Filed, SL->getElementOffset(It)));
    }
    return MDB.createTBAAStructTypeNode(Name, Fields);
  }
  if (auto AT = dyn_cast<ArrayType>(Ty)) {
    Type *ET = getArrayInnerType(AT);
    std::string ETName = getTypeString(ET);
    if (ETName.empty()) {
      return nullptr;
    }
    MDNode *ETMD;
    if (Tag) {
      ETMD = getTBAARoot(MDB, ETName);
    } else {
      ETMD = getTBAAMetadata(DL, MDB, ET, Tag);
    }
    if (ETMD == nullptr) {
      return nullptr;
    }
    return MDB.createTBAANode(Name, ETMD);
  }
  return getTBAARoot(MDB, Name);
}

// SrcTy does not contain array.
bool llvm::insertTBAAWithDefiniteType(const DataLayout &DL, Instruction *I,
                                      StructType *SrcTy, Type *DstTy,
                                      uint64_t Offset, bool Tag) {
  MDBuilder MDB(I->getContext());
  MDNode *SrcMD = getTBAAMetadata(DL, MDB, SrcTy, Tag);
  MDNode *DstMD = getTBAAMetadata(DL, MDB, DstTy);
  if (DstMD != nullptr && SrcMD != nullptr) {
    MDNode *MD = MDB.createTBAAStructTagNode(SrcMD, DstMD, Offset);
    I->setMetadata(LLVMContext::MD_tbaa, MD);
    return true;
  }
  return false;
}

static MDNode *getTBAAPrimitiveMetadata(TBAAProcessUnit &TBAAUnit, Type *Ty) {
  assert(!Ty->isStructTy() && !Ty->isArrayTy());
  if (MDNode *MD = getTBAAMetadata(TBAAUnit.DL, TBAAUnit.MDB, Ty)) {
    return TBAAUnit.MDB.createTBAAStructTagNode(MD, MD, 0);
  }
  return nullptr;
}

static bool
getTBAAStructMetadata(TBAAProcessUnit &TBAAUnit, StructType *ST,
                      unsigned Offset,
                      SmallVector<MDBuilder::TBAAStructField> &Fields) {
  unsigned ElementNum = ST->getStructNumElements();
  const StructLayout *SL = TBAAUnit.DL.getStructLayout(ST);
  for (unsigned It = 0; It < ElementNum; ++It) {
    Type *ET = ST->getElementType(It);
    if (!ET->isStructTy() && !ET->isArrayTy()) {
      if (MDNode *MD = getTBAAPrimitiveMetadata(TBAAUnit, ET)) {
        unsigned ETOffset = SL->getElementOffset(It) + Offset;
        unsigned ETSize = TBAAUnit.DL.getTypeSizeInBits(ET) / 8;
        Fields.push_back(MDBuilder::TBAAStructField(ETOffset, ETSize, MD));
        continue;
      } else {
        return false;
      }
    }
    if (auto AET = dyn_cast<ArrayType>(ET)) {
      unsigned ETOffset = SL->getElementOffset(It) + Offset;
      unsigned ETSize = TBAAUnit.DL.getTypeSizeInBits(ET) / 8;
      MDNode *MDRoot = TBAAUnit.MDB.createTBAARoot("Simple Cangjie TBAA");
      MDNode *MDPointer =
          TBAAUnit.MDB.createTBAANode("omnipotent char", MDRoot);
      MDNode *MD =
          TBAAUnit.MDB.createTBAAStructTagNode(MDPointer, MDPointer, 0);
      Fields.push_back(MDBuilder::TBAAStructField(ETOffset, ETSize, MD));
      continue;
    }
    if (auto SET = dyn_cast<StructType>(ET)) {
      unsigned ETOffset = SL->getElementOffset(It) + Offset;
      if (!getTBAAStructMetadata(TBAAUnit, SET, ETOffset, Fields)) {
        return false;
      }
    }
  }
  return true;
}

// %struct1 = type { i8 addrspace(1)*, i64, i64 }
// %struct2 = type { %struct1, i64 }
// %struct3 = type { i64, %struct2 }
// %0 = bitcast %struct3* %value1 to i8*
// %1 = bitcast %struct3* %value2 to i8*
// call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %0, i8* align 8 %1, i64
//     40, i1 false), !tbaa.struct !0
// ......
// !0 = !{i64 0, i64 8, !1, i64 8, i64 8, !3, i64 16, i64 8,
//     !1, i64 24, i64 8, !1, i64 32, i64 8, !1}
// !1 = !{!2, !2, i64 0}
// !2 = !{!"i64", !5}
// !3 = !{!4, !4, i64 0}
// !4 = !{!"pointer", !5}
// !5 = !{!"omnipotent char", !6}
// !6 = !{!"Simple Cangjie TBAA"}
static bool insertCJTBAAStructImpl(TBAAProcessUnit &TBAAUnit, StructType *ST) {
  if (ST == nullptr) {
    return false;
  }
  SmallVector<MDBuilder::TBAAStructField> Fields;
  if (!getTBAAStructMetadata(TBAAUnit, ST, 0, Fields)) {
    return false;
  }
  MDNode *MD = TBAAUnit.MDB.createTBAAStructNode(Fields);
  TBAAUnit.I->setMetadata(LLVMContext::MD_tbaa_struct, MD);
  return true;
}

Type *llvm::getInnerTypeByOffset(const DataLayout &DL, StructType *ST,
                                 uint64_t Offset) {
  assert(ST != nullptr && "ST should be StructType");
  auto *SL = DL.getStructLayout(ST);
  unsigned CurIdx = SL->getElementContainingOffset(Offset);
  for (unsigned I = CurIdx; I < ST->getNumElements(); ++I) {
    Type *Ty = ST->getElementType(I);
    auto ElemOffset = SL->getElementOffset(I);
    if (auto *ElemST = dyn_cast<StructType>(Ty)) {
      // For case: %type { %Unit.Type, %Unit.Type, %Tuple<i64> }
      if (ElemST->getNumElements() == 0)
        continue;

      // For case: %type { i64, float, %Unit.Type, %Tuple<i64> }
      if (ElemOffset > Offset) {
        return nullptr;
      }
      return getInnerTypeByOffset(DL, ElemST, Offset - ElemOffset);
    } else {
      // %type = type { i8*, i32, i64, i64 }
      // The offset is 12, written to the hole between I and I+1.
      return Offset == ElemOffset ? Ty : nullptr;
    }
  }
  // %type { i32, float, i32, %Unit.Type }
  // The offset is 12, written to the %Unit.Type.
  return nullptr;
}

// There is such a situation:
// %struct = type { i8, float }
// %0 = bitcast %struct* to i64*
// %1 = load i64, i64* %0
static bool isSameType(TBAAProcessUnit &TBAAUnit, uint64_t Offset = 0) {
  if (TBAAUnit.SrcTy == nullptr) {
    return false;
  }
  Type *SrcTy = getInnerTypeByOffset(TBAAUnit.DL, TBAAUnit.SrcTy, Offset);
  if (SrcTy == nullptr) {
    return false;
  }
  return SrcTy == TBAAUnit.DstTy;
}

// %struct1 = type { i8 addrspace(1)*, i64, i64 }
// %struct2 = type { %struct1, i64 }
// %struct3 = type { i64, %struct2 }
// %0 = getelementptr inbounds %struct3, %struct3* %value, i32 0, i32 0
// store i64 1, i64* %0, !tbaa !2480
// ......
// !0 = !{!1, !4, i64 0}
// !1 = !{!"struct3", !4, i64 0, !2, i64 8}
// !2 = !{!"struct2", !3, i64 0, !4, i64 24}
// !3 = !{!"struct1", !5, i64 0, !4, i64 8, !4, i64 16}
// !4 = !{!"i64", !6}
// !5 = !{!"pointer", !6}
// !6 = !{!"omnipotent char", !7}
// !7 = !{!"Simple Cangjie TBAA"}
static bool insertCJTBAAMeta(TBAAProcessUnit &TBAAUnit, APInt Offset,
                             bool IsSameTy = false) {
  // Offset = -1 means dst type is different of src type
  if (TBAAUnit.IsPrimitiveMetadata && Offset != -1) {
    if (MDNode *MD = getTBAAPrimitiveMetadata(TBAAUnit, TBAAUnit.DstTy)) {
      TBAAUnit.I->setMetadata(LLVMContext::MD_tbaa, MD);
      return true;
    }
    return false;
  }
  if (TBAAUnit.SrcTy == nullptr) {
    return false;
  }
  if (auto DstST = dyn_cast<StructType>(TBAAUnit.DstTy)) {
    return insertCJTBAAStructImpl(TBAAUnit, DstST);
  }
  if (TBAAUnit.DstTy->isArrayTy()) {
    return false;
  }
  if (!IsSameTy && !isSameType(TBAAUnit, Offset.getZExtValue())) {
    return false;
  }
  return insertTBAAWithDefiniteType(TBAAUnit.DL, TBAAUnit.I, TBAAUnit.SrcTy,
                                    TBAAUnit.DstTy,
                                    Offset.getZExtValue() + TBAAUnit.OriginOff);
}

static bool insertArrayLayoutMeta(TBAAProcessUnit &TBAAUnit, APInt &Offset,
                                  APInt &SubOffset) {
  if (SubOffset.getSExtValue() < 0) {
    TBAAUnit.IsPrimitiveMetadata = false;
    return insertCJTBAAMeta(TBAAUnit, Offset, true);
  }
  return insertTBAAWithDefiniteType(
      TBAAUnit.DL, TBAAUnit.I, TBAAUnit.SrcTy, TBAAUnit.DstTy,
      Offset.getZExtValue() + SubOffset.getSExtValue() + TBAAUnit.OriginOff,
      false);
}

static bool IsArrayLayoutElement(TBAAProcessUnit &TBAAUnit,
                                 const StructLayout *SL, GEPOperator *GEP) {
  assert(((TBAAUnit.SrcTy->hasName() &&
           TBAAUnit.SrcTy->getStructName().startswith("ArrayLayout.")) ||
          (isUnnamedArrayLayout(TBAAUnit.SrcTy))) &&
         "Src type should be array type");
  if (TBAAUnit.DstTy == nullptr) {
    return false;
  }
  // 3: ArrayLayout.xxx = type { %ArrayBase, [0 * xxx] },
  // indices of gep is 0, 1 and x
  if (GEP->getNumIndices() < 3) {
    return false;
  }
  // getelementptr %ArrayLayout.xxx, %ArrayLayout.xxx* %a, i32 0, i32 1, i64 %b
  // Such gep is to get the array element xxx in ArrayLayout.xxx
  // Index0: first indice of gep
  auto Index0 = dyn_cast<ConstantInt>(GEP->getOperand(1));
  // Index1: second indice of gep
  auto Index1 = dyn_cast<ConstantInt>(GEP->getOperand(2));
  if (Index0 == nullptr || Index1 == nullptr) {
    return false;
  }
  // 4: When the num of indices is more than 4, we do nothing now.
  if (Index0->getZExtValue() != 0 || Index1->getZExtValue() != 1 ||
      GEP->getNumIndices() > 4) {
    return false;
  }
  APInt Offset(TBAAUnit.DL.getIndexSizeInBits(0u), SL->getElementOffset(1));
  APInt SubOffset(TBAAUnit.DL.getIndexSizeInBits(0u), -1);
  // 3: ArrayLayout.xxx = type { %ArrayBase, [0 * xxx] },
  // indices of gep is 0, 1 and x
  if (GEP->getNumIndices() == 3) {
    Type *Ty = TBAAUnit.SrcTy->getStructElementType(1)->getArrayElementType();
    if (Ty != TBAAUnit.DstTy && !isa<StructType>(Ty))
      return false;
    if (!isa<StructType>(Ty))
      return insertArrayLayoutMeta(TBAAUnit, Offset, SubOffset);

    // The indices of the GEP is 3, and the internal array of Arraylayout is a
    // record array, so we are accessing the first element of the xth record of
    // the record array.
    SubOffset = 0;
    return insertArrayLayoutMeta(TBAAUnit, Offset, SubOffset);
  }

  // Index3: fourth indice of gep
  auto Index3 = dyn_cast<ConstantInt>(GEP->getOperand(4));
  StructType *ArrayET = dyn_cast<StructType>(
      TBAAUnit.SrcTy->getStructElementType(1)->getArrayElementType());
  if (Index3 == nullptr || ArrayET == nullptr) {
    return false;
  }
  SubOffset = APInt(TBAAUnit.DL.getIndexSizeInBits(0u),
                    TBAAUnit.DL.getStructLayout(ArrayET)->getElementOffset(
                        Index3->getZExtValue()));
  return insertArrayLayoutMeta(TBAAUnit, Offset, SubOffset);
}

static bool getSrcTyAndOffset(TBAAProcessUnit &TBAAUnit, Value *OP,
                              APInt &Offset, bool &CanAccuOffset) {
  if (auto GEP = dyn_cast<GEPOperator>(OP)) {
    if (GEP->getSourceElementType()->isStructTy()) {
      TBAAUnit.SrcTy = dyn_cast<StructType>(GEP->getSourceElementType());
      TBAAUnit.GEP = GEP;

      // SrcTy = type {i32, i32, [0 x i32], ...}
      // indices of gep is 0, 2, and a spefic constant int
      // If we find a type in GEP is ArrayType, we choose to align offset to the
      // offset of the first element of Array, so as to avoid the tbaa verifier
      // error.
      Type *Ty = TBAAUnit.SrcTy;
      for (unsigned I = 1; I < GEP->getNumIndices() - 1; ++I) {
        auto *Idx = dyn_cast<ConstantInt>(GEP->getOperand(I + 1));
        if (auto *STy = dyn_cast<StructType>(Ty))
          Ty = STy->getStructElementType(Idx->getZExtValue());
        else if (auto *ATy = dyn_cast<ArrayType>(Ty)) {
          CanAccuOffset = false;
          break;
        }
      }
      if (isa<ArrayType>(Ty))
        CanAccuOffset = false;
    } else if (auto GEPCB = dyn_cast<CallBase>(
                   GEP->getPointerOperand()->stripPointerCasts())) {
      return getSrcTyAndOffset(TBAAUnit, GEPCB, Offset, CanAccuOffset);
    } else if (auto *GEPAI = dyn_cast<AllocaInst>(
                   GEP->getPointerOperand()->stripPointerCasts())) {
      TBAAUnit.GEP = GEP;
      return getSrcTyAndOffset(TBAAUnit, GEPAI, Offset, CanAccuOffset);
    }
    if (!GEP->accumulateConstantOffset(TBAAUnit.DL, Offset)) {
      CanAccuOffset = false;
    }
    return true;
  } else if (auto CB = dyn_cast<CallBase>(OP)) {
    return getMallocType(TBAAUnit, CB);
  } else if (auto AI = dyn_cast<AllocaInst>(OP)) {
    Type *Ty = AI->getAllocatedType();
    if (Ty->isStructTy()) {
      CanAccuOffset = !TBAAUnit.GEP || TBAAUnit.GEP->accumulateConstantOffset(
                                           TBAAUnit.DL, Offset);
      if (CanAccuOffset)
        TBAAUnit.SrcTy = dyn_cast<StructType>(Ty);
    } else if (!Ty->isStructTy() && AI->getAllocatedType() == TBAAUnit.DstTy) {
      // We check the alloca type and dst type, because sroa may modify alloca
      // and then generate a series of memory insts based on new alloca, which
      // will cause the instruction we are currently analyzing and the newly
      // generated instruction by sroa to generate the wrong alias information
      // (NoAlias).
      TBAAUnit.IsPrimitiveMetadata = true;
      if (TBAAUnit.GEP &&
          (!TBAAUnit.GEP->accumulateConstantOffset(TBAAUnit.DL, Offset) ||
           Offset != 0 || Ty != TBAAUnit.GEP->getSourceElementType())) {
        CanAccuOffset = false;
        Offset = -1;
      }
    }
    return true;
  } else if (isa<GlobalVariable>(OP) || isa<Argument>(OP)) {
    assert(OP->getType()->isPointerTy());
    Type *ET = OP->getType()->getNonOpaquePointerElementType();
    if (auto SET = dyn_cast<StructType>(ET)) {
      TBAAUnit.SrcTy = SET;
      return true;
    }
    if (TBAAUnit.DstTy != nullptr && !TBAAUnit.DstTy->isPointerTy()) {
      TBAAUnit.IsPrimitiveMetadata = true;
      return true;
    }
  }
  return false;
}

static bool isUnnamedArrayLayout(StructType *ST) {
  // 2: ST is { %ArrayBase, [n x %xxx] }
  if (ST->getStructNumElements() != 2) {
    return false;
  }
  auto ET0 = dyn_cast<StructType>(ST->getStructElementType(0));
  auto ET1 = dyn_cast<ArrayType>(ST->getStructElementType(1));
  return ET0 != nullptr && ET1 != nullptr &&
         ET0->getStructName().equals("ArrayBase");
}

static bool preArrayLayout(TBAAProcessUnit &TBAAUnit) {
  if (TBAAUnit.GEP == nullptr || TBAAUnit.SrcTy == nullptr) {
    return false;
  }
  if (TBAAUnit.SrcTy->hasName()) {
    return TBAAUnit.SrcTy->getStructName().startswith("ArrayLayout.");
  }
  if (isUnnamedArrayLayout(TBAAUnit.SrcTy)) {
    return true;
  }
  return false;
}

static bool tryToInsertTBAAForGEPGEP(TBAAProcessUnit &TBAAUnit) {
  auto *GEP = TBAAUnit.GEP;
  if (!GEP)
    return false;
  auto *GEPGEP = dyn_cast<GEPOperator>(GEP->getPointerOperand());
  if (!GEPGEP || GEP->getNumIndices() != 1 ||
      isa<StructType>(GEP->getSourceElementType()) ||
      !isa<ConstantInt>(GEP->getOperand(1)))
    return false;
  if (!GEPGEP->getSourceElementType()->isStructTy())
    return false;

  auto CheckGEPGEP = [&GEPGEP] {
    Type *Ty = GEPGEP->getSourceElementType();
    for (unsigned I = 1; I < GEPGEP->getNumIndices() - 1; ++I) {
      auto *Idx = dyn_cast<ConstantInt>(GEPGEP->getOperand(I + 1));
      if (!Idx) {
        if (Ty->isStructTy())
          return false;
        else if (Ty->isArrayTy())
          Ty = cast<ArrayType>(Ty)->getElementType();
        else
          return false;
      } else {
        if (!Ty->isStructTy())
          return false;
        Ty = cast<StructType>(Ty)->getElementType(Idx->getZExtValue());
      }
    }
    return Ty->isStructTy();
  };
  // %1 = gep %0, 0, 1, %index, 1
  // %2 = gep %1, 1
  // The type of the penultimate pointer of the GEP must be struct. Otherwise,
  // the verifier may be incorrect after the InstCombine optimizes the GEP.
  if (!CheckGEPGEP())
    return false;

  auto *OuterIdx = cast<ConstantInt>(GEP->getOperand(1));
  if (OuterIdx->isZero())
    return prepareCJTBAA(TBAAUnit.DL, TBAAUnit.I, GEPGEP, TBAAUnit.DstTy,
                         false, 0);

  // If a scalar-typed pointer is advanced again, the final access may land in
  // the middle of a scalar or in struct padding. We cannot reconstruct a
  // verifier-safe scalar TBAA path from the parent struct in that case, so
  // conservatively omit TBAA.
  if (GEP->getSourceElementType()->isSingleValueType())
    return false;

  // %1 = gep %"ArrayLayout.xxx" addrspace(1)* %0, 0, 1, %x, 1
  // %2 = gep i64 addrspace(1)* %1, i64 1
  // load %2
  uint64_t Size =
      TBAAUnit.DL.getTypeSizeInBits(GEP->getSourceElementType()) / 8;
  uint64_t OriginOff = OuterIdx->getZExtValue() * Size;
  return prepareCJTBAA(TBAAUnit.DL, TBAAUnit.I, GEPGEP, TBAAUnit.DstTy, false,
                       OriginOff);
}

bool llvm::prepareCJTBAA(const DataLayout &DL, Instruction *I, Value *OP,
                         Type *DstTy, bool IsTBAAStruct, uint64_t OriginOff) {
  LLVMContext &Context = I->getContext();
  MDBuilder MDB(Context);
  Value *SrcOP = isa<GEPOperator>(OP) ? OP : OP->stripPointerCasts();
  auto GEP = dyn_cast<GEPOperator>(SrcOP);
  TBAAProcessUnit TBAAUnit(DL, MDB, Context, I, nullptr, DstTy, GEP, false,
                           OriginOff);
  APInt Offset(DL.getIndexSizeInBits(0u), 0);
  bool CanAccuOffset = true;
  if (!getSrcTyAndOffset(TBAAUnit, SrcOP, Offset, CanAccuOffset)) {
    return tryToInsertTBAAForGEPGEP(TBAAUnit);
  }
  if (!TBAAUnit.IsPrimitiveMetadata && TBAAUnit.SrcTy == nullptr &&
      !IsTBAAStruct) {
    return tryToInsertTBAAForGEPGEP(TBAAUnit);
  }
  if (preArrayLayout(TBAAUnit)) {
    if (IsArrayLayoutElement(TBAAUnit, DL.getStructLayout(TBAAUnit.SrcTy), GEP))
      return true;
    return tryToInsertTBAAForGEPGEP(TBAAUnit);
  }
  if (!CanAccuOffset) { // For this, we will not try again.
    return false;
  }
  if (IsTBAAStruct) {
    return insertCJTBAAStructImpl(TBAAUnit, TBAAUnit.SrcTy);
  } else {
    if (insertCJTBAAMeta(TBAAUnit, Offset))
      return true;
    return tryToInsertTBAAForGEPGEP(TBAAUnit);
  }
}

static MDNode *getTBAAStructTypeNode(Value *I, const DataLayout &DL,
                                     Instruction *UI) {
  // I: the base pointer
  Instruction *MDI = nullptr;
  Instruction *Ptr = nullptr;
  switch (UI->getOpcode()) {
  default:
    break;
  case Instruction::Load: {
    auto *LI = cast<LoadInst>(UI);
    Ptr = dyn_cast<Instruction>(LI->getPointerOperand());
    MDI = LI;
  } break;
  case Instruction::Store: {
    auto *SI = cast<StoreInst>(UI);
    Ptr = dyn_cast<Instruction>(SI->getPointerOperand());
    MDI = SI;
  } break;
  case Instruction::Call: {
    auto *CI = cast<CallInst>(UI);
    auto ID = CI->getIntrinsicID();
    if (ID == Intrinsic::cj_gcwrite_ref || ID == Intrinsic::cj_gcread_ref) {
      if (getPointerArg(CI)->stripInBoundsConstantOffsets() == I) {
        Ptr = dyn_cast<Instruction>(getPointerArg(CI));
        MDI = CI;
      }
    }
  } break;
  }
  if (Ptr) {
    // Ty1: type of MDI, Ty2: type of I
    // Ty1 maybe a sub struct of Ty2, and the tbaa of MDI is based on Ty1.
    // We want the tbaa based on the I's real type, so we need to check for
    // that.
    APInt Offset(DL.getIndexSizeInBits(0u), 0);
    auto *PtrBase = Ptr->stripAndAccumulateConstantOffsets(DL, Offset, true);
    // 1. I must be pointer base of memory instruction.
    if (PtrBase == I && MDI->hasMetadata(LLVMContext::MD_tbaa)) {
      uint64_t MDOffset =
          mdconst::extract<ConstantInt>(
              MDI->getMetadata(LLVMContext::MD_tbaa)->getOperand(2))
              ->getZExtValue(); // 2: Offset
      // 2. The offset obtained from accumulate and the offset obtained from
      // tbaa must be same.
      if (MDOffset == Offset.getZExtValue())
        return dyn_cast_or_null<MDNode>(MDI->getMetadata(LLVMContext::MD_tbaa)
                                            ->getOperand(0)); // 0: BaseTag
    }
  }
  return nullptr;
}

static MDNode *getInstStructTBAANode(const DataLayout &DL, Value *I) {
  // Find instructions that read or write the memory to I and get their tbaa.
  SmallVector<Value *, 8> WorkLists = {I};
  while (!WorkLists.empty()) {
    auto *TmpI = WorkLists.pop_back_val();
    for (Value *V : TmpI->users()) {
      if (isa<CallInst>(V) || isa<LoadInst>(V) || isa<StoreInst>(V))
        if (auto *N = getTBAAStructTypeNode(I, DL, cast<Instruction>(V)))
          return N;
      if (isa<BitCastInst>(V) || isa<AddrSpaceCastInst>(V) ||
          isa<GetElementPtrInst>(V))
        WorkLists.push_back(V);
    }
  }
  // Assert that all tbaa of them are the same.
  return nullptr;
}

// Update with sibling existing metadata
//          Base
//         /    \
//        I      S
// 1. Find the tbaa struct type tag of base through S;
// 2. Then calculate the offset from I to base;
// 3. Insert the tbaa of I based on 1 and 2.
// Some optimization points that can be done in the future: ref uses the
// exact type instead of i8 addrspace(1) *, so that the type can be read from
// the parent (base) tbaa, which can help the child instruction insert tbaa.
static bool updateBySibling(const DataLayout &DL, Instruction *I, Value *Ptr) {
  APInt Offset(DL.getIndexSizeInBits(0u), 0);
  auto *BasePtr = Ptr->stripAndAccumulateConstantOffsets(DL, Offset, true);
  // {i64, [0 x %record.T]}
  // Sibling: getelementptr %base, 0, 0
  // Ptr: getelementptr %base, 0, 1, i, 0
  // BasePtr is pointing to [0 x %record.T][i], underlying object is pointing to
  // %base, sibling is pointing to %base. If we dont check, the offset is
  // imprecise.
  if (!BasePtr || BasePtr != getUnderlyingObject(Ptr, 0))
    return false;
  auto *N = getInstStructTBAANode(DL, BasePtr);
  if (!N)
    return false;
  auto *BaseMD = N;
  std::string TypeStr =
      getTypeString(Ptr->getType()->getNonOpaquePointerElementType());
  uint64_t OffsetValue = Offset.getZExtValue();
  while (N && N->getNumOperands() > 1) {
    if (OffsetValue == 0)
      if (MDString *MDStr = dyn_cast<MDString>(N->getOperand(0)))
        if (MDStr->getString() == TypeStr)
          break;
    if (N->getNumOperands() == 2) {
      // {0*array_element, array_element}
      N = dyn_cast<MDNode>(N->getOperand(0));
      continue;
    }
    unsigned Idx = 1;
    assert(N->getNumOperands() >= 3 &&
           "TBAAStructType is illegal, please check!");
    for (; Idx < N->getNumOperands(); Idx += 2) {
      uint64_t CurOffset =
          mdconst::extract<ConstantInt>(N->getOperand(Idx + 1))->getZExtValue();
      if (CurOffset > OffsetValue)
        break;
    }
    assert(isa<ConstantAsMetadata>(N->getOperand(Idx - 1)) &&
           "TBAAStructType is illegal, please check!");
    OffsetValue -=
        mdconst::extract<ConstantInt>(N->getOperand(Idx - 1))->getZExtValue();
    N = dyn_cast<MDNode>(N->getOperand(Idx - 2));
  }
  // Not root
  if (N && N->getNumOperands() > 1) {
    MDBuilder MDB(I->getContext());
    auto *MD = MDB.createTBAAStructTagNode(BaseMD, N, Offset.getZExtValue());
    I->setMetadata(LLVMContext::MD_tbaa, MD);
    return true;
  }
  return false;
}

static bool maybeAllocaWithTwoDifferentType(Value *Ptr, Type *Ty,
                                            Instruction *I) {
  auto *BPtr = getUnderlyingObject(Ptr, 0);
  // Can't determine if they're alloca.
  if (isa<PHINode>(BPtr) || isa<SelectInst>(BPtr)) {
    I->setMetadata(LLVMContext::MD_tbaa, nullptr);
    return true;
  }
  if (isa<AllocaInst>(BPtr) && Ty != Ptr->stripPointerCasts()
                                         ->getType()
                                         ->getNonOpaquePointerElementType()) {
    I->setMetadata(LLVMContext::MD_tbaa, nullptr);
    return true;
  }
  return false;
}

// This function will only be called from other pass.
bool llvm::updateTBAA(const DataLayout &DL, Instruction *I) {
  Value *Ptr = nullptr;
  Type *Ty = nullptr;
  if (auto *LI = dyn_cast<LoadInst>(I)) {
    Ptr = LI->getPointerOperand();
    Ty = LI->getType();
  } else if (auto *SI = dyn_cast<StoreInst>(I)) {
    Ptr = SI->getPointerOperand();
    Ty = SI->getValueOperand()->getType();
  } else if (auto *II = dyn_cast<IntrinsicInst>(I)) {
    Intrinsic::ID ID = II->getIntrinsicID();
    switch (ID) {
    case Intrinsic::cj_gcwrite_ref:
      Ptr = getPointerArg(II);
      Ty = II->getOperand(0)->getType();
      break;
    case Intrinsic::cj_gcwrite_static_ref:
      Ptr = getPointerArg(II);
      Ty = II->getOperand(0)->getType();
      break;
    case Intrinsic::cj_gcread_ref:
      Ptr = getPointerArg(II);
      Ty = Ptr->getType()->getNonOpaquePointerElementType();
      break;
    case Intrinsic::cj_gcread_static_ref:
      Ptr = getPointerArg(II);
      Ty = Ptr->getType()->getNonOpaquePointerElementType();
      break;
    default:
      llvm_unreachable(
          "must be cangjie read/write barrier and not be struct barrier");
    }
  }
  if (maybeAllocaWithTwoDifferentType(Ptr, Ty, I))
    return false;
  if (I->hasMetadata(LLVMContext::MD_tbaa))
    return false;
  assert(!I->hasMetadata(LLVMContext::MD_tbaa) && "must have no tbaa");

  // Instructions such as load i56, [7 * i8]* %1 may appear, and we not
  // support insert tbaa medadata for i56. Therefore, for IntegerType , filter
  // its width.
  if (auto *IT = dyn_cast<IntegerType>(Ty)) {
    auto Width = IT->getBitWidth();
    if (Width != 1 && Width != 8 && Width != 16 && Width != 32 && Width != 64)
      return false;
  }
  // Currently, vector type also not supported.
  if (Ty->isVectorTy())
    return false;
  // These instructions should not use tbaa.struct.
  I->setMetadata(LLVMContext::MD_tbaa_struct, nullptr);
  return prepareCJTBAA(DL, I, Ptr, Ty) || updateBySibling(DL, I, Ptr);
}

static bool insertCJTBAA(Function &F) {
  const DataLayout &DL = F.getParent()->getDataLayout();
  bool Changed = false;
  for (auto I = inst_begin(F); I != inst_end(F);) {
    Instruction *Inst = &*I++;
    if (auto *LI = dyn_cast<LoadInst>(Inst)) {
      Type *Ty = LI->getType();
      if (isa<StructType>(Ty) || isa<ArrayType>(Ty)) {
        Changed |= prepareCJTBAA(DL, LI, LI->getPointerOperand(),
                                 LI->getType(), true);
      } else {
        Changed |= prepareCJTBAA(DL, LI, LI->getPointerOperand(), LI->getType());
      }
    } else if (auto *SI = dyn_cast<StoreInst>(Inst)) {
      Type *Ty = SI->getValueOperand()->getType();
      if (isa<StructType>(Ty) || isa<ArrayType>(Ty)) {
        Changed |= prepareCJTBAA(DL, SI, SI->getPointerOperand(),
                                 SI->getValueOperand()->getType(), true);
      } else {
        Changed |= prepareCJTBAA(DL, SI, SI->getPointerOperand(),
                                 SI->getValueOperand()->getType());
      }
    } else if (auto *CB = dyn_cast<CallBase>(Inst)) {
      unsigned OpNO;
      bool IsTBAAStruct = true;
      Type *DstTy = nullptr;
      switch (CB->getIntrinsicID()) {
      default:
        continue;
      case Intrinsic::memcpy:
      case Intrinsic::memset:
      case Intrinsic::memmove:
      case Intrinsic::cj_memset:
      case Intrinsic::cj_gcread_struct:
      case Intrinsic::cj_gcread_static_struct:
      case Intrinsic::cj_gcwrite_static_struct:
        // 0: Dst Operand for these
        OpNO = 0;
        break;
      case Intrinsic::cj_gcread_static_ref:
        OpNO = 0;
        DstTy = getPointerArg(CB)->getType()->getNonOpaquePointerElementType();
        IsTBAAStruct = false;
        break;
      case Intrinsic::cj_gcread_ref:
        OpNO = 1;
        DstTy = getPointerArg(CB)->getType()->getNonOpaquePointerElementType();
        IsTBAAStruct = false;
        break;
      case Intrinsic::cj_gcwrite_ref: {
        // 2: Dst Operand for these
        OpNO = 2;
        DstTy = getValueArg(CB)->getType();
        IsTBAAStruct = false;
        break;
      }
      case Intrinsic::cj_gcwrite_static_ref:
      case Intrinsic::cj_gcwrite_struct:
      case Intrinsic::cj_array_copy_ref:
      case Intrinsic::cj_array_copy_struct:
        // 1: Dst Operand for these
        OpNO = 1;
        break;
      }
      Changed |=
          prepareCJTBAA(DL, CB, CB->getArgOperand(OpNO), DstTy, IsTBAAStruct);
    }
  }
  return Changed;
}

PreservedAnalyses InsertCJTBAA::run(Function &F,
                                    AnalysisManager<Function> &) const {
  if (insertCJTBAA(F)) {
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

