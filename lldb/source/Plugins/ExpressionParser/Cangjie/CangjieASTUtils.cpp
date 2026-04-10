//===-- CangjieASTUtils.cpp -----------------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#include "CangjieASTUtils.h"
#include "cangjie/AST/Create.h"
#include "cangjie/Utils/CastingTemplate.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "lldb/Core/ValueObject.h"

using namespace lldb_private;
using namespace Cangjie;
using namespace Cangjie::AST;

bool lldb_private::StartsWith(const std::string& str, const std::string& pre)
{
    return str.find(pre) == 0;
}
bool EndsWith(const std::string& str, const std::string& suf) {
    if (str.length() < suf.length()) {
        return false;
    }
    return str.rfind(suf) == str.length() - suf.length();
}

std::string lldb_private::DeletePrefixOfType(const std::string& typeName) {
  auto name = typeName;
  if (IsEnumName(name)) {
    auto enumPrefix = GetEnumPrefix(name);
    auto ltPos = name.find(enumPrefix);
    return name.substr(0, ltPos) + name.substr(ltPos+ enumPrefix.size(), name.size());
  }
  // Delete interface$
  auto ltPosI = name.find(INTERFACE_PREFIX_NAME);
  if (ltPosI != std::string::npos) {
    return name.substr(0, ltPosI) + name.substr(ltPosI+ INTERFACE_PREFIX_NAME.size(), name.size());
  }
  return name;
}

std::string lldb_private::GetEnumPrefix(const std::string& name) {
  if (name.find(ENUM_PREFIX_NAME) != std::string::npos) {
    return ENUM_PREFIX_NAME;
  } else if (name.find(E0_PREFIX_NAME) != std::string::npos) {
    return E0_PREFIX_NAME;
  } else if (name.find(E2_PREFIX_NAME_OPTION_LIKE) != std::string::npos) {
    return E2_PREFIX_NAME_OPTION_LIKE;
  } else if (name.find(E3_PREFIX_NAME) != std::string::npos) {
    return E3_PREFIX_NAME;
  }

  return "";
}

std::string lldb_private::GetEnumNameWithoutPrefix(std::string& name, std::string& pkgname) {
  auto enumPrefix = GetEnumPrefix(name);
  // For example, enum RGBColor in package pkg's name is pkg::E0$RGBColor.
  auto e0Pos = name.find(E0_PREFIX_NAME);
  if (e0Pos != std::string::npos) {
    if (e0Pos == 0) {
      return pkgname + PACKAGE_SUFFIX + name.substr(e0Pos+ E0_PREFIX_NAME.size(), name.size());
    }
    return name.substr(0, e0Pos) + name.substr(e0Pos + E0_PREFIX_NAME.size(), name.size());
  }
  // For example, if the enum RGBColor's name is pkg::Enum$RGBColor, then subname is Enum$RGBColor.
  auto ltPos = name.find(enumPrefix);
  auto subname = GetSubNameWithoutPkgname(name, pkgname);
  if (subname.empty()) {
    subname = name.substr(0, ltPos) + name.substr(ltPos+ enumPrefix.size(), name.size());
    return subname;
  } else {
    subname = pkgname + PACKAGE_SUFFIX + name.substr(ltPos+ enumPrefix.size(), name.size());
    return subname;
  }
}

std::string lldb_private::GetTypeNameWithoutPrefix(ConstString type_name, std::string& pkgname)
{
    std::string name = SafeAsCString(type_name);
    name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
    if (EndsWith(name, "*")) {
      name = name.substr(0, name.size() - 1);
    }
    name = DeletePrefixOfType(name);
    auto subname = GetSubNameWithoutPkgname(name, pkgname);
    // For example, default::A<UInt32> 's type name is A.
    auto pos = subname.rfind("<");
    if (pos != std::string::npos) {
      subname = subname.substr(0, pos);
    }
    return subname.empty() ? name : subname;
}

std::string lldb_private::DeleteAllPkgname(const std::string& name, std::string& pkg) {
  // Multiple package names may exist. for example, default::Test<default::I>.
  std::string prefix = pkg + PACKAGE_SUFFIX;
  std::string typeName = name ;
  typeName = GetSubNameWithoutPkgname(typeName, pkg);
  if (typeName.empty()) {
    typeName = name;
  }
  auto pos = typeName.find(prefix);
  while (pos != std::string::npos) {
    typeName = typeName.substr(0, pos) + typeName.substr(pos + prefix.size(), typeName.size());
    pos = typeName.find(prefix);
  }
  return typeName;
}

