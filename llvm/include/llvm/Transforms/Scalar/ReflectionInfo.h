//===- ReflectionInfo.h - ---------------------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file provides interface to "Fill Klass Global value" pass.
//
// This file contains reflection info interfaces.
//
//===----------------------------------------------------------------------===//
#include "llvm/ADT/SetVector.h"
#include <map>

namespace llvm {
enum PackageInfoType : uint8_t {
  PIT_FLAG = 0,
  PIT_VERSION,
  PIT_MODULE_NAME,
  PIT_PACKAGE_NAME,
  PIT_CURRENT_NAME,
  PIT_NEXT_NAME = 5,
  PIT_MAX,
};

enum ClassReflectionType : uint8_t {
  CRT_TYPE_NAME = 0,
  CRT_GENERIC_CLASS,
  CRT_INSTANCE_FIELD,
  CRT_STATIC_FIELD,
  CRT_INSTANCE_METHOD,
  CRT_STATIC_METHOD = 5,
  CRT_ATTRIBUTE,
  CRT_MAX,
};

enum ClassDebugInfoType : uint8_t {
  CDT_TYPE_NAME = 0,
  CDT_INSTANCE_FIELD,
  CDT_MAX,
};

enum EnumReflectionType : uint8_t {
  ERT_TYPE_NAME = 0,
  ERT_ENUM_CTOR,
  ERT_GENERIC_CLASS,
  ERT_INSTANCE_METHOD,
  ERT_STATIC_METHOD,
  ERT_CTOR_ANNOTATIONS,
  ERT_ATTRIBUTE,
  ERT_MAX,
};

enum EnumCtorReflectionType : uint8_t {
  ECRT_ATTRIBUTE = 0,
  ECRT_MAX,
};

enum EnumDebugInfoType : uint8_t {
  EDT_TYPE_NAME = 0,
  EDT_ENUM_CTOR,
  EDT_ATTRIBUTE,
  EDT_MAX,
};

enum MethodReflectionType : uint8_t {
  MRT_NAME = 0,
  MRT_RETURN_TYPE,
  MRT_LINKAGE_NAME,
  MRT_ACTUAL_PARAM,
  MRT_GENERIC_PARAM,
  MRT_ATTRIBUTE = 5,
  MRT_MAX,
};

enum FillPackageInfoType : uint8_t {
  FPT_NEXT_PACKAGE = 0,
  FPT_TYPE_INFO_CNT,
  FPT_TYPE_TEMPLATE_CNT,
  FPT_GLOBAL_FUNC_CNT,
  FPT_GLOBAL_VAR_CNT,
  FPT_FLAG = 5,
  FPT_MODULE_NAME,
  FPT_PACKAGE_NAME,
  FPT_VERSION,
  FPT_PADDING1,
  FPT_PADDING2 = 10,
  FPT_PTRS,
};

enum FillReflectInfoType : uint8_t {
  FRT_FIELD_NAME = 0,
  FRT_MODIFIER,
  FRT_INSTANCE_FIELD,
  FRT_STATIC_FIELD,
  FRT_INSTANCE_METHOD,
  FRT_STATIC_METHOD = 5,
  FRT_ANNOTATION,
  FRT_GENERIC_CLASS,
  FRT_PTRS,
};

enum FillEnumReflectInfoType : uint8_t {
  FET_CTOR_INFO = 0,
  FET_MODIFIER,
  FET_CTOR_CNT,
  FET_INSTANCE_METHOD,
  FET_STATIC_METHOD,
  FET_ANNOTATION = 5,
  FET_GENERIC_CLASS,
  FET_PTRS,
};

enum FillEnumCtorReflectInfoType : uint8_t {
  FECT_ANNOTATION = 0,
  FECT_MODIFIER,
  FECT_CTOR_CNT,
  FECT_PTRS,
};

struct BaseInfo {
  Module &M;
  LLVMContext &C;
  Type *Int16Ty = nullptr;
  Type *Int32Ty = nullptr;
  Type *Int64Ty = nullptr;
  PointerType *Int8PtrTy = nullptr;
  PointerType *Int32PtrTy = nullptr;
  Constant *Int8PtrNull = nullptr;

  explicit BaseInfo(Module &M) : M(M), C(M.getContext()) {
    Int16Ty = Type::getInt16Ty(C);
    Int32Ty = Type::getInt32Ty(C);
    Int64Ty = Type::getInt64Ty(C);
    Int8PtrTy = Type::getInt8PtrTy(C);
    Int32PtrTy = Type::getInt32PtrTy(C);
    Int8PtrNull = ConstantPointerNull::get(Int8PtrTy);
  }

  Constant *createGlobalVal(std::string &Str, StringRef TypeName,
                            SmallVectorImpl<Constant *> &BodyVec);
};

struct ParamInfo {
  StringRef TypeName;
  StringRef ParamName;
  Constant *Annotation;
};

struct MethodInfo : BaseInfo {
  bool IsStatic = false;
  unsigned Modifier = 0;
  StringRef MethodLinkName;
  StringRef MethodName;
  StringRef ReturnType;
  Constant *Annotation;
  SmallVector<ParamInfo, 8> ActualParams;
  SmallVector<StringRef, 8> GenericParams;

  explicit MethodInfo(Module &M) : BaseInfo(M) {}

  Constant *fillMethodInfo(unsigned Idx, StringRef ClassName);
  Constant *fillActualParamInfo(StringRef Prefix);
  Constant *fillGenericParamInfo(StringRef Prefix);
};

struct EnumCtorInfo {
  StringRef CtorName;
  StringRef TypeName;
};

struct FieldInfo : BaseInfo {
  bool IsStatic = false;
  unsigned Modifier = 0;
  unsigned FieldIdx = 0;
  StringRef StaticName;
  StringRef TypeName;
  StringRef FieldName;
  Constant *Annotation;

