//===- CJFillMetadata.h - ---------------------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file provides interface to "Fill Cangjie Metadata" pass.
//
// This pass Fill Cangjie Metadata for global varible such as TypeInfo and
// ReflectionInfo.
//===----------------------------------------------------------------------===//
#ifndef LLVM_TRANSFORMS_SCALAR_FILL_CJ_METADATA_H
#define LLVM_TRANSFORMS_SCALAR_FILL_CJ_METADATA_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
enum ClassInfoFieldType : uint8_t {
  CIT_NAME = 0,
  CIT_TYPE,
  CIT_FLAG,
  CIT_FIELD_NUM,
  CIT_SIZE,
  CIT_GCTIB = 5,
  CIT_UUID,
  CIT_ALIGN,
  CIT_TYPE_ARG_NUM,      // generic type argumnet number
  CIT_VALID_INHERIT_NUM, // valid inherit index
  CIT_OFFSETS = 10,      // offset of fields
  CIT_GENERIC_FROM,      // generic type template
  CIT_TYPE_ARG,          // generic type arg[]
  CIT_FIELD,             // fields[]
  CIT_SUPER,             // super klass or component klass
  CIT_VTABLE = 15,       // virtual extension def ref in extension_defs[]
  CIT_ITABLE,            // interface hashtable
  CIT_REFLECTION,        // reflection*
  CIT_MAX,               // klass type length
};

enum TypeKind : int8_t {
  // reference type
  TK_CLASS = -128,
  TK_INTERFACE = -127,
  TK_RAWARRAY = -126,
  TK_FUNC = -125,
  TK_TEMP_ENUM = -124,
  TK_GENERIC = -1,        // TI: { name, kind }
  TK_GENERIC_CUSTOM = -2, // TI: { name, kind, typeArgCnt, typeArgs, TT}

  // value type
  TK_NOTHING = 0,
  TK_UNIT,
  TK_BOOL,
  TK_RUNE,
  TK_UINT8,
  TK_UINT16 = 5,
  TK_UINT32,
  TK_UINT64,
  TK_UINT_NATIVE,
  TK_INT8,
  TK_INT16 = 10,
  TK_INT32,
  TK_INT64,
  TK_INT_NATIVE,
  TK_FLOAT16,
  TK_FLOAT32 = 15,
  TK_FLOAT64,
  TK_CSTRING,
  TK_CPOINTER,
  TK_CFUNC,
  TK_VARRAY = 20,
  TK_TUPLE,
  TK_STRUCT,
  TK_ENUM,
  TK_MAX,
};

enum TypeTemplateFieldType : uint8_t {
  TT_NAME = 0,
  TT_TYPE,
  TT_FLAG,
  TT_FIELD_NUM,
  TT_TYPE_ARG_NUM,      // generic type argumnet number
  TT_FIELDS_FNS = 5,    // function array for fields compute
  TT_SUPER_FN,          // function for super compute
  TT_FINALIZER,         // generic type finalizer
  TT_REFLECTION,        // reflection*
  TT_VEDREF,            // virtual extension def ref in extension_defs[]
  TT_VALID_INHERIT_NUM, // valid inherit index
  TT_MAX,               // typeTemplate length
};

enum TypeFlag : uint8_t {
  TF_NONE = 0,
  TF_HAS_REF_FIELD = 0x1 << 0,
  TF_HAS_FINALIZER = 0x1 << 1,
  TF_FUTURE_CLASS = 0x1 << 2,
  TF_MUTEX_CLASS = 0x1 << 3,
  TF_MONITOR_CLASS = 0x1 << 4,
  TF_WAIT_QUEUE_CLASS = 0x1 << 5,
  TF_REFLECTION = 0x1 << 6,
  TF_HAS_EXT_PART = 0x1 << 7,
};

enum ExtensionDefFieldType : uint8_t {
  ET_TYPE_PARAM_COUNT = 0,
  ET_IS_INTERFACE_TYPEINFO,
  ET_FLAG,
  ET_FUNC_TABLE_SIZE,
  ET_TARGET_TYPE,
  ET_INTERFACE_FN,
  ET_WHERE_COND_FN,
  ET_FUNC_TABLE,
};