std::string lldb_private::DeleteAllGenericParamsPkgname(const std::string& name) {
  size_t start = name.find('<');
  size_t end = name.find('>');
  if (start == std::string::npos || end == std::string::npos || start >= end) {
      return name;
  }

  std::string prefix = name.substr(0, start);
  std::string content = name.substr(start + 1, end - start - 1);
  std::string delimiter = ",";
  std::string result = prefix + "<";

  size_t pos = 0;
  bool first = true;
  while (pos < content.length()) {
      size_t found = content.find(delimiter, pos);
      if (found == std::string::npos) {
          found = content.length();
      }
      std::string token = content.substr(pos, found - pos);
      size_t colonPos = token.find("::");
      if (colonPos != std::string::npos) {
          token = token.substr(colonPos + 2);
      }

      if (!first) {
          result += delimiter;
      }
      result += token;
      first = false;
      pos = found + 1;
  }
  result += ">";
  return result;
}

std::string lldb_private::GetSubNameWithoutPkgname(std::string& name, std::string& pkg)
{
  if (pkg.empty()) {
    return "";
  }
  name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
  // For example, a pkg name can be "pkg::HashMap<pkg::String, Int64>".
  auto prefix = pkg + PACKAGE_SUFFIX;
  if (StartsWith(name, prefix)) {
    return name.substr(prefix.size(), name.size() - prefix.size());
  }
  return "";
}

std::string lldb_private::GetPkgNameFromCompilerUnit(const CompileUnit& compUnit)
{
  std::string diFileName = SafeAsCString(compUnit.GetPrimaryFile().GetFilename());
  return diFileName.substr(diFileName.find('-') + 1);
}

bool CangjieASTBuiler::IsFunctionType(ConstString type_name)
{
  ConstString compare("(.+::|^)\\(.*\\)( ?)->.+$");
  RegularExpression regex(compare.GetStringRef());
  return regex.Execute(type_name.AsCString());
}

static std::map<std::string, Cangjie::AST::TypeKind> base_type_map = {
    { "Unit", Cangjie::AST::TypeKind::TYPE_UNIT },
    { "Bool", Cangjie::AST::TypeKind::TYPE_BOOLEAN },
    { "Rune", Cangjie::AST::TypeKind::TYPE_RUNE },
    { "Int8", Cangjie::AST::TypeKind::TYPE_INT8},
    { "UInt8", Cangjie::AST::TypeKind::TYPE_UINT8 },
    { "Int16", Cangjie::AST::TypeKind::TYPE_INT16 },
    { "UInt16", Cangjie::AST::TypeKind::TYPE_UINT16 },
    { "Int32", Cangjie::AST::TypeKind::TYPE_INT32 },
    { "UInt32", Cangjie::AST::TypeKind::TYPE_UINT32 },
    { "Int64", Cangjie::AST::TypeKind::TYPE_INT64 },
    { "UInt64", Cangjie::AST::TypeKind::TYPE_UINT64 },
    { "IntNative", Cangjie::AST::TypeKind::TYPE_INT_NATIVE},
    { "UIntNative", Cangjie::AST::TypeKind::TYPE_UINT_NATIVE},
    { "Float16", Cangjie::AST::TypeKind::TYPE_FLOAT16 },
    { "Float32", Cangjie::AST::TypeKind::TYPE_FLOAT32 },
    { "Float64", Cangjie::AST::TypeKind::TYPE_FLOAT64 },
};

OwnedPtr<AST::Type> CangjieASTBuiler::CreateFuncType(Ptr<Decl> target, std::string pkg,
                                                     const CompilerType &type)
{
  // Prefer using the CompilerType to derive parameter and return types.
  OwnedPtr<FuncType> ret = MakeOwned<FuncType>();
  if (!type.IsValid()) {
    return ret;
  }

  auto func_type = type;
  if (func_type.IsPointerType()) {
    func_type = func_type.GetPointeeType();
  }
  if (!func_type.IsFunctionType()) {
    auto field = func_type.GetFieldWithName("ptr");
    if (field.IsValid() && field.IsPointerType()) {
      func_type = field.GetPointeeType();
    }
  }
  if (!func_type.IsFunctionType()) {
    return ret;
  }

  auto paramCount = func_type.GetFunctionArgumentCount();
  for (int i = 0; i < paramCount; i++) {
    auto paramType = func_type.GetFunctionArgumentAtIndex(i);
    std::string paramTypeStr = SafeAsCString(paramType.GetTypeName());
    ret->paramTypes.emplace_back(CreateAstType(AstTypeInfo(paramTypeStr, paramType), target, pkg, true));
  }

  auto retType = func_type.GetFunctionReturnType();
  if (retType.IsValid()) {
    std::string retTypeStr = SafeAsCString(retType.GetTypeName());
    ret->retType = CreateAstType(AstTypeInfo(retTypeStr, retType), target, pkg, true);
  }
  return ret;
}