  explicit FieldInfo(Module &M) : BaseInfo(M) {}

  Constant *fillStaticFieldInfo();
  Constant *fillInstanceFieldInfo(unsigned Idx, StringRef ClassName);
};

struct ClassReflectInfo : BaseInfo {
  unsigned Modifier = 0;
  Constant *Annotation;
  StringRef GenericClass;
  SmallVector<StringRef, 8> FieldNames;
  SmallVector<FieldInfo *, 8> InstanceFields;
  SmallVector<FieldInfo *, 8> StaticFields;
  SmallVector<MethodInfo *, 8> InstanceMethods;
  SmallVector<MethodInfo *, 8> StaticMethods;

  explicit ClassReflectInfo(Module &M) : BaseInfo(M) {}

  Constant *fillInstanceFieldNames(StringRef ClassName);
};

struct EnumReflectInfo : BaseInfo {
  unsigned Modifier = 0;
  Constant *Annotation;
  StringRef GenericClass;
  SmallVector<EnumCtorInfo, 8> EnumCtors;
  SmallVector<MethodInfo *, 8> InstanceMethods;
  SmallVector<MethodInfo *, 8> StaticMethods;
  SmallVector<Constant*, 8> CtorAnnotationMethods;

  explicit EnumReflectInfo(Module &M) : BaseInfo(M) {}

  Constant *fillEnumCtorInfos(StringRef TypeInfoName);
};

struct EnumCtorReflectInfo : BaseInfo {
  unsigned Modifier = 0;
  Constant *Annotation;
  explicit EnumCtorReflectInfo(Module &M) : BaseInfo(M) {}
};

class ReflectInfo : BaseInfo {
public:
  explicit ReflectInfo(Module &M);
  ~ReflectInfo() {
    for (auto M : AllMethods)
      delete (M);

    for (auto F : AllFields)
      delete (F);
  }
  Constant *getReflectionInfo(GlobalVariable *GV, bool IsEnum);
  Constant *getClassReflectInfo(const MDTuple *ReflectMD);
  Constant *getEnumReflectInfo(const MDTuple *ReflectMD);
  Constant *getEnumCtorReflectInfo(const MDTuple *ReflectMD);
  Constant *getClassDebugInfo(const MDTuple *ReflectMD);
  Constant *getEnumDebugInfo(const MDTuple *ReflectMD);
  bool fillPackageInfo();
  static bool enable(Module &M);

private:
  StringRef ModuleName;
  StringRef PkgName;

  SetVector<GlobalVariable *> TypeInfos;
  SetVector<GlobalVariable *> TypeTemplates;
  std::map<StringRef, MethodInfo *> StaticGlobalMethods;
  std::map<StringRef, FieldInfo *> StaticGlobalFields;

  const DataLayout &DL;
  StringRef TIName = "";

  SmallVector<MethodInfo *> AllMethods;
  SmallVector<FieldInfo *> AllFields;

  std::pair<GlobalVariable *, StructType *>
  buildPackageInfoGV(std::string PackName);
  void parseAttributes(MDTuple *Attr, unsigned &Modifier,
                       Constant *&Annotation);
  void parseInstanceFieldMDs(ClassReflectInfo &Info, const MDTuple *FieldMDs);
  void parseStaticFieldMDs(ClassReflectInfo &Info, const MDTuple *FieldMDs);
  void parseParamMd(SmallVectorImpl<ParamInfo> &ParamInfos, MDTuple *ParamMD);
  void parseGenericParamMd(SmallVectorImpl<StringRef> &ParamInfos,
                           MDTuple *ParamMD);
  bool parseOneMethodMd(MethodInfo &Info, MDTuple *MethodMD, bool IsStatic);
  void parseInstanceMethodMDs(SmallVectorImpl<MethodInfo *> &Infos,
                              MDTuple *MethodMDs);
  void parseStaticMethodMDs(SmallVectorImpl<MethodInfo *> &Infos,
                            MDTuple *MethodMDs);
  void parseCtorAnnotationMDs(SmallVectorImpl<Constant*> &Infos,
                              MDTuple *CtorAnnoMDs);
  void parseEnumCtorMDs(SmallVectorImpl<EnumCtorInfo> &Info, MDTuple *CtorMDs);
  void parseOneInstanceFieldMd(FieldInfo &Info, MDTuple *FieldMD);
  void parseOneStaticFieldMd(FieldInfo &Info, MDTuple *FieldMD);
  void fillClassReflectInfo(ClassReflectInfo &Info,
                            SmallVector<Constant *, 0> &BodyVec);
  void fillEnumReflectInfo(EnumReflectInfo &Info,
                           SmallVector<Constant *, 0> &BodyVec);
  void fillEnumCtorReflectInfo(EnumCtorReflectInfo &Info,
                               SmallVector<Constant *, 0> &BodyVec);
  void getGlobalsFromNamedMD(SetVector<GlobalVariable *> &GVs,
                             NamedMDNode *MDNode);
  MethodInfo *getMethodInfo() {
    MethodInfo *MI = new MethodInfo(M);
    AllMethods.push_back(MI);
    return MI;
  }
  FieldInfo *getFieldInfo() {
    FieldInfo *FI = new FieldInfo(M);
    AllFields.push_back(FI);
    return FI;
  }
};
StringRef getStringFromMD(const MDTuple *MD, unsigned Idx);
MDTuple *getMDOperand(const MDTuple *MD, unsigned Idx);
} // namespace llvm
