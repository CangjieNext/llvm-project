//===- CJFillMetadata.cpp - Fill Cangjie Metadata ---------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Fill Cangjie Metadata for global varible such as TypeInfo and ReflectionInfo.
//
//===----------------------------------------------------------------------===//
#include "llvm/Transforms/Scalar/CJFillMetadata.h"

#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/CJStructTypeGCInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/ReflectionInfo.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
using namespace llvm;

namespace {
const char *CFileKlassStr = "CFileKlass";
} // namespace

StructType *llvm::getTypeLayoutType(GlobalVariable *GV) {
  const MDNode *Metadata = GV->getMetadata("RelatedType");
  if (!Metadata)
    return nullptr;

  StringRef TypeName =
      dyn_cast<MDString>(Metadata->getOperand(0).get())->getString();
  return StructType::getTypeByName(GV->getContext(), TypeName);
}

std::string llvm::getPrimitiveTypeName(Type *Ty) {
  if (auto PT = dyn_cast<PointerType>(Ty)) {
    if (PT->getElementType()->isIntegerTy(8)) {
      if (PT->getAddressSpace() == 1) {
        return "array_ref";
      } else {
        return "array_cptr";
      }
    }
    if (PT->getElementType()->isFunctionTy()) {
      return "array_cfunc";
    }
  }
  if (Ty->isIntegerTy()) {
    switch (Ty->getIntegerBitWidth()) {
    default:
      break;
    case 1:
      return "array_bool";
    case 8:
      return "array_int8";
    case 16:
      return "array_int16";
    case 32:
      return "array_int32";
    case 64:
      return "array_int64";
    }
  }
  if (Ty->is16bitFPTy()) {
    return "array_float16";
  }
  if (Ty->isFloatTy()) {
    return "array_float32";
  }
  if (Ty->isDoubleTy()) {
    return "array_float64";
  }
  if (Ty->isStructTy()) {
    return Ty->getStructName().str();
  }
  report_fatal_error("Other type for array to adapt!");
}

const static DenseSet<StringRef> PrimitiveTypeInfo{
    "Unit.ti",      "Nothing.ti",    "Bool.ti",    "Rune.ti",    "Int8.ti",
    "UInt8.ti",     "Int16.ti",      "UInt16.ti",  "Int32.ti",   "UInt32.ti",
    "Int64.ti",     "UInt64.ti",     "Float16.ti", "Float32.ti", "Float64.ti",
    "IntNative.ti", "UIntNative.ti", "CString.ti"};

bool llvm::isPrimitiveTypeInfo(GlobalVariable *GV) {
  return PrimitiveTypeInfo.contains(GV->getName());
}

unsigned llvm::getValueTypeSize(GlobalVariable *GV) {
  assert(isPrimitiveTypeInfo(GV) && "Not primitive type!");
  return StringSwitch<unsigned>(GV->getName())
      .Case("Unit.ti", 0)
      .Case("Nothing.ti", 0)
      .Case("Bool.ti", 1)
      .Case("Rune.ti", 4)
      .Case("Int8.ti", 1)
      .Case("UInt8.ti", 1)
      .Case("Int16.ti", 2)
      .Case("UInt16.ti", 2)
      .Case("Int32.ti", 4)
      .Case("UInt32.ti", 4)
      .Case("Int64.ti", 8)
      .Case("UInt64.ti", 8)
      .Case("Float16.ti", 2)
      .Case("Float32.ti", 4)
      .Case("Float64.ti", 8)
      .Case("IntNative.ti", 8)
      .Case("UIntNative.ti", 8)
      .Case("CString.ti", 8)
      .Default(0);
}

// The first of result indicates whether it is known, and the second is its
// type kind.
std::pair<bool, TypeKind> llvm::getKnownTypeKind(GlobalVariable *GV) {
  if (isPrimitiveTypeInfo(GV))
    return std::make_pair(true, TK_INT64);

  StringRef Name = GV->getName();
  TypeKind Kind = StringSwitch<TypeKind>(Name)
                      .Case("std.core:Object.ti", TK_CLASS)
                      .Case("std.core:String.ti", TK_STRUCT)
                      .Case("std.core:Thread.ti", TK_CLASS)
                      .Case("std.core:Any.ti", TK_INTERFACE)
                      .Case("std.core:Duration.ti", TK_STRUCT)
                      .Case("std.core:Hashable.ti", TK_INTERFACE)
                      .Case("std.core:Ordering.ti", TK_ENUM)
                      .Case("std.core:Resource.ti", TK_INTERFACE)
                      .Case("std.core:ToString.ti", TK_INTERFACE)
                      .Case("std.core:Exception.ti", TK_CLASS)
                      .Default(TK_MAX);
  if (Kind != TK_MAX)
    return std::make_pair(true, Kind);

  // Check whether the type is an instantiated generic type, and its name uses
  // the form xxx<T>, the xxx is in the trustlist.
  size_t Pos = Name.find('<');
  if (Pos == StringRef::npos)
    return std::make_pair(false, Kind);

  StringRef TTName = Name.substr(0, Pos);
  Kind = StringSwitch<TypeKind>(TTName)
             .Case("Tuple", TK_TUPLE)
             .Case("Func", TK_FUNC)
             .Case("VArray", TK_VARRAY)
             .Case("RawArray", TK_RAWARRAY)
             .Case("CPointer", TK_CPOINTER)
             .Case("CFunc", TK_CFUNC)
             .Case("std.core:Comparable", TK_INTERFACE)
             .Case("std.core:Option", TK_TEMP_ENUM)
             .Case("std.core:ArrayIterator", TK_CLASS)
             .Case("std.core:Range", TK_STRUCT)
             .Case("std.core:Array", TK_STRUCT)
             .Case("std.core:Box", TK_CLASS)
             .Case("std.core:Collection", TK_INTERFACE)
             .Case("std.core:Iterable", TK_INTERFACE)
             .Case("std.core:Iterator", TK_CLASS)
             .Case("std.core:ArrayIterator", TK_CLASS)
             .Case("std.core:AtomicBox", TK_CLASS)
             .Case("std.core:Equal", TK_INTERFACE)
             .Default(TK_MAX);
  if (Kind != TK_MAX)
    return std::make_pair(true, Kind);
  else
    return std::make_pair(false, Kind);
}

bool llvm::hasRunCangjieOpt(Module &M) {
  if (auto MDValue = mdconst::extract_or_null<ConstantInt>(
          M.getModuleFlag("Cangjie_OPT"))) {
    if (MDValue->getZExtValue()) {
      return true;
    }
  }
  return false;
}

TypeTemplate::TypeTemplate(GlobalVariable *GV) : GV(GV) {
  assert(GV->hasInitializer() && "TypeTemplate GV has no Initializer!");
  TT = dyn_cast<ConstantStruct>(GV->getInitializer());
  int64_t T = cast<ConstantInt>(TT->getOperand(TT_TYPE))->getSExtValue();
  assert(T < TK_MAX && "TypeTempate type kind is error!");
  Kind = static_cast<TypeKind>(T);
}

bool TypeTemplate::isEnum() { return Kind == TK_ENUM || Kind == TK_TEMP_ENUM; }

bool TypeTemplate::isSyncClass() {
  return GV->hasAttribute("Future") || GV->hasAttribute("Monitor") ||
         GV->hasAttribute("Mutex") || GV->hasAttribute("WaitQueue");
}

Constant *TypeTemplate::get(unsigned Idx) {
  return cast<Constant>(TT->getOperand(Idx));
}

TypeKind TypeTemplate::getKind() {
  int64_t T = cast<ConstantInt>(TT->getOperand(TT_TYPE))->getSExtValue();
  assert(T < TK_MAX && "TypeTemplate type kind is error!");
  return static_cast<TypeKind>(T);
}

uint8_t TypeTemplate::getFlag() {
  uint64_t Flag = cast<ConstantInt>(get(TT_FLAG))->getZExtValue();
  return static_cast<uint8_t>(Flag);
}

unsigned TypeTemplate::getFieldNum() {
  return cast<ConstantInt>(TT->getOperand(TT_FIELD_NUM))->getZExtValue();
}

unsigned TypeTemplate::getTypeArgNum() {
  return cast<ConstantInt>(TT->getOperand(TT_TYPE_ARG_NUM))->getZExtValue();
}

TypeInfo::TypeInfo(GlobalVariable *GV) : GV(GV) {
  assert(GV->hasInitializer() && "GV has no Initializer!");
  Data = dyn_cast<ConstantStruct>(GV->getInitializer());
  int64_t T = cast<ConstantInt>(Data->getOperand(CIT_TYPE))->getSExtValue();
  assert(T < TK_MAX && "Data type kind is error!");
  Kind = static_cast<TypeKind>(T);
}

bool TypeInfo::isStructType() {
  return Kind == TK_STRUCT || Kind == TK_TUPLE || Kind == TK_ENUM;
}

bool TypeInfo::isPrimitiveType() {
  return Kind == TK_UNIT || Kind == TK_BOOL || Kind == TK_UINT8 ||
         Kind == TK_UINT16 || Kind == TK_UINT32 || Kind == TK_UINT64 ||
         Kind == TK_INT8 || Kind == TK_INT16 || Kind == TK_INT32 ||
         Kind == TK_INT64 || Kind == TK_FLOAT16 || Kind == TK_FLOAT32 ||
         Kind == TK_FLOAT64;
}