bool CheckTypeCorrect(std::string& typeStr)
{
  // Checks if < and > match in a string.
  auto ltNum = std::count(typeStr.begin(), typeStr.end(), '<');
  auto gtNum = std::count(typeStr.begin(), typeStr.end(), '>');
  if (ltNum == gtNum) {
    return true;
  }
  return false;
}

static bool IsEnumWithArgs(ConstString type_name)
{
  ConstString compare("(.+)?E[1-3]\\$");
  RegularExpression regex(compare.GetStringRef());
  return regex.Execute(type_name.AsCString());
}

static std::vector<CompilerType>
GetEnumElementsType(std::vector<std::string> elements, CompilerType type, std::string pkg)
{
  std::vector<CompilerType> elements_type(elements.size());
  if (!type.IsValid()) {
    return elements_type;
  }
  if (type.IsPointerType()) {
    type = type.GetPointeeType();
  }
  CompilerType arg;
  if (type.GetTypeName().GetStringRef().contains("E2$")) {
    arg = type.GetFieldWithName("val");
    if (!arg.IsValid()) {
      return elements_type;
    }
    if (arg.IsPointerType()) {
      arg = arg.GetPointeeType();
    }
    elements_type[0] = arg;
    return elements_type;
  }

  for (uint32_t i = 0; i < type.GetNumDirectBaseClasses(); i++) {
    auto base = type.GetDirectBaseClassAtIndex(i, nullptr);
    auto num_fields = base.GetNumFields();
    if (num_fields != elements.size() + 1) {
      continue;
    }
    for (uint32_t j = 1; j < num_fields; j++) {
      std::string name;
      arg = base.GetFieldAtIndex(j, name, nullptr, nullptr, nullptr);
      if (arg.IsPointerType()) {
        arg = arg.GetPointeeType();
      }
      auto arg_name = arg.GetTypeName();
      if (arg_name != elements[j - 1].c_str() && arg_name != (pkg + std::string("::") + elements[j - 1]).c_str()) {
        break;
      }
      elements_type[j - 1] = arg;
    }
  }
  return elements_type;
}

OwnedPtr<AST::Type> CangjieASTBuiler::CreateRefType(AstTypeInfo info, Ptr<Decl> target, std::string pkg,
                                                    bool isGenericDeclFromTarget)
{
  // For example, a reference type name can be "Range<Int64>" or "Array<String>" or "HashMap<String, Int64>".
  OwnedPtr<RefType> ret = MakeOwned<RefType>();
  auto name = info.name;
  ret->ref.target = target;
  name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
  if (EndsWith(name, "*")) {
    name = name.substr(0, name.size() - 1);
  }
  // For example, enum RGBColor{Red} 's type name is pkg.Enum$RGBColor and pkg.RGBColor is need.
  if (IsEnumName(name)) {
    name = GetEnumNameWithoutPrefix(name, pkg);
  }
  // When the reference type is a generic interface, for example, if the type name is `default::Interface$B<$G_T>`,
  // then the reference type name is `B`.
  if (name.find(INTERFACE_PREFIX_NAME) != std::string::npos) {
    name = DeletePrefixOfType(name);
  }
  if (IsGenericType(name)) {
    if (target && target->identifier.Val() != GetGenericDeclName(name)) {
      Ptr<AST::Decl> genericTarget = GetTargetOfGenericDecl(target, name);
      if (genericTarget) {
        ret->ref.target = genericTarget;
      }
    }
  }
  auto ltPos = name.find("<");
  // When a reference type has generic parameters, the parameters may have package names. For example, C<default::D>.
  if (auto lcolon = name.find(PACKAGE_SUFFIX);
      lcolon == std::string::npos || (ltPos != std::string::npos && ltPos < lcolon)) {
    name = pkg + PACKAGE_SUFFIX + name;
    ltPos = name.find("<");
  }
  if (ltPos == std::string::npos || name.back() != '>') {
    if (IsGenericType(name)) {
      name = GetGenericParamDeclName(name);
    }
    ret->ref.identifier = name;
    m_type_names.insert(ret->ref.identifier);
    return ret;
  }
  ret->ref.identifier = name.substr(0, ltPos);
  m_type_names.insert(ret->ref.identifier);
  if (!isGenericDeclFromTarget) {
    return ret;
  }
  auto idLen = ret->ref.identifier.Val().size();
  // typeArg: String,Array<ArrayList<HashMap<Int64,Object>>>,String
  auto args = name.substr(idLen + 1, name.size() - idLen - 2);
  auto elements = SplitTypeName(args);
  std::vector<CompilerType> elements_type(elements.size());
  if (IsEnumWithArgs(ConstString(info.name)) && !IsGenericType(name)) {
    elements_type = GetEnumElementsType(elements, info.type, pkg);
  }
  for(size_t i = 0; i < elements.size(); i++) {
    ret->typeArguments.emplace_back(CreateAstType(AstTypeInfo(elements[i], elements_type[i]), target, pkg));
  }
  return ret;
}

