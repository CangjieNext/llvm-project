//===-- CangjieASTUtils.h -------------------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_AST_UTILS_H
#define LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_AST_UTILS_H
#include "lldb/Symbol/CompileUnit.h"
#include "cangjie/Mangle/BaseMangler.h"
#include "cangjie/Sema/TypeManager.h"
#include <utility>

using namespace Cangjie;
using namespace Cangjie::AST;

namespace lldb_private {
inline const std::string LOCAL_PACKAGE_NAME = "__lldb_locals";
inline const std::string EXPR_RESULT_NAME = "__lldb_expr_result";
inline const std::string RANGE_NAME = "Range";
inline const std::string TUPLE_NAME = "Tuple";
inline const std::string OPTION_NAME = "std.core::Option<";
inline const std::string INJECTED_SUPER_NAME = "$__lldb_injected_super";
inline const std::string INJECTED_THIS_NAME = "__lldb_injected_self";
inline const std::string COLLECTION_PACKAGE_NAME = "std.collection";
inline const std::string ENUM_PREFIX_NAME = "E1$"; // Common enum
inline const std::string E0_PREFIX_NAME = "E0$"; // all enum constructors are without parameter.
inline const std::string E2_PREFIX_NAME_OPTION_LIKE = "E2$"; // option like enum
inline const std::string E2_CTOR_PREFIX_NAME = "N$_";
inline const std::string E3_PREFIX_NAME = "E3$"; // all enum constructors are without ref parameter.
inline const std::string INTERFACE_PREFIX_NAME = "Interface$";
inline const std::string GENERIC_TYPE_PREFIX_NAME = "$G_";
inline const std::string CJDB_ADD_GENERIC_MEMBER_FUNC_NAME = "$GetGenericDef";
inline const std::string LOCAL_FUNC_CAPTUREDVAR = "$CapturedVars";
inline const std::string PACKAGE_SUFFIX = "::";

struct AstTypeInfo {
  std::string name;
  CompilerType type;

  AstTypeInfo() = default;
  explicit AstTypeInfo(std::string n, CompilerType t = CompilerType())
      : name(std::move(n)), type(t) {}
};

class CangjieASTBuiler {
public:
    OwnedPtr<AST::Type> CreateRefType(AstTypeInfo info, Ptr<AST::Decl> target, std::string pkg,
                                      bool isGenericDeclFromTarget = true);
    OwnedPtr<AST::Type> CreateFuncType(Ptr<AST::Decl> target, std::string pkg,
                                       const CompilerType &type);
    OwnedPtr<AST::Type> CreateTupleType(AstTypeInfo info, Ptr<Decl> target, std::string pkg);
    std::vector<std::string> GetInstantiatedParamDeclName(const std::string& name);
    std::vector<std::string> SplitTupleName(std::string name);
    std::vector<std::string> SplitTypeName(std::string name);
    std::vector<std::string> SplitCollectionName(std::string name);
    OwnedPtr<AST::Type> CreateVArrayType(std::string name, Ptr<AST::Decl> target);
    OwnedPtr<AST::Type> CreateOptionType(CompilerType type, Ptr<Decl> target, std::string pkg);
    OwnedPtr<AST::Type> CreateAstType(AstTypeInfo info, Ptr<AST::Decl> target, std::string pkg = "",
                                      bool isGenericDeclFromTarget = true);
    static bool IsFunctionType(ConstString type_name);
    OwnedPtr<AST::VarDecl> CreateVarDecl(const CompilerType type, std::string name,
        std::string prefix, Ptr<AST::Decl> target = nullptr);
    OwnedPtr<AST::VarDecl> CreateEnumVarDecl(CompilerType type, std::string name,
        std::string prefix, Ptr<AST::Decl> target = nullptr);

    OwnedPtr<AST::FuncDecl> CreateEnumFuncDecl(const CompilerType &enumType, const CompilerType &ctorType,
                                           std::string name, Ptr<AST::Decl> pdecl = nullptr);
    OwnedPtr<AST::FuncDecl> CreateFuncDecl(
        CompilerType type, std::string identifier, int line = 1, Ptr<AST::Decl> pdecl = nullptr, std::string funcName = "");
    OwnedPtr<AST::EnumDecl> CreateEnumDecl(const CompilerType& type);
    void SetGenericByName(Ptr<AST::Generic> generic, std::string& typeName, const Ptr<AST::Decl>& decl);
    void CreateDeclGeneric(Ptr<AST::Decl> ret, std::string& typeName);
    void CreateFuncDeclGeneric(Ptr<AST::FuncDecl> ret, CompilerType& type, std::string& name, Ptr<AST::Decl> pdecl);
    Ptr<AST::Decl> GetTargetOfGenericDecl(Ptr<AST::Decl> pdecl, std::string& retTypeStr);
    bool IsRefType(const Ptr<AST::Ty>& ty);
    std::string m_current_pkgname;
    std::set<std::string> m_type_names;
    std::map<std::string, lldb_private::CompilerType> m_capture_vars;
    unsigned int m_file_id;
};

bool IsEnumName(const std::string& str);
std::string GetEnumPrefix(const std::string& name);
std::string GetEnumNameWithoutPrefix(std::string& name, std::string& pkgname);
bool StartsWith(const std::string& str, const std::string& pre);
std::string GetPkgNameFromCompilerUnit(const CompileUnit& compUnit);
std::string GetSubNameWithoutPkgname(std::string& name, std::string& pkg);
std::string DeleteAllPkgname(const std::string& name, std::string& pkg);
std::string DeleteAllGenericParamsPkgname(const std::string& name);
std::string DeletePrefixOfType(const std::string& name);
bool IsGenericTypeParameters(const std::string& type_name);
bool IsGenericType(const std::string& type_name);
bool IsGenericInstanced(const std::string& type_name);
bool IsGenericFromFuncDecl(std::vector<std::string>& instantiatedNames, std::string name);
std::string GetGenericDeclName(const std::string& type_name);
std::string GetGenericParamDeclName(const std::string& name);
std::string GetTypeNameWithoutPrefix(ConstString type_name, std::string& pkgname);
std::string SafeAsCString(const ConstString& cs);
}

#endif