bool TypeInfo::isReferenceType() { return Kind < 0; }

bool TypeInfo::isArray() { return Kind == TK_RAWARRAY; }

bool TypeInfo::isEnum() { return Kind == TK_ENUM || Kind == TK_TEMP_ENUM; }

bool TypeInfo::isTuple() { return Kind == TK_TUPLE; }

bool TypeInfo::isVarray() { return Kind == TK_VARRAY; }

bool TypeInfo::isFunction() { return Kind == TK_FUNC; }

bool TypeInfo::isClass() { return Kind == TK_CLASS || Kind == TK_TEMP_ENUM; }

bool TypeInfo::isInterface() { return Kind == TK_INTERFACE; }

bool TypeInfo::isCType() {
  return Kind == TK_CSTRING || Kind == TK_CPOINTER || Kind == TK_CFUNC;
}

bool TypeInfo::isPureValueType() {
  return isPrimitiveType() || isVarray() || isCType();
}

bool TypeInfo::isSyncClass() {
  return GV->hasAttribute("Future") || GV->hasAttribute("Monitor") ||
         GV->hasAttribute("Mutex") || GV->hasAttribute("WaitQueue");
}

bool TypeInfo::isGenericType() { return getTypeArgNum() > 0; }

Value *TypeInfo::getReferenceType(GlobalVariable *GV) {
  Value *Ret = nullptr;
  LLVMContext &C = GV->getContext();
  if (GV->hasInitializer()) {
    TypeInfo TI(GV);
    Ret = ConstantInt::get(Type::getInt1Ty(C), TI.isReferenceType());
    return Ret;
  }
  auto P = getKnownTypeKind(GV);
  if (P.first) {
    bool IsRef = P.second < 0 ? true : false;
    Ret = ConstantInt::get(Type::getInt1Ty(C), IsRef);
  }
  return Ret;
}

Constant *TypeInfo::get(unsigned Idx) {
  return cast<Constant>(Data->getOperand(Idx));
}

uint8_t TypeInfo::getTypeFlag() {
  uint64_t Flag = cast<ConstantInt>(get(CIT_FLAG))->getZExtValue();
  return static_cast<uint8_t>(Flag);
}

unsigned TypeInfo::getSize() {
  return cast<ConstantInt>(get(CIT_SIZE))->getZExtValue();
}

unsigned TypeInfo::getFieldNum() {
  return cast<ConstantInt>(get(CIT_FIELD_NUM))->getZExtValue();
}

unsigned TypeInfo::getTypeArgNum() {
  return cast<ConstantInt>(get(CIT_TYPE_ARG_NUM))->getZExtValue();
}

Constant *TypeInfo::getGCTib() { return get(CIT_GCTIB); }

unsigned TypeInfo::getAlign() {
  return cast<ConstantInt>(get(CIT_ALIGN))->getZExtValue();
}

Constant *TypeInfo::getOffsets() {
  auto C = cast<ConstantExpr>(get(CIT_OFFSETS))->getOperand(0);
  auto GV = cast<GlobalVariable>(C);
  assert(GV->hasInitializer() && "Field GV has no Initializer!");
  return GV->getInitializer();
}

unsigned TypeInfo::getOffset(unsigned Idx) {
  Constant *OffsetsGV = getOffsets();
  if (OffsetsGV->isZeroValue())
    return 0;

  auto Offsets = dyn_cast<ConstantDataArray>(OffsetsGV);
  assert(Idx < Offsets->getNumElements() && "getOffset index is error");
  return Offsets->getElementAsInteger(Idx);
}

Constant *TypeInfo::getFields() {
  auto C = cast<ConstantExpr>(get(CIT_FIELD))->getOperand(0);
  auto GV = cast<GlobalVariable>(C);
  assert(GV->hasInitializer() && "Field GV has no Initializer!");
  return GV->getInitializer();
}

GlobalVariable *TypeInfo::getSuperOrComponent() {
  return dyn_cast_or_null<GlobalVariable>(get(CIT_SUPER));
}

GlobalVariable *TypeInfo::getField(unsigned Idx) {
  Constant *FieldGV = getFields();
  assert(Idx < FieldGV->getNumOperands() && "getField index is error");
  return cast<GlobalVariable>(FieldGV->getOperand(Idx));
}

Constant *TypeInfo::getTypeArgs() {
  auto C = cast<ConstantExpr>(get(CIT_TYPE_ARG))->getOperand(0);
  auto GV = cast<GlobalVariable>(C);
  assert(GV->hasInitializer() && "TypeArg GV has no Initializer!");
  return cast<Constant>(GV->getInitializer());
}

GlobalVariable *TypeInfo::getTypeArg(unsigned Idx) {
  Constant *TypeArgGV = getTypeArgs();
  assert(Idx < TypeArgGV->getNumOperands() && "getTypeArg index is error");
  return cast<GlobalVariable>(TypeArgGV->getOperand(Idx));
}

GlobalVariable *TypeInfo::getTypeTemplate() {
  if (!hasTypeTempate())
    return nullptr;

  auto GV =
      dyn_cast<GlobalVariable>(get(CIT_GENERIC_FROM)->stripPointerCasts());
  if (!GV->hasInitializer())
    return nullptr;

  return GV;
}

StructType *TypeInfo::getLayoutType() {
  const MDNode *Metadata = GV->getMetadata("RelatedType");
  if (!Metadata)
    return nullptr;

  StringRef TypeName =
      dyn_cast<MDString>(Metadata->getOperand(0).get())->getString();
  return StructType::getTypeByName(GV->getContext(), TypeName);
}

Type *TypeInfo::getArrayElementType() {
  assert(isArray() && "Illegal raw array type.");
  StructType *ST = getLayoutType();
  assert(ST->getNumElements() == 2 && "ArrayLayout's elemNums should be 2");
  auto *AT = dyn_cast<ArrayType>(ST->getElementType(1));
  assert(AT != nullptr && "ArrayLayout[1] should be ArrayType");
  return AT->getElementType();
}

bool TypeInfo::hasRefField() { return getTypeFlag() & TF_HAS_REF_FIELD; }

bool TypeInfo::hasTypeTempate() {
  Constant *Temp = get(CIT_GENERIC_FROM);
  if (Temp->isNullValue())
    return false;

  return isa<GlobalVariable>(Temp->stripPointerCasts());
}

void TypeInfo::resetTypeInfo(GlobalVariable *GV) {
  this->GV = GV;
  assert(GV->hasInitializer() && "GV has no Initializer!");
  Data = dyn_cast<ConstantStruct>(GV->getInitializer());
  int64_t T = cast<ConstantInt>(Data->getOperand(CIT_TYPE))->getSExtValue();
  assert(T < TK_MAX && "Data type kind is error!");
  Kind = static_cast<TypeKind>(T);
}

ExtensionDefData::ExtensionDefData(GlobalVariable *GV) {
  assert(GV->hasInitializer() && "ExtensionDef GV has no Initializer!");
  ConstantStruct *Data = dyn_cast<ConstantStruct>(GV->getInitializer());
  TypeParamCount =
      cast<ConstantInt>(Data->getOperand(ET_TYPE_PARAM_COUNT))->getSExtValue();
  IsInterfaceTypeInfo =
      cast<ConstantInt>(Data->getOperand(ET_IS_INTERFACE_TYPEINFO))
          ->getZExtValue();
  Flag = cast<ConstantInt>(Data->getOperand(ET_FLAG))->getZExtValue();
  FuncTableSize =
      cast<ConstantInt>(Data->getOperand(ET_FUNC_TABLE_SIZE))->getSExtValue();
  auto CE = dyn_cast<ConstantExpr>(Data->getOperand(ET_TARGET_TYPE));
  TargetType = dyn_cast<GlobalVariable>(CE->getOperand(0));
  CE = dyn_cast<ConstantExpr>(Data->getOperand(ET_INTERFACE_FN));
  if (IsInterfaceTypeInfo == 0) {
    InterfaceFn = dyn_cast<Function>(CE->getOperand(0));
  } else {
    InterfaceTypeInfo = dyn_cast<GlobalVariable>(CE->getOperand(0));
  }
  auto Field = cast<Constant>(Data->getOperand(ET_WHERE_COND_FN));
  if (!Field->isNullValue()) {
    CE = dyn_cast<ConstantExpr>(Field);
    WhereCondFn = dyn_cast<Function>(CE->getOperand(0));
  }
  Field = cast<Constant>(Data->getOperand(ET_FUNC_TABLE));
  if (!Field->isNullValue()) {
    CE = dyn_cast<ConstantExpr>(Field);
    FuncTable = dyn_cast<GlobalVariable>(CE->getOperand(0));
  }
}

Function *ExtensionDefData::getFuncByIndex(unsigned Idx) {
  assert(FuncTable && "ExtensionDef has no FuncTable!");
  assert(FuncTable->hasInitializer() && "FuncTable GV has no Initializer!");
  ConstantArray *Tbl = dyn_cast<ConstantArray>(FuncTable->getInitializer());
  return cast<Function>(Tbl->getOperand(Idx)->stripPointerCasts());
}