bool CangjieASTBuiler::IsRefType(const Ptr<AST::Ty>& ty) {
  CJC_NULLPTR_CHECK(ty);
  if (ty->IsClassLike()) {
    return true;
  }
  return false;
}

std::vector<std::string> CangjieASTBuiler::SplitCollectionName(std::string name) {
  // For example, a name can be "String" or "Range<Int64>" or "Array<String>" or "HashMap<String, Int64>".
  std::vector<std::string> ret;
  name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
  if (EndsWith(name, "*")) {
    name = name.substr(0, name.size() - 1);
  }
  auto ltPos = name.find("<");
  if (ltPos == std::string::npos || name.back() != '>') {
    return ret;
  }
  auto idLen = name.substr(0, ltPos).size();
  // typeArg: String,Array<ArrayList<HashMap<Int64,Object>>>,String
  auto args = name.substr(idLen + 1, name.size() - idLen - 2);
  return SplitTypeName(args);
}

std::vector<std::string> CangjieASTBuiler::SplitTypeName(std::string name) {
  std::vector<std::string> ret;
  if (name.find(",") == std::string::npos) {
    ret.emplace_back(name);
    return ret;
  }
  std::stringstream ss(name);
  std::string typeStr;
  std::string arg;
  while (std::getline(ss, arg, ',')) {
    if (arg.empty()) {
      break;
    }
    typeStr += arg;
    if (!CheckTypeCorrect(typeStr)) {
      typeStr += ",";
      continue;
    }
    ret.emplace_back(typeStr);
    typeStr = "";
  }
  return ret;
}

std::vector<std::string> CangjieASTBuiler::SplitTupleName(std::string name) {
  // For example, a tuple name can be "Tuple<Bool, String, Array<UInt8>>".
  std::vector<std::string> ret;
  auto tlen = TUPLE_NAME.size();
  name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
  auto args = name.substr(tlen + 1, name.size() - tlen - 2);
  return SplitTypeName(args);
}

OwnedPtr<AST::Type> CangjieASTBuiler::CreateTupleType(AstTypeInfo info, Ptr<Decl> target, std::string pkg)
{
  OwnedPtr<TupleType> ret = MakeOwned<TupleType>();
  if (IsGenericType(info.name)) {
   std::vector<std::string> elements = SplitTupleName(info.name);
   for (auto& ele:elements) {
     ret->fieldTypes.emplace_back(CreateAstType(AstTypeInfo(ele), target));
   }
   return ret;
  }
  auto type = info.type;
  if (!type.IsValid()) {
    return nullptr;
  }
  for (uint32_t i = 0; i < type.GetNumFields(); i++) {
    std::string name;
    auto field = type.GetFieldAtIndex(i, name, nullptr, nullptr, nullptr);
    if (!field.IsValid()) {
      return nullptr;
    }
    if (field.IsPointerType()) {
      field = field.GetPointeeType();
    }
    ret->fieldTypes.emplace_back(CreateAstType(AstTypeInfo(field.GetTypeName().AsCString(), field), target, pkg));
  }
  return ret;
}