enum ReflectModifyType : uint32_t {
  RMT_DEFAULT = 0x1 << 0,
  RMT_PRIVATE = 0x1 << 1,
  RMT_PROTECTED = 0x1 << 2,
  RMT_PUBLIC = 0x1 << 3,
  RMT_IMMUTABLE = 0x1 << 4,
  RMT_BOXCLASS = 0x1 << 5,
  RMT_OPEN = 0x1 << 6,
  RMT_OVERRIDE = 0x1 << 7,
  RMT_REDEF = 0x1 << 8,
  RMT_ABSTRACT = 0x1 << 9,
  RMT_SEALED = 0x1 << 10,
  RMT_MUT = 0x1 << 11,
  RMT_STATIC = 0x1 << 12,
  RMT_ENUM_KIND0 = 0x1 << 13,
  RMT_ENUM_KIND1 = 0x1 << 14,
  RMT_ENUM_KIND2 = 0x1 << 15,
  RMT_ENUM_KIND3 = 0x1 << 16,
  RMT_HAS_SRET0 = 0x1 << 17, // has sret but it is'not generic
  RMT_HAS_SRET1 = 0x1 << 18, // has sret and it is 'T'
  RMT_HAS_SRET2 = 0x1 << 19, // has sret and it is 'known struct T'
  RMT_HAS_SRET3 = 0x1 << 20, // has sret and it is 'unknow struct T'
  RMT_ENUM_CTOR = 0x1 << 21,
  RMT_UNKNOWN_SIZE = 0x1 << 22,
  RMT_REFLECT_VER_BIT1 = 0x1 << 28,
  RMT_REFLECT_VER_BIT2 = 0x1 << 29,
  RMT_REFLECT_VER_BIT3 = 0x1 << 30,
  RMT_MAX = 0x1U << 31,
};

struct ExtensionDefData {
  unsigned TypeParamCount;
  uint8_t IsInterfaceTypeInfo;
  uint8_t Flag;
  uint16_t FuncTableSize;
  GlobalVariable *TargetType = 0;
  union {
    Function *InterfaceFn = nullptr;
    GlobalVariable *InterfaceTypeInfo;
  };
  Function *WhereCondFn = nullptr;
  GlobalVariable *FuncTable = nullptr;

  explicit ExtensionDefData(GlobalVariable *GV);
  bool isTypeInfo() { return TypeParamCount == 0; }
  Function *getFuncByIndex(unsigned Idx);
};

struct TypeTemplate {
  ConstantStruct *TT;
  GlobalVariable *GV;
  TypeKind Kind;

  explicit TypeTemplate(GlobalVariable *GV);

  bool isEnum();
  bool isSyncClass();

  Constant *get(unsigned Idx);
  TypeKind getKind();
  uint8_t getFlag();
  unsigned getFieldNum();
  unsigned getTypeArgNum();
};

struct TypeInfo {
  ConstantStruct *Data;
  GlobalVariable *GV;
  TypeKind Kind;

  explicit TypeInfo(GlobalVariable *GV);

  bool isStructType();
  bool isPrimitiveType();
  bool isReferenceType();
  bool isArray();
  bool isEnum();
  bool isTuple();
  bool isFunction();
  bool isVarray();
  bool isClass();
  bool isInterface();
  bool isCType();
  bool isPureValueType();
  bool isSyncClass();
  bool isGenericType();

  Constant *get(unsigned Idx);
  uint8_t getTypeFlag();
  unsigned getSize();
  unsigned getFieldNum();
  unsigned getTypeArgNum();
  Constant *getGCTib();
  unsigned getAlign();
  Constant *getOffsets();
  Constant *getTypeArgs();
  Constant *getFields();
  // If it is object-type, get the super class, if array-type,
  // get the component class.
  GlobalVariable *getSuperOrComponent();
  unsigned getOffset(unsigned Idx);
  GlobalVariable *getField(unsigned Idx);
  GlobalVariable *getTypeArg(unsigned Idx);
  GlobalVariable *getTypeTemplate();
  StructType *getLayoutType();
  Type *getArrayElementType();
  bool hasRefField();
  bool hasTypeTempate();

  // update typeinfo global variable.
  void resetTypeInfo(GlobalVariable *GV);
  static Value *getReferenceType(GlobalVariable *GV);
};

class RefValue {
public:
  explicit RefValue(Value *V) : Ref(V) {}
  ~RefValue() = default;

  GlobalVariable *findTypeInfoGV();

private:
  Value *Ref;
  SmallSet<Value *, 8> VisitedValue;
  DenseMap<Value *, unsigned> TypeVec;

  void getTypeInfoGV(Value *V);
  void getTypeInfoGVLoad(Value *V);
  void getTypeInfoGVParam(Function *F, int ArgNo);
  bool isNewObjectCallSite(CallBase *CI);
};

GlobalVariable *findTypeInfoGV(Value *V);

class Module;

struct CJFillMetadata : public PassInfoMixin<CJFillMetadata> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) const;
};

constexpr uint32_t ArrayHeadSize = 16;
constexpr uint32_t ObjectHeadSize = 8;
constexpr uint32_t SyncSize = 168;
constexpr uint32_t MaxAllocaObj = 10000;
constexpr uint32_t MaxArrayFast = 4 * 1024; // 4kb

StructType *getTypeLayoutType(GlobalVariable *GV);
std::string getPrimitiveTypeName(Type *Ty);
bool isPrimitiveTypeInfo(GlobalVariable *GV);
unsigned getValueTypeSize(GlobalVariable *GV);
std::pair<bool, TypeKind> getKnownTypeKind(GlobalVariable *GV);
unsigned getTypeModifier(StringRef Name);
bool hasRunCangjieOpt(Module &M);
} // namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_FILL_CJ_METADATA_H