GlobalVariable *RefValue::findTypeInfoGV() {
  getTypeInfoGV(Ref);
  if (TypeVec.size() == 1 && !TypeVec.count(nullptr)) {
    return cast<GlobalVariable>(TypeVec.begin()->first);
  }
  return nullptr;
}

void RefValue::getTypeInfoGV(Value *V) {
  V = V->stripPointerCasts();
  if (!VisitedValue.insert(V).second) {
    return;
  }
  if (auto *LI = dyn_cast<LoadInst>(V)) {
    getTypeInfoGVLoad(LI->getPointerOperand());
  } else if (auto *Arg = dyn_cast<Argument>(V)) {
    int ArgNo = Arg->getArgNo();
    Function *CurFunc = Arg->getParent();
    if (!CurFunc->hasInternalLinkage()) {
      TypeVec[nullptr]++;
      return;
    }
    getTypeInfoGVParam(CurFunc, ArgNo);
  } else if (auto *CI = dyn_cast<CallBase>(V)) {
    if (!isNewObjectCallSite(CI)) {
      TypeVec[nullptr]++;
      return;
    }
    getTypeInfoGV(CI->getOperand(0));
  } else if (auto *GV = dyn_cast<GlobalVariable>(V)) {
    if (!GV->hasAttribute("CFileKlass")) {
      report_fatal_error("Incorrect typeinfo!");
    }
    TypeVec[GV]++;
  } else {
    TypeVec[nullptr]++;
  }
}

void RefValue::getTypeInfoGVLoad(Value *V) {
  if (auto *GV = dyn_cast<GlobalVariable>(V)) {
    // %obj = call i8 addrspace(1)* @llvm.cj.heapmalloc.class
    // call @gcwrite.static (i8 addrspace(1)* %obj, i8 addrspace(1)** %GV)
    // %ref = call i8 addrspace(1)* @gcread.static (i8 addrspace(1)** %GV)
    auto *MD = GV->getMetadata("cjType");
    if (!MD) {
      TypeVec[nullptr]++;
      return;
    }
    std::string TypeStr =
        dyn_cast<MDString>(MD->getOperand(0).get())->getString().str();
    Module *M = GV->getParent();
    auto *ObjTypeInfo = M->getGlobalVariable(TypeStr);
    TypeVec[ObjTypeInfo]++;
    return;
  }
  if (auto *AI = dyn_cast<AllocaInst>(V)) {
    // %ins = alloca i8 addrspace(1)*, align 8
    // %obj = call i8 addrspace(1)* @llvm.cj.heapmalloc.class
    // store i8 addrspace(1)* %obj, i8 addrspace(1)** %ins, align 8
    // %ref = load i8 addrspace(1)*, i8 addrspace(1)** %ins, align 8
    if (AI->getAllocatedType() ==
        Type::getInt8PtrTy(AI->getFunction()->getContext(), 1)) {
      for (User *U : AI->users()) {
        if (StoreInst *SI = dyn_cast<StoreInst>(U)) {
          getTypeInfoGV(SI->getValueOperand()->stripPointerCasts());
        }
      }
      return;
    }
  }
  TypeVec[nullptr]++;
}

void RefValue::getTypeInfoGVParam(Function *F, int ArgNo) {
  for (auto I = F->user_begin(), E = F->user_end(); I != E;) {
    User *U = *(I++);
    auto *GetFunc = dyn_cast<CallBase>(U);
    if (GetFunc == nullptr) {
      // Maybe func stored to other field. Now we can't analysis the type of
      // param.
      TypeVec[nullptr]++;
      return;
    }
    getTypeInfoGV(U->getOperand(ArgNo));
  }
}

bool RefValue::isNewObjectCallSite(CallBase *CI) {
  assert(CI && "NewObject callsite is nullptr");
  unsigned ID = CI->getIntrinsicID();
  if (ID == Intrinsic::cj_malloc_object)
    return true;

  Function *Callee = CI->getCalledFunction();
  if (Callee && Callee->hasName()) {
    StringRef FuncName = Callee->getName();
    if (FuncName.isCangjieNewObjFunction() ||
        FuncName.equals("CJ_MCC_NewPinnedObject"))
      return true;
  }
  return false;
}

GlobalVariable *llvm::findTypeInfoGV(Value *V) {
  V = V->stripPointerCasts();
  if (auto *GV = dyn_cast<GlobalVariable>(V)) {
    return GV;
  }

  // process the following case:
  // %temp = bitcast i8 addrspace(1)* %obj to i8* addrspace(1)*
  // %typeinfo = load i8*, i8* addrspace(1)* %temp, align 8
  if (LoadInst *LI = dyn_cast<LoadInst>(V)) {
    Value *RefPtr = LI->getPointerOperand()->stripPointerCasts();
    RefValue Ref(RefPtr);
    return Ref.findTypeInfoGV();
  }
  return nullptr;
}

StringRef llvm::getStringFromMD(const MDTuple *MD, unsigned Idx) {
  return dyn_cast<MDString>(MD->getOperand(Idx).get())->getString();
}

MDTuple *llvm::getMDOperand(const MDTuple *MD, unsigned Idx) {
  return dyn_cast<MDTuple>(MD->getOperand(Idx).get());
}

unsigned llvm::getTypeModifier(StringRef Name) {
  constexpr StringRef REFLECT_VERSION_PREFIX = "reflectVersion";
  constexpr size_t REFLECT_VERSION_PREFIX_LEN = REFLECT_VERSION_PREFIX.size();
  constexpr unsigned MAX_REFLECT_VERSION = 7; // 3 bits: 0b111
  constexpr unsigned VERSION_BIT_MASK_1 = 1; // 0b001
  constexpr unsigned VERSION_BIT_MASK_2 = 2; // 0b010
  constexpr unsigned VERSION_BIT_MASK_3 = 4; // 0b100

  if (Name.startswith(REFLECT_VERSION_PREFIX)) {
    StringRef VersionStr = Name.substr(REFLECT_VERSION_PREFIX_LEN);
    unsigned Version = 0;
    if (!VersionStr.getAsInteger(10, Version) &&
        Version > 0 &&
        Version <= MAX_REFLECT_VERSION) {
      unsigned Result = 0;
      if (Version & VERSION_BIT_MASK_1)
        Result |= RMT_REFLECT_VER_BIT1;
      if (Version & VERSION_BIT_MASK_2)
        Result |= RMT_REFLECT_VER_BIT2;
      if (Version & VERSION_BIT_MASK_3)
        Result |= RMT_REFLECT_VER_BIT3;
      return Result;
    }
    return RMT_MAX;
  }

  return StringSwitch<unsigned>(Name)
      .Case("default", RMT_DEFAULT)
      .Case("private", RMT_PRIVATE)
      .Case("protected", RMT_PROTECTED)
      .Case("public", RMT_PUBLIC)
      .Case("immutable", RMT_IMMUTABLE)
      .Case("box", RMT_BOXCLASS)
      .Case("open", RMT_OPEN)
      .Case("override", RMT_OVERRIDE)
      .Case("redef", RMT_REDEF)
      .Case("abstract", RMT_ABSTRACT)
      .Case("sealed", RMT_SEALED)
      .Case("mut", RMT_MUT)
      .Case("static", RMT_STATIC)
      .Case("enumKind0", RMT_ENUM_KIND0)
      .Case("enumKind1", RMT_ENUM_KIND1)
      .Case("enumKind2", RMT_ENUM_KIND2)
      .Case("enumKind3", RMT_ENUM_KIND3)
      .Case("hasSRet0", RMT_HAS_SRET0)
      .Case("hasSRet1", RMT_HAS_SRET1)
      .Case("hasSRet2", RMT_HAS_SRET2)
      .Case("hasSRet3", RMT_HAS_SRET3)
      .Case("enumCtor", RMT_ENUM_CTOR)
      .Case("unknownSize", RMT_UNKNOWN_SIZE)
      .Default(RMT_MAX);
}

static Constant *getOrInsertConstStringGV(Module &M, StringRef Str) {
  std::string Name = (Str.empty() ? "NullString" : Str.str()) + ".reflectStr";
  GlobalVariable *StrGV = M.getNamedGlobal(Name);
  if (StrGV == nullptr) {
    Constant *StrContent = ConstantDataArray::getString(M.getContext(), Str);
    StrGV = dyn_cast<GlobalVariable>(
        M.getOrInsertGlobal(Name, StrContent->getType()));
    StrGV->setInitializer(StrContent);
    StrGV->setLinkage(GlobalValue::InternalLinkage);
    StrGV->addAttribute("CFileReflect");
    StrGV->setAlignment(Align(1));
  }
  return StrGV;
}

static Constant *createReflectGV(Module &M, std::string &Str, StructType *ST,
                                 SmallVectorImpl<Constant *> &BodyVec) {
  GlobalVariable *GV = dyn_cast<GlobalVariable>(M.getOrInsertGlobal(Str, ST));
  GV->setInitializer(ConstantStruct::get(ST, BodyVec));
  GV->setLinkage(GlobalValue::InternalLinkage);
  GV->addAttribute("CFileReflect");
  return GV;
}

static GlobalVariable *getNameGlobal(Module &M, StringRef Name) {
  GlobalVariable *GV = M.getNamedGlobal(Name);
  if (GV == nullptr) {
    report_fatal_error("Can't find " + Twine(Name.str()) +
                       " global variable for reflection");
  }
  return GV;
}