OwnedPtr<AST::Type> CangjieASTBuiler::CreateVArrayType(std::string name, Ptr<Decl> target)
{
    // For example, a VArray name can be "VArray<Int64, $3>".
    OwnedPtr<VArrayType> ret = MakeOwned<VArrayType>();
    auto valen = VARRAY_NAME.size();
    name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
    auto vargs = name.substr(valen + 1, name.size() - valen - 2);
    std::stringstream ss(vargs);
    std::string typeStr;
    std::string varg;
    while (std::getline(ss, varg, ',')) {
        if (varg.empty()) {
            break;
        }
        typeStr += varg;
        if (!CheckTypeCorrect(typeStr)) {
          typeStr += ",";
          continue;
        }
        if (StartsWith(typeStr, "$")) {
            OwnedPtr<LitConstExpr> lit = MakeOwned<LitConstExpr>();
            lit->kind = LitConstKind::INTEGER;
            lit->stringValue = typeStr.substr(1, typeStr.size() - 1);
            OwnedPtr<ConstantType> constType = MakeOwned<ConstantType>();
            constType->constantExpr = std::move(lit);
            ret->constantType = std::move(constType);
        } else {
            ret->typeArgument = CreateAstType(AstTypeInfo(typeStr), target);
        }
        typeStr = "";
    }
    return ret;
}

OwnedPtr<AST::Type> CangjieASTBuiler::CreateOptionType(CompilerType type, Ptr<Decl> target, std::string pkg)
{
  if (!type.IsValid()) {
    return nullptr;
  }

  std::string name = "";
  auto val_type = type.GetFieldWithName("val");
  if (!val_type.IsValid()) {
    return nullptr;
  }
  if (val_type.IsPointerType()) {
    val_type = val_type.GetPointeeType();
  }

  OwnedPtr<RefType> ret = MakeOwned<RefType>();
  ret->ref.target = target;
  ret->ref.identifier = "Option";
  ret->typeArguments.emplace_back(CreateAstType(AstTypeInfo(val_type.GetTypeName().AsCString(), val_type),
                                                target, pkg));
  return ret;
}

OwnedPtr<AST::Type> CangjieASTBuiler::CreateAstType(AstTypeInfo info, Ptr<Decl> target, std::string pkg,
                                                    bool isGenericDeclFromTarget)
{
  auto name = info.name;
  CompilerType type = info.type;
  auto subname = GetSubNameWithoutPkgname(name, pkg);
  if (!subname.empty()) {
    return CreateAstType(AstTypeInfo(subname, type), target, pkg, isGenericDeclFromTarget);
  }
  if (IsFunctionType(ConstString(name))) {
    return CreateFuncType(target, pkg, type);
  }
  if (StartsWith(name, TUPLE_NAME)) {
    return CreateTupleType(AstTypeInfo(name, type), target, pkg);
  }
  if (StartsWith(name, VARRAY_NAME)) {
    return CreateVArrayType(name, target);
  }
  if (StartsWith(name, OPTION_NAME)) {
    auto ret = CreateOptionType(type, target, pkg);
    if (ret != nullptr) {
      return ret;
    }
  }
  if (base_type_map.find(name) != base_type_map.end()) {
    OwnedPtr<PrimitiveType> primType = MakeOwned<PrimitiveType>();
    primType->kind = base_type_map[name];
    primType->str = name;
    return primType;
  }
  return CreateRefType(AstTypeInfo(name, type), target, pkg, isGenericDeclFromTarget);
}

void CangjieASTBuiler::CreateFuncDeclGeneric(Ptr<AST::FuncDecl> decl, CompilerType& type, std::string& name,
                                             Ptr<AST::Decl> pdecl) {
  if (!decl || !decl->funcBody || name.empty() || decl->identifier == name) {
    return;
  }
  auto typeName = GetSubNameWithoutPkgname(name, decl->fullPackageName);
  if (typeName.empty()) {
    typeName = name;
  }
  auto pkgPos = typeName.rfind(PACKAGE_SUFFIX);
  if (pkgPos!= std::string::npos) {
    typeName = typeName.substr(pkgPos + 2, typeName.size() - 1);
  }
  if (!IsGenericTypeParameters(typeName)) {
    return;
  }
  // If funcDecl's generic from outerDecl, set target. otherwise, create it.
  Log *log = GetLog(LLDBLog::Expressions);
  LLDB_LOGF(log, "create func [%s] 's generic type %s\n", ConstString(decl->identifier.Val()).AsCString(),
            ConstString(name).AsCString());
  // for member func, like default::A<T, K>::init => A<T, K>::f<T> => f<T>
  // for global func, like default::fun1<KK>
  decl->EnableAttr(Attribute::GENERIC);
  decl->funcBody->generic = MakeOwned<Generic>();
  // default::Pair<$G_T,$G_U> => Pair<$G_T,$G_U> => T, U
  std::vector<std::string> params;
  SetGenericByName(decl->funcBody->generic.get(), typeName, decl);
}