Constant *BaseInfo::createGlobalVal(std::string &Str, StringRef TypeName,
                                    SmallVectorImpl<Constant *> &BodyVec) {
  StructType *ST = StructType::getTypeByName(C, TypeName);
  if (!ST) {
    SmallVector<Type *> TypeVec(BodyVec.size());
    for (unsigned Idx = 0; Idx < BodyVec.size(); ++Idx) {
      TypeVec[Idx] = BodyVec[Idx]->getType();
    }
    ST = StructType::create(C, TypeVec, TypeName);
  }
  return createReflectGV(M, Str, ST, BodyVec);
}

Constant *ClassReflectInfo::fillInstanceFieldNames(StringRef TIName) {
  unsigned Cnt = FieldNames.size();
  if (Cnt == 0)
    return Int8PtrNull;

  SmallVector<Constant *, 0> Body;
  for (auto Name : FieldNames) {
    Constant *NameGV = getOrInsertConstStringGV(M, Name);
    Body.push_back(ConstantExpr::getBitCast(NameGV, Int8PtrTy));
  }
  std::string GVName = TIName.str() + ".fieldnames";
  std::string TypeName = "reflect.fieldNamesType" + std::to_string(Cnt);
  Constant *GV = createGlobalVal(GVName, TypeName, Body);
  return ConstantExpr::getBitCast(GV, Int8PtrTy);
}

Constant *EnumReflectInfo::fillEnumCtorInfos(StringRef TypeInfoName) {
  if (EnumCtors.empty())
    return Int8PtrNull;

  SmallVector<Constant *, 0> BodyVec;
  for (unsigned Idx = 0; Idx < EnumCtors.size(); ++Idx) {
    EnumCtorInfo &Info = EnumCtors[Idx];
    BodyVec.push_back(getOrInsertConstStringGV(M, Info.CtorName));
    if (Info.TypeName.empty()) {
      BodyVec.push_back(Int8PtrNull);
    } else {
      Function *Func = M.getFunction(Info.TypeName);
      if (Func == nullptr) {
        report_fatal_error("Can't find " + Twine(Info.TypeName.str()) +
                           " enum type for reflection");
      }
      BodyVec.push_back(ConstantExpr::getBitCast(Func, Int8PtrTy));
    }
  }
  std::string GVName = TypeInfoName.str() + ".enumctors";
  Constant *EnumCtorsGV = createGlobalVal(GVName, GVName + "Type", BodyVec);
  return ConstantExpr::getBitCast(EnumCtorsGV, Int8PtrTy);
}

Constant *MethodInfo::fillMethodInfo(unsigned Idx, StringRef TIName) {
  std::string FuncGVName;
  if (IsStatic) {
    if (MethodLinkName.empty()) {
      FuncGVName = TIName.str() + ".staticMethod" + std::to_string(Idx);
    } else {
      FuncGVName = MethodLinkName.str() + ".staticMethod";
    }
  } else {
    FuncGVName = TIName.str() + ".method" + std::to_string(Idx);
  }
  Constant *GlobalVar = M.getGlobalVariable(FuncGVName);
  if (!GlobalVar) {
    SmallVector<Constant *, 0> Body;
    Constant *Name = getOrInsertConstStringGV(M, MethodName);
    Body.push_back(ConstantExpr::getBitCast(Name, Int8PtrTy));
    Body.push_back(ConstantInt::get(Int32Ty, Modifier));
    Body.push_back(ConstantInt::get(Int16Ty, ActualParams.size()));
    Body.push_back(ConstantInt::get(Int16Ty, GenericParams.size()));
    Function *MethodAddr = M.getFunction(MethodLinkName);
    if (MethodAddr == nullptr) {
      Body.push_back(Int8PtrNull);
    } else {
      Body.push_back(ConstantExpr::getBitCast(MethodAddr, Int8PtrTy));
    }
    Constant *ReturnTI = getNameGlobal(M, ReturnType);
    Body.push_back(ConstantExpr::getBitCast(ReturnTI, Int8PtrTy));
    Body.push_back(Annotation);
    if (TIName.empty()) {
      Body.push_back(Int8PtrNull);
    } else {
      Constant *TI = getNameGlobal(M, TIName);
      Body.push_back(ConstantExpr::getBitCast(TI, Int8PtrTy));
    }
    Body.push_back(fillActualParamInfo(FuncGVName));
    Body.push_back(fillGenericParamInfo(FuncGVName));
    GlobalVar = createGlobalVal(FuncGVName, "reflect.methodType", Body);
  }
  return ConstantExpr::getBitCast(GlobalVar, Int8PtrTy);
}

Constant *MethodInfo::fillActualParamInfo(StringRef Prefix) {
  unsigned Cnt = ActualParams.size();
  if (Cnt == 0) {
    return Int8PtrNull;
  }
  std::string ParamGVName = Prefix.str() + ".actualParam";
  SmallVector<Constant *, 0> Body;
  for (unsigned Idx = 0; Idx < Cnt; ++Idx) {
    ParamInfo &PI = ActualParams[Idx];
    Constant *Name = getOrInsertConstStringGV(M, PI.ParamName);
    Body.push_back(ConstantExpr::getBitCast(Name, Int8PtrTy));
    Body.push_back(ConstantInt::get(Int32Ty, Idx));
    Constant *TypeTI = getNameGlobal(M, PI.TypeName);
    Body.push_back(ConstantExpr::getBitCast(TypeTI, Int8PtrTy));
    Body.push_back(ActualParams[Idx].Annotation);
  }
  std::string TypeName = "reflect.actualParamType" + std::to_string(Cnt);
  Constant *GV = createGlobalVal(ParamGVName, TypeName, Body);
  return ConstantExpr::getBitCast(GV, Int8PtrTy);
}

Constant *MethodInfo::fillGenericParamInfo(StringRef Prefix) {
  unsigned Cnt = GenericParams.size();
  if (Cnt == 0) {
    return Int8PtrNull;
  }
  std::string ParamGVName = Prefix.str() + ".genericParam";
  SmallVector<Constant *, 0> Body;
  for (unsigned Idx = 0; Idx < Cnt; ++Idx) {
    Body.push_back(getNameGlobal(M, GenericParams[Idx]));
  }
  std::string TypeName = "reflect.genericParamType" + std::to_string(Cnt);
  Constant *GV = createGlobalVal(ParamGVName, TypeName, Body);
  return ConstantExpr::getBitCast(GV, Int8PtrTy);
}

Constant *FieldInfo::fillInstanceFieldInfo(unsigned Idx, StringRef TIName) {
  std::string FieldName = TIName.str() + ".field" + std::to_string(Idx);
  Constant *GlobalVar = M.getGlobalVariable(FieldName);
  if (!GlobalVar) {
    SmallVector<Constant *, 0> Body;
    Body.push_back(ConstantInt::get(Int32Ty, Modifier));
    Body.push_back(ConstantInt::get(Int32Ty, FieldIdx));
    Body.push_back(Annotation);
    GlobalVar = createGlobalVal(FieldName, "reflect.fieldType", Body);
  }
  return ConstantExpr::getBitCast(GlobalVar, Int8PtrTy);
}

Constant *FieldInfo::fillStaticFieldInfo() {
  std::string GlobalName = StaticName.str() + ".globalVars";
  Constant *GlobalVar = M.getGlobalVariable(GlobalName);
  if (!GlobalVar) {
    SmallVector<Constant *, 0> Body;
    Constant *NameGV = getOrInsertConstStringGV(M, FieldName);
    Constant *TypeGV = getNameGlobal(M, TypeName);
    Constant *GlobalAddr = getNameGlobal(M, StaticName);
    Body.push_back(ConstantExpr::getBitCast(NameGV, Int8PtrTy));
    Body.push_back(ConstantInt::get(Int32Ty, Modifier));
    Body.push_back(ConstantExpr::getBitCast(TypeGV, Int8PtrTy));
    Body.push_back(ConstantExpr::getBitCast(GlobalAddr, Int8PtrTy));
    Body.push_back(Annotation);
    GlobalVar = createGlobalVal(GlobalName, "reflect.staticFieldType", Body);
  }
  return ConstantExpr::getBitCast(GlobalVar, Int8PtrTy);
}

ReflectInfo::ReflectInfo(Module &M) : BaseInfo(M), DL(M.getDataLayout()) {
  // generate reflection info by metadata.
  NamedMDNode *PkgInfoMD = M.getNamedMetadata("pkg_info");
  if (PkgInfoMD == nullptr)
    return;

  getGlobalsFromNamedMD(TypeInfos, M.getNamedMetadata("types"));
  getGlobalsFromNamedMD(TypeInfos, M.getNamedMetadata("primitive_tis"));
  getGlobalsFromNamedMD(TypeTemplates, M.getNamedMetadata("type_templates"));
  getGlobalsFromNamedMD(TypeTemplates, M.getNamedMetadata("primitive_tts"));

  // global funcs in current bc.
  NamedMDNode *GlobalFuncsMD = M.getNamedMetadata("functions");
  if (GlobalFuncsMD != nullptr) {
    for (unsigned Idx = 0; Idx < GlobalFuncsMD->getNumOperands(); ++Idx) {
      auto *MethodMD = dyn_cast<MDTuple>(GlobalFuncsMD->getOperand(Idx));
      MethodInfo *Info = getMethodInfo();
      bool IsPublic = parseOneMethodMd(*Info, MethodMD, true);
      if (IsPublic) {
        StaticGlobalMethods[Info->MethodLinkName] = Info;
      }
    }
  }
  // global vars in current bc.
  NamedMDNode *GlobalVarsMD = M.getNamedMetadata("global_variables");
  if (GlobalVarsMD != nullptr) {
    for (unsigned Idx = 0; Idx < GlobalVarsMD->getNumOperands(); ++Idx) {
      auto *FieldMD = dyn_cast<MDTuple>(GlobalVarsMD->getOperand(Idx));
      FieldInfo *Info = getFieldInfo();
      parseOneStaticFieldMd(*Info, FieldMD);
      if (Info->Modifier & RMT_PUBLIC) {
        StaticGlobalFields[Info->StaticName] = Info;
      }
    }
  }
}