void CangjieASTBuiler::SetGenericByName(Ptr<AST::Generic> generic, std::string& typeName, const Ptr<AST::Decl>& decl) {
  auto subname = typeName;
  auto lpos = subname.find("<");
  auto rpos = subname.find(">");
  auto paramsName = subname.substr(lpos + 1, rpos - lpos - 1);
  auto elements = SplitTypeName(paramsName);
  for (auto& ele:elements) {
    OwnedPtr<GenericParamDecl> gpd = MakeOwned<GenericParamDecl>();
    gpd->identifier = {GetGenericParamDeclName(ele), decl->begin, decl->end};
    gpd->fullPackageName = decl->fullPackageName;
    gpd->begin = decl->begin;
    gpd->end = decl->begin;
    generic->typeParameters.emplace_back(std::move(gpd));
  }
}

void CangjieASTBuiler::CreateDeclGeneric(Ptr<AST::Decl> decl, std::string& typeName) {
  if (!IsGenericTypeParameters(typeName)) {
    return;
  }
  decl->EnableAttr(Attribute::GENERIC);
  decl->generic = MakeOwned<Generic>();
  // default::Pair<$G_T,$G_U> => Pair<$G_T,$G_U> => T, U
  std::vector<std::string> params;
  auto subname = GetSubNameWithoutPkgname(typeName, decl->fullPackageName);
  SetGenericByName(decl->generic.get(), subname, decl);
}

OwnedPtr<Cangjie::AST::VarDecl> CangjieASTBuiler::CreateEnumVarDecl(
    CompilerType type, std::string name, std::string prefix, Ptr<Decl> target) {
  auto decl = MakeOwned<Cangjie::AST::VarDecl>();
  decl->identifier = name;
  std::string typeStr = SafeAsCString(type.GetTypeName());
  decl->type = nullptr;
  decl->EnableAttr(Attribute::PUBLIC, Attribute::EXTERNAL);
  decl->isVar = true;
  if (decl->type) {
    decl->ty = decl->type->ty;
  }
  return decl;
}

OwnedPtr<Cangjie::AST::VarDecl> CangjieASTBuiler::CreateVarDecl(
    const CompilerType type, std::string name, std::string prefix, Ptr<Decl> target) {
  auto decl = MakeOwned<Cangjie::AST::VarDecl>();
  decl->identifier = name;
  decl->type = CreateAstType(AstTypeInfo(SafeAsCString(type.GetTypeName()), type), target, prefix, true);
  decl->EnableAttr(Attribute::PUBLIC, Attribute::GLOBAL);
  decl->isVar = true;
  if (decl->type) {
    decl->ty = decl->type->ty;
  }
  return decl;
}

OwnedPtr<Cangjie::AST::FuncDecl> CangjieASTBuiler::CreateEnumFuncDecl(
    const CompilerType& enumType, const CompilerType& ctorFuncType, std::string funcName, Ptr<AST::Decl> pdecl) {
  auto decl = MakeOwned<Cangjie::AST::FuncDecl>();
  decl->identifier = funcName;
  decl->funcBody = MakeOwned<FuncBody>();
  // For example, a generic type's name can be default::Enum$RGBColor<$G_T>.
  std::string typeStr = SafeAsCString(enumType.GetTypeName());
  decl->funcBody->retType = CreateAstType(AstTypeInfo(typeStr, enumType), pdecl, m_current_pkgname, true);
  auto paramList = MakeOwned<FuncParamList>();

  if (typeStr.find(E2_PREFIX_NAME_OPTION_LIKE) != std::string::npos) {
    bool isMember = (pdecl != nullptr);
    std::string paramTypeStr = SafeAsCString(ctorFuncType.GetTypeName());
    if (IsGenericTypeParameters(typeStr)) {
      paramTypeStr = GENERIC_TYPE_PREFIX_NAME + GetGenericParamDeclName(typeStr);
    }
    OwnedPtr<FuncParam> param = MakeOwned<FuncParam>();
    param->identifier = "a" + std::to_string(1);
    auto tempDecl = isMember ? pdecl: decl.get();
    param->type = CreateAstType(AstTypeInfo(paramTypeStr, ctorFuncType), tempDecl, m_current_pkgname, true);
    paramList->params.emplace_back(std::move(param));
  } else {
    for (uint32_t i = 1; i < ctorFuncType.GetNumFields(); i++) {
      bool isMember = (pdecl != nullptr);
      std::string member_name2;
      auto field = ctorFuncType.GetFieldAtIndex(i, member_name2, nullptr, nullptr, nullptr);
      std::string paramTypeStr = SafeAsCString(field.GetTypeName());
      OwnedPtr<FuncParam> param = MakeOwned<FuncParam>();
      param->identifier = "a" + std::to_string(i);
      auto tempDecl = isMember ? pdecl: decl.get();
      param->type = CreateAstType(AstTypeInfo(paramTypeStr, field), tempDecl, m_current_pkgname, true);
      paramList->params.emplace_back(std::move(param));
    }
  }
  decl->funcBody->paramLists.emplace_back(std::move(paramList));
  decl->EnableAttr(Attribute::PUBLIC, Attribute::EXTERNAL);
  return decl;
}