Constant *ReflectInfo::getReflectionInfo(GlobalVariable *GV, bool IsEnum) {
  const auto *ReflectMD =
      dyn_cast_or_null<MDTuple>(GV->getMetadata("Reflection"));
  if (ReflectMD == nullptr)
    return Int8PtrNull;

  TIName = GV->getName();
  if (enable(M)) {
    if (GV->hasAttribute(CFileKlassStr)) {
      if (!IsEnum && !TypeInfos.contains(GV)) {
        report_fatal_error(GV->getName() +
                           ": typeinfo's reflect info don't exist in package "
                           "info at the same time!");
      }
    } else if (GV->hasAttribute("cj_tt")) {
      if (!IsEnum && !TypeTemplates.contains(GV)) {
        report_fatal_error(GV->getName() +
                           ": type_template's reflect info don't exist in "
                           "package info at the same time!");
      }
    }
    if (IsEnum) {
      if (ReflectMD->getNumOperands() == ERT_MAX)
        return getEnumReflectInfo(ReflectMD);
      if (ReflectMD->getNumOperands() == ECRT_MAX)
        return getEnumCtorReflectInfo(ReflectMD);
      report_fatal_error(GV->getName() +
                         ": enum reflect info's operand number error!");
    } else {
      return getClassReflectInfo(ReflectMD);
    }
  } else {
    if (IsEnum) {
      return getEnumDebugInfo(ReflectMD);
    } else {
      return getClassDebugInfo(ReflectMD);
    }
  }
}

// Reflection: !{typeName, instanceFields, staticFields, instanceMethods,
//               staticMethods, attributes}
Constant *ReflectInfo::getClassReflectInfo(const MDTuple *ReflectMD) {
  // record all reflection info
  ClassReflectInfo Info(M);
  Info.GenericClass = getStringFromMD(ReflectMD, CRT_GENERIC_CLASS);
  parseAttributes(getMDOperand(ReflectMD, CRT_ATTRIBUTE), Info.Modifier,
                  Info.Annotation);
  parseInstanceFieldMDs(Info, getMDOperand(ReflectMD, CRT_INSTANCE_FIELD));
  parseStaticFieldMDs(Info, getMDOperand(ReflectMD, CRT_STATIC_FIELD));
  parseInstanceMethodMDs(Info.InstanceMethods,
                         getMDOperand(ReflectMD, CRT_INSTANCE_METHOD));
  parseStaticMethodMDs(Info.StaticMethods,
                       getMDOperand(ReflectMD, CRT_STATIC_METHOD));

  SmallVector<Constant *, 0> BodyVec(FRT_PTRS);
  fillClassReflectInfo(Info, BodyVec);

  std::string GVName = TIName.str() + ".reflect";
  auto *ReflectGV = createGlobalVal(GVName, GVName + "Type", BodyVec);
  return ConstantExpr::getBitCast(ReflectGV, Int8PtrTy);
}

// Reflection: !{type name, enum ctors, instance methods, static methods,
//               type attributes}
Constant *ReflectInfo::getEnumReflectInfo(const MDTuple *ReflectMD) {
  // record all reflection info
  EnumReflectInfo Info(M);
  Info.GenericClass = getStringFromMD(ReflectMD, ERT_GENERIC_CLASS);
  parseAttributes(getMDOperand(ReflectMD, ERT_ATTRIBUTE), Info.Modifier,
                  Info.Annotation);
  parseEnumCtorMDs(Info.EnumCtors, getMDOperand(ReflectMD, ERT_ENUM_CTOR));
  parseInstanceMethodMDs(Info.InstanceMethods,
                         getMDOperand(ReflectMD, ERT_INSTANCE_METHOD));
  parseStaticMethodMDs(Info.StaticMethods,
                       getMDOperand(ReflectMD, ERT_STATIC_METHOD));

  SmallVector<Constant *, 0> BodyVec(FET_PTRS);
  fillEnumReflectInfo(Info, BodyVec);

  std::string GVName = TIName.str() + ".reflect";
  auto *ReflectGV = createGlobalVal(GVName, GVName + "Type", BodyVec);
  return ConstantExpr::getBitCast(ReflectGV, Int8PtrTy);
}

// Reflection: !{type attributes}
Constant *ReflectInfo::getEnumCtorReflectInfo(const MDTuple *ReflectMD) {
  EnumCtorReflectInfo Info(M);
  parseAttributes(getMDOperand(ReflectMD, ECRT_ATTRIBUTE), Info.Modifier,
                  Info.Annotation);

  SmallVector<Constant *, 0> BodyVec(FECT_PTRS);
  fillEnumCtorReflectInfo(Info, BodyVec);

  std::string GVName = TIName.str() + ".reflect";
  auto *ReflectGV = createGlobalVal(GVName, GVName + "Type", BodyVec);
  return ConstantExpr::getBitCast(ReflectGV, Int8PtrTy);
}

// Reflection: !{typeName, instanceFields}
Constant *ReflectInfo::getClassDebugInfo(const MDTuple *ReflectMD) {
  MDTuple *FieldMDs = getMDOperand(ReflectMD, CDT_INSTANCE_FIELD);
  unsigned Cnt = FieldMDs->getNumOperands();
  if (Cnt == 0)
    return Int8PtrNull;

  SmallVector<Constant *, 0> Body;
  for (unsigned Idx = 0; Idx < Cnt; ++Idx) {
    auto *FieldMD = dyn_cast<MDTuple>(FieldMDs->getOperand(Idx).get());
    // InstanceFieldMD: !{idx, <field name>, <attributes>}
    StringRef FieldName = getStringFromMD(FieldMD, 1);
    Constant *NameGV = getOrInsertConstStringGV(M, FieldName);
    Body.push_back(ConstantExpr::getBitCast(NameGV, Int8PtrTy));
  }
  std::string GVName = TIName.str() + ".fieldNames";
  auto *FieldNamesGV = createGlobalVal(GVName, GVName + "Type", Body);

  SmallVector<Constant *, 0> BodyVec;
  BodyVec.push_back(ConstantExpr::getBitCast(FieldNamesGV, Int8PtrTy));

  std::string DebugGVName = TIName.str() + ".reflect";
  auto *ReflectGV = createGlobalVal(DebugGVName, DebugGVName + "Type", BodyVec);
  return ConstantExpr::getBitCast(ReflectGV, Int8PtrTy);
}

// Reflection: !{typeName, enum ctors, type attributes}
Constant *ReflectInfo::getEnumDebugInfo(const MDTuple *ReflectMD) {
  EnumReflectInfo Info(M);
  parseAttributes(getMDOperand(ReflectMD, EDT_ATTRIBUTE), Info.Modifier,
                  Info.Annotation);
  parseEnumCtorMDs(Info.EnumCtors, getMDOperand(ReflectMD, EDT_ENUM_CTOR));

  // 3: { ctors info, modifier, ctors info cnt}
  SmallVector<Constant *, 0> BodyVec(3);
  BodyVec[FET_CTOR_INFO] = Info.fillEnumCtorInfos(TIName);
  BodyVec[FET_MODIFIER] = ConstantInt::get(Int32Ty, Info.Modifier);
  BodyVec[FET_CTOR_CNT] = ConstantInt::get(Int32Ty, Info.EnumCtors.size());

  std::string GVName = TIName.str() + ".reflect";
  auto *ReflectGV = createGlobalVal(GVName, GVName + "Type", BodyVec);
  return ConstantExpr::getBitCast(ReflectGV, Int8PtrTy);
}

std::pair<GlobalVariable *, StructType *>
ReflectInfo::buildPackageInfoGV(std::string PackName) {
  // generate packageInfo gv and init it later.
  unsigned Nums = FPT_PTRS + TypeInfos.size() + TypeTemplates.size() +
                  StaticGlobalMethods.size() + StaticGlobalFields.size();
  SmallVector<Type *, 0> TypeVec(Nums, Int8PtrTy);
  TypeVec[FPT_TYPE_INFO_CNT] = Int32Ty;
  TypeVec[FPT_TYPE_TEMPLATE_CNT] = Int32Ty;
  TypeVec[FPT_GLOBAL_FUNC_CNT] = Int32Ty;
  TypeVec[FPT_GLOBAL_VAR_CNT] = Int32Ty;
  TypeVec[FPT_FLAG] = Int64Ty;

  auto PkgInfoType = StructType::getTypeByName(C, PackName + ".pkgInfoType");
  if (PkgInfoType == nullptr) {
    PkgInfoType = StructType::create(C, TypeVec, PackName + ".pkgInfoType");
  }
  PackName += ".packageInfo";
  auto PackageInfoGV =
      dyn_cast<GlobalVariable>(M.getOrInsertGlobal(PackName, PkgInfoType));
  PackageInfoGV->setLinkage(GlobalValue::ExternalLinkage);
  PackageInfoGV->addAttribute("CFileReflect");
  GlobalValue::SanitizerMetadata Meta;
  Meta.NoAddress = true;
  PackageInfoGV->setSanitizerMetadata(Meta);
  return {PackageInfoGV, PkgInfoType};
}