Ptr<AST::Decl> CangjieASTBuiler::GetTargetOfGenericDecl(Ptr<AST::Decl> pdecl, std::string& retTypeStr) {
  if (!IsGenericType(retTypeStr) || !pdecl) {
    return nullptr;
  }
  Ptr<Generic> generic = pdecl->GetGeneric();
  if (!generic) {
    return nullptr;
  }
  Ptr<AST::Decl> reftarget = nullptr;
  // Set ref target of refType.
  for (auto& tmpGenericParamDecl:generic->typeParameters) {
    if (tmpGenericParamDecl->identifier.Val() == GetGenericParamDeclName(retTypeStr)) {
      reftarget = tmpGenericParamDecl.get();
      break;
    }
  }
  return reftarget;
}

bool lldb_private::IsGenericFromFuncDecl(std::vector<std::string>& instantiatedNames, std::string name) {
  for (auto& cur:instantiatedNames) {
    if (cur == GetGenericParamDeclName(name)) {
      return true;
    }
  }
  return false;
}

OwnedPtr<Cangjie::AST::FuncDecl> CangjieASTBuiler::CreateFuncDecl(
    CompilerType type, std::string identifier, int line, Ptr<AST::Decl> pdecl, std::string funcName) {
  auto decl = MakeOwned<Cangjie::AST::FuncDecl>();
  auto declBegin = Position(m_file_id, line, 1);
  decl->identifier = {identifier, declBegin, declBegin + Position(0, 1, 1)};
  decl->begin = Position(m_file_id, line, 1);
  decl->end = decl->begin + Position(0, 1, 1);
  decl->funcBody = MakeOwned<FuncBody>();
  decl->fullPackageName = m_current_pkgname;
  auto subName = GetSubNameWithoutPkgname(funcName, m_current_pkgname);
  subName = subName.empty() ? funcName:subName;
  auto instantiatedNames = GetInstantiatedParamDeclName(subName);
  // For func with generic type, create generic type by funcName.
  CreateFuncDeclGeneric(decl.get(), type, funcName, pdecl);
  if (pdecl && (identifier == "init" || identifier == pdecl->identifier)) {
    decl->EnableAttr(Attribute::CONSTRUCTOR);
    decl->EnableAttr(Attribute::TOOL_ADD);
    decl->funcBody->retType = CreateAstType(AstTypeInfo(pdecl->identifier), pdecl, m_current_pkgname);
  } else {
    auto retType = type.GetFunctionReturnType();
    std::string retTypeStr = SafeAsCString(retType.GetTypeName());
    auto tempDecl = lldb_private::IsGenericFromFuncDecl(instantiatedNames, retTypeStr) ? decl.get():pdecl;
    decl->funcBody->retType = CreateAstType(AstTypeInfo(retTypeStr, retType), tempDecl, m_current_pkgname, true);
  }
  auto paramList = MakeOwned<FuncParamList>();
  for (int i = 0; i < type.GetFunctionArgumentCount(); i++) {
    auto paramType = type.GetFunctionArgumentAtIndex(i);
    std::string paramTypeStr = SafeAsCString(paramType.GetTypeName());
    if (i == 0 && paramTypeStr.find(LOCAL_FUNC_CAPTUREDVAR) != std::string::npos) {
      // If the first argument is about captured variables, you need to omit the first argument.
      m_capture_vars.insert({identifier, paramType});
      continue;
    }
    OwnedPtr<FuncParam> param = MakeOwned<FuncParam>();
    param->identifier = "a" + std::to_string(i);
    auto tempDecl = lldb_private::IsGenericFromFuncDecl(instantiatedNames, paramTypeStr) ? decl.get():pdecl;
    param->type = CreateAstType(AstTypeInfo(paramTypeStr, paramType), tempDecl, m_current_pkgname, true);
    paramList->params.emplace_back(std::move(param));
  }
  decl->funcBody->paramLists.emplace_back(std::move(paramList));
  if (!pdecl) {
    decl->EnableAttr(Attribute::GLOBAL);
  }
  decl->EnableAttr(Attribute::PUBLIC);
  return decl;
}