bool ReflectInfo::fillPackageInfo() {
  NamedMDNode *PkgInfoMD = M.getNamedMetadata("pkg_info");
  if (PkgInfoMD == nullptr)
    return false;

  MDTuple *PkgInfoTuple = dyn_cast<MDTuple>(PkgInfoMD->getOperand(0));
  if (PkgInfoTuple->getNumOperands() != PIT_MAX) {
    report_fatal_error("invalid package info metadata!");
  }

  StringRef Version = getStringFromMD(PkgInfoTuple, PIT_VERSION);
  StringRef ModuleName = getStringFromMD(PkgInfoTuple, PIT_MODULE_NAME);
  StringRef PkgName = getStringFromMD(PkgInfoTuple, PIT_PACKAGE_NAME);
  StringRef CurName = getStringFromMD(PkgInfoTuple, PIT_CURRENT_NAME);
  StringRef NextName = getStringFromMD(PkgInfoTuple, PIT_NEXT_NAME);

  auto [PackageInfoGV, PkgInfoType] = buildPackageInfoGV(CurName.str());

  SmallVector<Constant *, 0> PkgInfoGVInit;
  unsigned TypeNum = TypeInfos.size();
  unsigned TemplateNum = TypeTemplates.size();
  unsigned GlobalFuncNums = StaticGlobalMethods.size();
  unsigned GlobalVarNums = StaticGlobalFields.size();
  if (NextName.empty()) {
    PkgInfoGVInit.push_back(Int8PtrNull);
  } else {
    std::string NextPkgName = NextName.str() + ".packageInfo";
    auto NextPackageInfo =
        dyn_cast<GlobalVariable>(M.getOrInsertGlobal(NextPkgName, Int8PtrTy));
    NextPackageInfo->setLinkage(GlobalValue::ExternalLinkage);
    PkgInfoGVInit.push_back(
        ConstantExpr::getBitCast(NextPackageInfo, Int8PtrTy));
  }
  PkgInfoGVInit.push_back(ConstantInt::get(Int32Ty, TypeNum));
  PkgInfoGVInit.push_back(ConstantInt::get(Int32Ty, TemplateNum));
  PkgInfoGVInit.push_back(ConstantInt::get(Int32Ty, GlobalFuncNums));
  PkgInfoGVInit.push_back(ConstantInt::get(Int32Ty, GlobalVarNums));
  PkgInfoGVInit.push_back(ConstantInt::get(Int64Ty, enable(M)));

  PkgInfoGVInit.push_back(ConstantExpr::getBitCast(
      getOrInsertConstStringGV(M, ModuleName), Int8PtrTy));

  PkgInfoGVInit.push_back(ConstantExpr::getBitCast(
      getOrInsertConstStringGV(M, PkgName), Int8PtrTy));

  PkgInfoGVInit.push_back(ConstantExpr::getBitCast(
      getOrInsertConstStringGV(M, Version), Int8PtrTy));

  PkgInfoGVInit.push_back(Int8PtrNull);
  PkgInfoGVInit.push_back(Int8PtrNull);

  for (unsigned Idx = 0; Idx < TypeNum; ++Idx) {
    PkgInfoGVInit.push_back(
        ConstantExpr::getBitCast(TypeInfos[Idx], Int8PtrTy));
  }
  for (unsigned Idx = 0; Idx < TemplateNum; ++Idx) {
    PkgInfoGVInit.push_back(
        ConstantExpr::getBitCast(TypeTemplates[Idx], Int8PtrTy));
  }
  // add global funcs
  for (auto Itr = StaticGlobalMethods.begin(), End = StaticGlobalMethods.end();
       Itr != End; ++Itr) {
    PkgInfoGVInit.push_back(Itr->second->fillMethodInfo(0, ""));
  }
  // add global vars
  for (auto Itr = StaticGlobalFields.begin(), End = StaticGlobalFields.end();
       Itr != End; ++Itr) {
    PkgInfoGVInit.push_back(Itr->second->fillStaticFieldInfo());
  }
  PackageInfoGV->setInitializer(
      ConstantStruct::get(PkgInfoType, PkgInfoGVInit));

  M.eraseNamedMetadata(PkgInfoMD);
  return true;
}

bool ReflectInfo::enable(Module &M) {
  NamedMDNode *PkgInfoMD = M.getNamedMetadata("pkg_info");
  if (PkgInfoMD == nullptr)
    return false;

  MDTuple *PkgInfoTuple = dyn_cast<MDTuple>(PkgInfoMD->getOperand(0));
  if (PkgInfoTuple->getNumOperands() != PIT_MAX) {
    report_fatal_error("invalid package info metadata!");
  }
  auto MDConstant =
      mdconst::extract<ConstantInt>(PkgInfoTuple->getOperand(PIT_FLAG).get());
  if (MDConstant->getZExtValue() > 0)
    return true;

  return false;
}

void ReflectInfo::fillClassReflectInfo(ClassReflectInfo &Info,
                                       SmallVector<Constant *, 0> &BodyVec) {
  BodyVec[FRT_FIELD_NAME] = Info.fillInstanceFieldNames(TIName);
  BodyVec[FRT_MODIFIER] = ConstantInt::get(Int32Ty, Info.Modifier);
  BodyVec[FRT_INSTANCE_FIELD] =
      ConstantInt::get(Int16Ty, Info.InstanceFields.size());
  BodyVec[FRT_STATIC_FIELD] =
      ConstantInt::get(Int16Ty, Info.StaticFields.size());
  BodyVec[FRT_INSTANCE_METHOD] =
      ConstantInt::get(Int32Ty, Info.InstanceMethods.size());
  BodyVec[FRT_STATIC_METHOD] =
      ConstantInt::get(Int32Ty, Info.StaticMethods.size());
  BodyVec[FRT_ANNOTATION] = Info.Annotation;
  if (Info.GenericClass.empty()) {
    BodyVec[FRT_GENERIC_CLASS] = Int8PtrNull;
  } else {
    BodyVec[FRT_GENERIC_CLASS] = getNameGlobal(M, Info.GenericClass);
  }

  for (unsigned Idx = 0; Idx < Info.InstanceFields.size(); ++Idx) {
    BodyVec.push_back(
        Info.InstanceFields[Idx]->fillInstanceFieldInfo(Idx, TIName));
  }
  for (unsigned Idx = 0; Idx < Info.StaticFields.size(); ++Idx) {
    BodyVec.push_back(Info.StaticFields[Idx]->fillStaticFieldInfo());
  }
  for (unsigned Idx = 0; Idx < Info.InstanceMethods.size(); ++Idx) {
    BodyVec.push_back(Info.InstanceMethods[Idx]->fillMethodInfo(Idx, TIName));
  }
  for (unsigned Idx = 0; Idx < Info.StaticMethods.size(); ++Idx) {
    BodyVec.push_back(Info.StaticMethods[Idx]->fillMethodInfo(Idx, TIName));
  }
}

void ReflectInfo::fillEnumReflectInfo(EnumReflectInfo &Info,
                                      SmallVector<Constant *, 0> &BodyVec) {
  BodyVec[FET_CTOR_INFO] = Info.fillEnumCtorInfos(TIName);
  BodyVec[FET_MODIFIER] = ConstantInt::get(Int32Ty, Info.Modifier);
  BodyVec[FET_CTOR_CNT] = ConstantInt::get(Int32Ty, Info.EnumCtors.size());
  BodyVec[FET_INSTANCE_METHOD] =
      ConstantInt::get(Int32Ty, Info.InstanceMethods.size());
  BodyVec[FET_STATIC_METHOD] =
      ConstantInt::get(Int32Ty, Info.StaticMethods.size());
  BodyVec[FET_ANNOTATION] = Info.Annotation;
  if (Info.GenericClass.empty()) {
    BodyVec[FET_GENERIC_CLASS] = Int8PtrNull;
  } else {
    BodyVec[FET_GENERIC_CLASS] = getNameGlobal(M, Info.GenericClass);
  }

  for (unsigned Idx = 0; Idx < Info.InstanceMethods.size(); ++Idx) {
    BodyVec.push_back(Info.InstanceMethods[Idx]->fillMethodInfo(Idx, TIName));
  }
  for (unsigned Idx = 0; Idx < Info.StaticMethods.size(); ++Idx) {
    BodyVec.push_back(Info.StaticMethods[Idx]->fillMethodInfo(Idx, TIName));
  }
}

void ReflectInfo::fillEnumCtorReflectInfo(EnumCtorReflectInfo &Info,
                                          SmallVector<Constant *, 0> &BodyVec) {
  BodyVec[FECT_ANNOTATION] = Info.Annotation;
  BodyVec[FECT_MODIFIER] = ConstantInt::get(Int32Ty, Info.Modifier);
  BodyVec[FECT_CTOR_CNT] = ConstantInt::get(Int32Ty, 0);
}

void ReflectInfo::parseAttributes(MDTuple *Attr, unsigned &Modifier,
                                  Constant *&Annotation) {
  unsigned Idx = 0;
  unsigned IdxEnd = Attr->getNumOperands();
  // last element is annotation info
  while (Idx < IdxEnd - 1) {
    Modifier |= getTypeModifier(getStringFromMD(Attr, Idx++));
  }
  // 1: set to default if no modifier in attributes
  if (Modifier == 0) {
    Modifier = 1;
  }
  StringRef AnnoFuncName = getStringFromMD(Attr, Idx);
  if (AnnoFuncName.equals("none")) {
    Annotation = Int8PtrNull;
    return;
  }
  Function *Func = M.getFunction(AnnoFuncName);
  if (Func == nullptr) {
    report_fatal_error(AnnoFuncName + ": can't find this anno func!");
  }
  Annotation = ConstantExpr::getBitCast(Func, Int8PtrTy);
}

// !InstanceFieldMDs = !{!{name1 attributes}, !{name2 attributes},...}
void ReflectInfo::parseInstanceFieldMDs(ClassReflectInfo &Info,
                                        const MDTuple *FieldMDs) {
  for (unsigned Idx = 0; Idx < FieldMDs->getNumOperands(); ++Idx) {
    auto *FieldMD = dyn_cast<MDTuple>(FieldMDs->getOperand(Idx).get());
    FieldInfo *TempInfo = getFieldInfo();
    parseOneInstanceFieldMd(*TempInfo, FieldMD);
    Info.FieldNames.emplace_back(TempInfo->FieldName);
    if (TempInfo->Modifier & RMT_PUBLIC) {
      Info.InstanceFields.emplace_back(TempInfo);
    }
  }
}

// !StaticFieldMDs = !{
//   !{field1 name, field1 type name, field1 link name, field1 attributes},
//   !{field2 name, field2 type name, field2 link name, field2 attributes},
//   ...
// }
void ReflectInfo::parseStaticFieldMDs(ClassReflectInfo &Info,
                                      const MDTuple *FieldMDs) {
  for (unsigned Idx = 0; Idx < FieldMDs->getNumOperands(); ++Idx) {
    auto *FieldMD = dyn_cast<MDTuple>(FieldMDs->getOperand(Idx).get());
    FieldInfo *TempInfo = getFieldInfo();
    parseOneStaticFieldMd(*TempInfo, FieldMD);
    if (TempInfo->Modifier & RMT_PUBLIC) {
      auto Itr = StaticGlobalFields.find(TempInfo->StaticName);
      if (Itr != StaticGlobalFields.end()) {
        report_fatal_error(TempInfo->StaticName +
                           ": static field's reflect info cann't exist in "
                           "global var at the same time!");
      }
      Info.StaticFields.emplace_back(TempInfo);
    }
  }
  return;
}

// !{paramName, typeName, !attributes}
void ReflectInfo::parseParamMd(SmallVectorImpl<ParamInfo> &ParamInfos,
                               MDTuple *ParamMD) {
  uint32_t TempModifiers = 0;
  for (unsigned Idx = 0, End = ParamMD->getNumOperands(); Idx < End; ++Idx) {
    ParamInfo Param;
    auto *ParamTuple = dyn_cast<MDTuple>(ParamMD->getOperand(Idx));
    Param.TypeName = getStringFromMD(ParamTuple, 1);
    Param.ParamName = getStringFromMD(ParamTuple, 0);
    // 2: attributes
    MDTuple *AttributeMD = getMDOperand(ParamTuple, 2);
    parseAttributes(AttributeMD, TempModifiers, Param.Annotation);
    ParamInfos.emplace_back(Param);
  }
}

// !{typeName}
void ReflectInfo::parseGenericParamMd(SmallVectorImpl<StringRef> &ParamInfos,
                                      MDTuple *ParamMD) {
  for (unsigned Idx = 0, End = ParamMD->getNumOperands(); Idx < End; ++Idx) {
    ParamInfos.emplace_back(getStringFromMD(ParamMD, Idx));
  }
}

// !{name, returnType, linkName, attributes, actualParam, genericParam}
bool ReflectInfo::parseOneMethodMd(MethodInfo &Info, MDTuple *MethodMD,
                                   bool IsStatic) {
  uint32_t Modifier = 0;
  // 3: attribute
  parseAttributes(getMDOperand(MethodMD, MRT_ATTRIBUTE), Modifier,
                  Info.Annotation);
  bool IsPublic = Modifier & RMT_PUBLIC;
  if (!IsPublic) {
    return false;
  }
  Info.IsStatic = IsStatic;
  Info.Modifier = Modifier;
  Info.MethodLinkName = getStringFromMD(MethodMD, MRT_LINKAGE_NAME);
  Info.MethodName = getStringFromMD(MethodMD, MRT_NAME);
  Info.ReturnType = getStringFromMD(MethodMD, MRT_RETURN_TYPE);
  MDTuple *ActualParamMD = getMDOperand(MethodMD, MRT_ACTUAL_PARAM);
  parseParamMd(Info.ActualParams, ActualParamMD);
  MDTuple *GenericParamMD = getMDOperand(MethodMD, MRT_GENERIC_PARAM);
  parseGenericParamMd(Info.GenericParams, GenericParamMD);
  return true;
}

void ReflectInfo::parseInstanceMethodMDs(SmallVectorImpl<MethodInfo *> &Infos,
                                         MDTuple *MethodMDs) {
  for (unsigned Idx = 0; Idx < MethodMDs->getNumOperands(); ++Idx) {
    auto *MethodMD = dyn_cast<MDTuple>(MethodMDs->getOperand(Idx).get());
    MethodInfo *TempInfo = getMethodInfo();
    bool IsPublic = parseOneMethodMd(*TempInfo, MethodMD, false);
    if (IsPublic) {
      Infos.emplace_back(TempInfo);
    }
  }
}

void ReflectInfo::parseStaticMethodMDs(SmallVectorImpl<MethodInfo *> &Infos,
                                       MDTuple *MethodMDs) {
  for (unsigned Idx = 0; Idx < MethodMDs->getNumOperands(); ++Idx) {
    auto *MethodMD = dyn_cast<MDTuple>(MethodMDs->getOperand(Idx).get());
    MethodInfo *TempInfo = getMethodInfo();
    bool IsPublic = parseOneMethodMd(*TempInfo, MethodMD, true);
    if (IsPublic) {
      auto Itr = StaticGlobalMethods.find(TempInfo->MethodLinkName);
      if (Itr != StaticGlobalMethods.end()) {
        report_fatal_error(TempInfo->MethodLinkName +
                           ": static method's reflect info cann't exist in "
                           "global funcs at the same time!");
      }
      Infos.emplace_back(TempInfo);
    }
  }
}

void ReflectInfo::parseEnumCtorMDs(SmallVectorImpl<EnumCtorInfo> &Info,
                                   MDTuple *CtorMDs) {
  for (unsigned Idx = 0; Idx < CtorMDs->getNumOperands(); ++Idx) {
    auto *MethodMD = dyn_cast<MDTuple>(CtorMDs->getOperand(Idx).get());
    EnumCtorInfo CtorInfo;
    CtorInfo.CtorName = getStringFromMD(MethodMD, 0);
    CtorInfo.TypeName = getStringFromMD(MethodMD, 1);
    Info.emplace_back(CtorInfo);
  }
}

// InstanceFieldMD: !{idx, <field name>, <attributes>}
void ReflectInfo::parseOneInstanceFieldMd(FieldInfo &Info, MDTuple *FieldMD) {
  uint32_t Modifier = 0;
  parseAttributes(getMDOperand(FieldMD, 2), Modifier, Info.Annotation);
  Info.Modifier = Modifier;
  Info.IsStatic = false;
  auto MDConstant =
      mdconst::extract_or_null<ConstantInt>(FieldMD->getOperand(0).get());
  Info.FieldIdx = MDConstant->getZExtValue();
  Info.FieldName = getStringFromMD(FieldMD, 1);
}

// StaticFieldMD: !{<field name>, <type name>, <linkage name>, <attributes>}
void ReflectInfo::parseOneStaticFieldMd(FieldInfo &Info, MDTuple *FieldMD) {
  uint32_t Modifier = 0;
  // 3: attributes
  parseAttributes(getMDOperand(FieldMD, 3), Modifier, Info.Annotation);
  Info.Modifier = Modifier;
  Info.IsStatic = true;
  // 3: field's linkname
  Info.StaticName = getStringFromMD(FieldMD, 2);
  Info.TypeName = getStringFromMD(FieldMD, 1);
  Info.FieldName = getStringFromMD(FieldMD, 0);
}

void ReflectInfo::getGlobalsFromNamedMD(SetVector<GlobalVariable *> &GVs,
                                        NamedMDNode *MDNode) {
  if (MDNode == nullptr)
    return;

  for (unsigned Idx = 0; Idx < MDNode->getNumOperands(); ++Idx) {
    auto *MD = dyn_cast<MDTuple>(MDNode->getOperand(Idx));
    GVs.insert(getNameGlobal(M, getStringFromMD(MD, 0)));
  }
}