OwnedPtr<AST::EnumDecl> CangjieASTBuiler::CreateEnumDecl(const CompilerType& type) {
  OwnedPtr<EnumDecl> decl = MakeOwned<Cangjie::AST::EnumDecl>();
  // If the enum type `RGBColor` has parameters, the name is Enum$RGBColor.Otherwise, the name is default.RGBColor.
  decl->identifier = GetTypeNameWithoutPrefix(type.GetTypeName(), m_current_pkgname);
  decl->fullPackageName = m_current_pkgname;
  decl->mangledName = BaseMangler().Mangle(*decl);
  decl->EnableAttr(Attribute::PUBLIC, Attribute::EXTERNAL, Attribute::GLOBAL);
  return decl;
}

bool lldb_private::IsEnumName(const std::string& name) {
  return name.find("Enum$") != std::string::npos ||
         name.find(ENUM_PREFIX_NAME) != std::string::npos ||
         name.find(E0_PREFIX_NAME) != std::string::npos ||
         name.find(E2_PREFIX_NAME_OPTION_LIKE) != std::string::npos ||
         name.find(E3_PREFIX_NAME) != std::string::npos;
}

bool lldb_private::IsGenericType(const std::string& type_name) {
  auto pos = type_name.find(GENERIC_TYPE_PREFIX_NAME);
  return pos != std::string::npos;
}

bool lldb_private::IsGenericTypeParameters(const std::string& type_name) {
  auto pos = type_name.rfind("<");
  auto rpos = type_name.rfind(">");
  if (pos != std::string::npos && rpos!= std::string::npos) {
    return true;
  }
  return false;
}

std::vector<std::string> CangjieASTBuiler::GetInstantiatedParamDeclName(const std::string& declName) {
  std::vector<std::string> result;
  std::string name = declName;
  name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
  auto pkgPos = name.find(PACKAGE_SUFFIX);
  if (pkgPos!= std::string::npos) {
    name = name.substr(pkgPos + 2, name.size() - 1);
  }
  auto lpos = name.find("<");
  auto rpos = name.rfind(">");
  if (lpos == std::string::npos || rpos== std::string::npos) {
    return result;
  }
  name = name.substr(lpos + 1, rpos - lpos - 1);
  return SplitTypeName(name);
}

std::string lldb_private::GetGenericParamDeclName(const std::string& gname) {
  std::string name = gname;
  if (IsGenericTypeParameters(name)) {
    auto lpos = name.find("<");
    auto rpos = name.find(">");
    name = name.substr(lpos + 1, rpos - lpos - 1);
  }
  auto pos = name.rfind(GENERIC_TYPE_PREFIX_NAME);
  if (pos == std::string::npos) {
    return name;
  }
  auto subNameStr = name.substr(pos + GENERIC_TYPE_PREFIX_NAME.size(), name.size()-1);
  return subNameStr;
}

bool lldb_private::IsGenericInstanced(const std::string& type_name) {
  auto pos = type_name.rfind("<");
  auto rpos = type_name.rfind(">");
  if (pos == std::string::npos || rpos== std::string::npos) {
    return false;
  }
  auto subName = type_name.substr(pos+1, rpos-1);
  if (subName.rfind(GENERIC_TYPE_PREFIX_NAME) == std::string::npos ) {
    return true;
  }
  return false;
}

std::string lldb_private::GetGenericDeclName(const std::string& declName) {
  // Obtain the declaration of a generic type, for example, get A from default::A<$G_T>.
  std::string name = declName;
  // Delete pkg name.
  auto pkgPos = name.find(PACKAGE_SUFFIX);
  if (pkgPos != std::string::npos) {
    name = name.substr(pkgPos + 2, name.size() - 1);
  }
  auto pos = name.find("<");
  auto rpos = name.rfind(">");
  if (pos == std::string::npos || rpos== std::string::npos) {
    return name;
  }
  return name.substr(0, pos);
}

std::string lldb_private::SafeAsCString(const ConstString& cs) {
  const char *s = cs.AsCString();
  return s ? std::string(s) : std::string();
}