class TypeInfoMaker {
public:
  TypeInfoMaker(Module &M, CJStructTypeGCInfo &GCInfo, ReflectInfo &Reflector)
      : M(M), C(M.getContext()), DL(M.getDataLayout()), GCInfo(GCInfo),
        Reflector(Reflector) {}

  void fillTypeInfo(GlobalVariable *GV) {
    buildTypeInfoBody(GV);

    // no need to append 16-bytes asan guard for TI.
    GlobalValue::SanitizerMetadata Meta;
    Meta.NoAddress = true;
    GV->setSanitizerMetadata(Meta);
  }

  void fillTemplate(GlobalVariable *GV) {
    TypeTemplate TT(GV);
    uint8_t Flag = TT.getFlag();
    if (GV->hasAttribute("Future")) {
      assert(TT.isSyncClass() && "Not a Future TypeTemplate!");
      Flag |= TF_FUTURE_CLASS;
    }
    if (GV->hasAttribute("HasExtPart")) {
      Flag |= TF_HAS_EXT_PART;
    }
    if (Reflector.enable(M)) {
      Flag |= TF_REFLECTION;
    }

    SmallVector<Constant *, TT_MAX> BodyVec(TT_MAX);
    // Initialize all fields.
    for (unsigned Idx = 0; Idx < TT_MAX; ++Idx) {
      if (Idx == TT_FLAG) {
        BodyVec[Idx] = ConstantInt::get(Type::getInt8Ty(C), Flag);
      } else if (Idx == TT_REFLECTION) {
        BodyVec[Idx] = Reflector.getReflectionInfo(GV, TT.isEnum());
      } else {
        BodyVec[Idx] = TT.get(Idx);
      }
    }
    // rewrite GV data.
    GV->setInitializer(ConstantStruct::get(TT.TT->getType(), BodyVec));
  }

  void buildTypeInfoBody(GlobalVariable *GV) {
    TypeInfo TI(GV);
    SmallVector<Constant *, CIT_MAX> BodyVec(CIT_MAX);
    // Initialize all fields.
    initializeTypeInfoElements(TI, BodyVec);

    if (StructType *ST = TI.getLayoutType()) {
      // Generating the gctib of the struct type.
      auto &Info = GCInfo.getOrInsertTypeGCInfo(ST, TI.isArray());

      if (TI.isSyncClass()) {
        Info.ObjSize = SyncSize - 8; // 8: typeinfo* size
      }

      BodyVec[CIT_FLAG] =
          ConstantInt::get(Type::getInt8Ty(C), getFlag(GV, Info.BMInfo.HasRef));
      if (!TI.isArray()) {
        BodyVec[CIT_SIZE] = ConstantInt::get(Type::getInt32Ty(C), Info.ObjSize);
        Type *BMType = TI.getGCTib()->getType();
        std::string NamePrefix = GV->getName().str();
        BodyVec[CIT_GCTIB] =
            GCInfo.getOrInsertBitMap(Info.BMInfo.BMStr, BMType, NamePrefix);
      }
    }

    if (!TI.isGenericType()) {
      BodyVec[CIT_REFLECTION] = Reflector.getReflectionInfo(GV, TI.isEnum());
    }
    // rewrite GV data.
    GV->setInitializer(ConstantStruct::get(TI.Data->getType(), BodyVec));
  }

  bool needFillTypeInfo(GlobalVariable *GV) {
    if (!GV->hasAttribute(CFileKlassStr))
      return false;

    TypeInfo TI(GV);
    if (TI.isPureValueType())
      return false;

    return true;
  }

private:
  Module &M;
  LLVMContext &C;
  const DataLayout &DL;
  CJStructTypeGCInfo &GCInfo;
  ReflectInfo &Reflector;

  unsigned getFlag(GlobalVariable *TI, bool HasRef) {
    assert(TI != nullptr && "Is not valid TypeInfo");
    unsigned Flag = 0;
    if (HasRef)
      Flag |= TF_HAS_REF_FIELD;

    if (TI->hasAttribute("HasFinalizer"))
      Flag |= TF_HAS_FINALIZER;

    if (TI->hasAttribute("Future"))
      Flag |= TF_FUTURE_CLASS;

    if (TI->hasAttribute("Mutex"))
      Flag |= TF_MUTEX_CLASS;

    if (TI->hasAttribute("Monitor"))
      Flag |= TF_MONITOR_CLASS;

    if (TI->hasAttribute("WaitQueue"))
      Flag |= TF_WAIT_QUEUE_CLASS;

    if (Reflector.enable(M))
      Flag |= TF_REFLECTION;

    if (TI->hasAttribute("HasExtPart"))
      Flag |= TF_HAS_EXT_PART;

    return Flag;
  }

  void initializeTypeInfoElements(TypeInfo &TypeInfo,
                               SmallVectorImpl<Constant *> &BodyVec) {
    for (int i = 0; i < CIT_MAX; ++i) {
      BodyVec[i] = TypeInfo.get(i);
    }
  }
};

static Value *analysisGVType(Value *V) {
  auto *CI = dyn_cast<CallBase>(V);
  if (!CI)
    return nullptr;

  unsigned ID = CI->getIntrinsicID();
  if (ID == Intrinsic::cj_malloc_object) {
    if (auto CE = dyn_cast<ConstantExpr>(CI->getOperand(0)))
      return CE->getOperand(0);
  }
  return nullptr;
}

static bool setDefiniteTypeGV(GlobalVariable *GV) {
  // first support only gcptr.
  auto *I8PtrTy = Type::getInt8Ty(GV->getContext())->getPointerTo(1);
  auto *I8PtrPtrTy = I8PtrTy->getPointerTo();
  if (GV->getType() != I8PtrPtrTy || !GV->hasInternalLinkage()) {
    return false;
  }
  SmallSet<Value *, 8> TypeVec;
  for (auto I = GV->user_begin(), E = GV->user_end(); I != E;) {
    User *U = *(I++);
    auto *CB = dyn_cast<CallBase>(U);
    if (!CB)
      return false;

    unsigned ID = CB->getIntrinsicID();
    if (ID == Intrinsic::cj_gcwrite_static_ref) {
      TypeVec.insert(analysisGVType(CB->getOperand(0)));
      continue;
    } else if (ID == Intrinsic::cj_gcread_static_ref) {
      continue;
    } else {
      return false;
    }
  }
  // If all uses of global value i8 AS (1) ** have only barrier read and write,
  // and are unique TypeInfo, mark global value metadata information.
  if (TypeVec.size() == 1 && *TypeVec.begin() != nullptr) {
    MDString *SS = MDString::get(
        GV->getContext(), dyn_cast<GlobalValue>(*TypeVec.begin())->getName());
    GV->setMetadata("cjType", MDNode::get(GV->getContext(), {SS}));
    return true;
  }
  return false;
}

static bool markTypeInfoTypeForGCRoots(Module &M) {
  Module::GlobalListType &GVlist = M.getGlobalList();
  bool Changed = false;
  for (auto I = GVlist.begin(), E = GVlist.end(); I != E; ++I) {
    if (I->hasMetadata("GlobalVarType")) {
      if (setDefiniteTypeGV(cast<GlobalVariable>(I))) {
        Changed = true;
      }
    }
  }
  return Changed;
}

static bool fillGV(Module &M) {
  CJStructTypeGCInfo GCInfo(M);
  ReflectInfo Reflector(M);
  TypeInfoMaker TIMaker(M, GCInfo, Reflector);

  bool Changed = markTypeInfoTypeForGCRoots(M);
  Module::GlobalListType &GVlist = M.getGlobalList();
  for (auto I = GVlist.begin(), E = GVlist.end(); I != E; ++I) {
    if (!I->hasInitializer())
      continue;

    GlobalVariable *GV = dyn_cast<GlobalVariable>(I);
    if (TIMaker.needFillTypeInfo(GV)) {
      Changed = true;
      TIMaker.fillTypeInfo(GV);
      continue;
    }
    if (GV->hasAttribute("cj_tt")) {
      TIMaker.fillTemplate(GV);
    }
  }
  Changed |= Reflector.fillPackageInfo();
  return Changed;
}

PreservedAnalyses CJFillMetadata::run(Module &M, ModuleAnalysisManager &) const {
  if (hasRunCangjieOpt(M)) {
    return PreservedAnalyses::all();
  }
  if (fillGV(M)) {
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

namespace {
class CJFillMetadataLegacyPass : public ModulePass {
public:
  static char ID;
  CJFillMetadataLegacyPass() : ModulePass(ID) {
    initializeCJFillMetadataLegacyPassPass(*PassRegistry::getPassRegistry());
  }
  ~CJFillMetadataLegacyPass() = default;

  bool runOnModule(Module &M) override { return fillGV(M); };
};
} // namespace

char CJFillMetadataLegacyPass::ID = 0;

ModulePass *llvm::createCJFillMetadataLegacyPass() {
  return new CJFillMetadataLegacyPass();
}

INITIALIZE_PASS(CJFillMetadataLegacyPass, "cj-fill-metadata", "Fill Cangjie Metadata", false,
                false)
