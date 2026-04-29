//===-- CangjieDeclMap.cpp ------------------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#include "CangjieDeclMap.h"
#include "CangjieIRForTarget.h"
#include "cangjie/AST/Create.h"
#include "cangjie/AST/Utils.h"
#include "lldb/Core/Module.h"
#include "lldb/Symbol/SymbolVendor.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "clang/Sema/Sema.h"
#include "Plugins/TypeSystem/Clang/TypeSystemClang.h"
#include "CangjieDemangle.h"
using namespace lldb_private;
using namespace Cangjie;
using namespace Cangjie::AST;

bool CangjieDeclMap::IsInterfaceType(CompilerType& type) {
  if (type.GetTypeClass() == lldb::eTypeClassClass) {
    std::string typeName = SafeAsCString(type.GetTypeName());
    if (typeName.find(INTERFACE_PREFIX_NAME) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void CangjieDeclMap::CollectLocalVariableAddress(const std::string& sname, lldb::VariableSP& var) {
    auto frame = m_exe_ctx.GetFramePtr();
    auto target = m_exe_ctx.GetTargetSP();
    if (!var || !frame || !target) {
        return;
    }
    auto valobj = frame->GetValueObjectForFrameVariable(var, lldb::DynamicValueType::eNoDynamicValues);
    AddressType address_type = eAddressTypeInvalid;
    const bool scalar_is_load_address = false;
    lldb::addr_t addr = valobj->GetAddressOf(scalar_is_load_address, &address_type);
    m_captured_var_addrs[sname] = (uint64_t)addr;
}

void CangjieDeclMap::CacheParsedVars(ConstString name, lldb::VariableSP& var, VariableKind kind) {
  CompilerType type = var->GetType()->GetFullCompilerType();
  if (m_parsed_vars.find(name) != m_parsed_vars.end()) {
      if (kind == VariableKind::LocalVariable) {
          m_parsed_vars[name] = var;
      }
  } else {
      if (kind != VariableKind::LocalVariable && IsInterfaceType(type)) {
        m_parsed_vars.insert({var->GetName(), var});
        if (kind == VariableKind::StaticVariable) {
          m_parsed_vars.insert({name, var});
        }
      } else if (kind != VariableKind::LocalVariable) {
        m_parsed_vars.insert({var->GetName(), var});
      }
      m_parsed_vars.insert({name, var});
  }
}

lldb::VariableSP CangjieDeclMap::GetVariableByName(const std::string& sname, VariableKind kind) {
  ConstString name(sname.c_str());
  lldb::VariableSP var;
  if (kind == VariableKind::LocalVariable) {
      var = m_local_variables->FindVariable(name, true);
      CollectLocalVariableAddress(sname, var);
  } else if (kind == VariableKind::StaticVariable) {
    var = m_static_variables->FindVariable(name, true);
  } else {
      var = m_global_variables->FindVariable(name, true);
  }
  return var;
}

void CangjieDeclMap::CollectOptionValType(CompilerType& val_type) {
  for (uint32_t i = 0; i < val_type.GetNumFields(); i++) {
    std::string member_name;
    auto child_type = val_type.GetFieldAtIndex(i, member_name, nullptr, nullptr, nullptr);
    if (member_name != "val") {
      continue;
    }
    if (child_type.GetTypeClass() == lldb::eTypeClassPointer) {
      child_type = child_type.GetPointeeType();
    }
    if (child_type.GetTypeClass() == lldb::eTypeClassTypedef) {
      auto underlyingType = child_type.GetTypedefedType();
      std::string typeName = SafeAsCString(underlyingType.GetTypeName());
      m_parsed_types.insert({DeletePrefixOfType(typeName), underlyingType});
    }
  }
}

CompilerType CangjieDeclMap::GetVariableType(lldb::VariableSP var) {
    CompilerType type = var->GetType()->GetFullCompilerType();
    ReplaceTypeDefWithInterfaceType(type);
    std::string type_name = SafeAsCString(type.GetTypeName());
    if (CangjieASTBuiler::IsFunctionType(type.GetTypeName())) {
      type_name = GetSubNameWithoutPkgname(type_name, m_current_pkgname);
    }
    // Variables of type interface will have a dynamic type. For example, var a:interface1 = classA()
    if (IsInterfaceType(type)) {
      auto frame = m_exe_ctx.GetFramePtr();
      if (frame != nullptr) {
        auto valobj = frame->GetValueObjectForFrameVariable(var, lldb::DynamicValueType::eNoDynamicValues);
        dynamic_type = valobj->GetDynamicType();
        m_parsed_types.insert({dynamic_type.GetTypeName().AsCString(), dynamic_type});
      }
    }
    if (ConstString(type_name).GetStringRef().contains(GENERIC_TYPE_PREFIX_NAME)) {
      auto frame = m_exe_ctx.GetFramePtr();
      if (frame != nullptr) {
        auto valobj = frame->GetValueObjectForFrameVariable(var, lldb::DynamicValueType::eNoDynamicValues);
        auto dynamic_Type = valobj->GetDynamicType();
        type = dynamic_Type;
        type_name = SafeAsCString(type.GetTypeName());
        if (CangjieASTBuiler::IsFunctionType(type.GetTypeName())) {
          type_name = GetSubNameWithoutPkgname(type_name, m_current_pkgname);
        }
      }
    }
    type_name = DeletePrefixOfType(type_name);
    if (type.GetTypeClass() == lldb::eTypeClassStruct &&
        (type_name.find("std.core::Option") == 0)) {
      // Option is now implemented as a reference type.
      type_name = type.GetTypeName().AsCString();
      CollectOptionValType(type);
    }
    if (type.GetTypeClass() == lldb::eTypeClassPointer &&
        type.GetTypeName().GetStringRef().contains(E2_PREFIX_NAME_OPTION_LIKE)) {
      type_name = DeletePrefixOfType(type.GetPointeeType().GetTypeName().AsCString());
    }
    m_parsed_types.insert({type_name, type});
    return type;
}

void CangjieDeclMap::CollectVarDecl(const std::string& sname, CompilerType& type, VariableKind kind,
                                    std::vector<CompilerTypeInfo>& m_translate_decls) {
  LookUpMemberVariable(type, m_translate_decls);
  lldb::VariableSP var = GetVariableByName(sname, kind);
  ConstString name(sname.c_str());
  if (!var) {
    return;
  }
  std::string mangledName = kind == VariableKind::StaticVariable ? var->GetMangledName().AsCString() : "";
  bool isLet = var->GetType()->GetFullCompilerType().IsConst();
  if (type.IsValid()) {
    CompilerTypeInfo tempInfo(type, 0, mangledName);
    if (var->GetScope() == lldb::eValueTypeVariableArgument) {
      isLet = true;
    }
    bool find_global = kind != VariableKind::LocalVariable;
    tempInfo.SetDeclInfo(DeclKind::VarDecl, name.AsCString(), find_global, "", isLet);
    m_translate_decls.emplace_back(tempInfo);
  }
}

CompilerType CangjieDeclMap::LookUpVariable(const std::string& sname, VariableKind kind) {
    lldb::VariableSP var = GetVariableByName(sname, kind);
    if (!var) {
      return CompilerType();
    }
    CompilerType type = GetVariableType(var);
    ConstString name(sname.c_str());
    CacheParsedVars(name, var, kind);
    return type;
}

void CangjieDeclMap::LookUpMemberVariable(CompilerType& type, std::vector<CompilerTypeInfo>& m_translate_decls) {
  std::string refTypeName = type.GetTypeName().AsCString();
  // Only collect generic instantiation types of user-defined types.
  if (!IsGenericInstanced(refTypeName)) {
    return;
  }
  if (type.GetTypeClass() != lldb::eTypeClassStruct && type.GetTypeClass() != lldb::eTypeClassClass
      && type.GetTypeClass() != lldb::eTypeClassPointer) {
    return;
  }
  Log *log = GetLog(LLDBLog::Expressions);
  // If the generic type is a custom type, collect it.
  auto instantiatedNames = builder.GetInstantiatedParamDeclName(refTypeName);
  for (auto& ele:instantiatedNames) {
    LookUpTypeByName(ele, m_translate_decls);
    auto instantiateName = GetSubNameWithoutPkgname(ele, m_current_pkgname);
    if (instantiateName.empty()) {
      instantiateName = ele;
    }
    LookUpTypeByName(instantiateName, m_translate_decls);
  }
  // for generic type, for example:
  // class C1<T> {  var c1:T }
  // struct S3<T> { var s3:T}
  // var a1 = C1<S3<Int8>>(S3<Int8>(8))
  // expr a1.c1 , S3 needs to be collected here.
  for (uint32_t i = 0; i < type.GetNumFields(); i++) {
    std::string member_name;
    auto child_type = type.GetFieldAtIndex(i, member_name, nullptr, nullptr, nullptr);
    if (member_name == "$ti" || member_name == "$ti*" || !child_type.IsValid()) {
      continue;
    }
    if (child_type.GetTypeClass() == lldb::eTypeClassPointer) {
      child_type = child_type.GetPointeeType();
    }
    if (child_type.GetTypeClass() != lldb::eTypeClassStruct && type.GetTypeClass() != lldb::eTypeClassClass) {
      continue;
    }
    auto tempName = child_type.GetTypeName().AsCString();
    if (m_parsed_types.find(tempName) != m_parsed_types.end()) {
      continue;
    }
    m_parsed_types.insert({tempName, child_type});
    CompilerTypeInfo tempInfo(child_type, 0, "");
    tempInfo.SetDeclInfo(DeclKind::UserDefinedDecl, tempName, true);
    m_translate_decls.emplace_back(tempInfo);
    if (log) {
      LLDB_LOGF(log, "[Generic]: add instantiated type [%s] for [%s:%s]. \n", tempName,
                ConstString(refTypeName).AsCString(), ConstString(member_name).AsCString());
    }
  }
}

CompilerType CangjieDeclMap::LookUpGlobalVariable(lldb::ModuleSP module,  ConstString name,
                                                  const CompilerDeclContext &namespace_decl) {
  auto target = m_exe_ctx.GetTargetSP();
  if (!target) {
    return CompilerType();
  }

  VariableList vars;
  if (module && namespace_decl) {
    module->FindGlobalVariables(name, namespace_decl, -1, vars);
  } else {
    target->GetImages().FindGlobalVariables(name, -1, vars);
  }

  if (vars.GetSize() == 0) {
    return CompilerType();
  }

  return vars.GetVariableAtIndex(0)->GetType()->GetFullCompilerType();
}

CompilerType CangjieDeclMap::LookUptype(lldb::ModuleSP module,
    ConstString name, const CompilerDeclContext &namespace_decl) {
  TypeList types;
  const bool exact_match = true;
  llvm::DenseSet<SymbolFile *> searched_symbol_files;
  if (module && namespace_decl) {
    module->FindTypesInNamespace(name, namespace_decl, 1, types);
  } else {
    auto target = m_exe_ctx.GetTargetSP();
    target->GetImages().FindTypes(
        module.get(), name, exact_match, 1, searched_symbol_files, types);
  }

  size_t num_types = types.GetSize();
  for (size_t ti = 0; ti < num_types; ++ti) {
    lldb::TypeSP type_sp = types.GetTypeAtIndex(ti);
    CompilerType full_type = type_sp->GetFullCompilerType();
    if (full_type.IsValid()) {
      return full_type;
    }
    continue;
  }

  return CompilerType();
}

bool CangjieDeclMap::LookUpPackage(ConstString name) {
  auto target = m_exe_ctx.GetTargetSP();
  NamespaceMapSP parsed_package = std::make_shared<NamespaceMap>();
  for (lldb::ModuleSP image : target->GetImages().Modules()) {
    if (!image) {
      continue;
    }

    SymbolFile *symbol_file = image->GetSymbolFile();
    if (!symbol_file) {
      continue;
    }
    CompilerDeclContext namespace_decl;
    CompilerDeclContext found_namespace_decl = symbol_file->FindNamespace(name, namespace_decl);
    if (found_namespace_decl) {
      parsed_package->push_back(
          std::pair<lldb::ModuleSP, CompilerDeclContext>(image, found_namespace_decl));
    }
  }

  if (!parsed_package->empty()) {
    m_parsed_packages.insert(std::pair<ConstString, NamespaceMapSP>(name, parsed_package));
    return true;
  }

  return false;
}

std::vector<lldb_private::CangjieDeclMap::CompilerTypeInfo> CangjieDeclMap::LookUpFunction(lldb::ModuleSP module,
    ConstString name, const CompilerDeclContext &namespace_decl) {
  std::vector<CompilerTypeInfo> result;
  SymbolContextList sc_list;
  auto target = m_exe_ctx.GetTargetPtr();
  if (namespace_decl && module) {
    ModuleFunctionSearchOptions function_options;
    function_options.include_inlines = false;
    function_options.include_symbols = false;

    module->FindFunctions(name, namespace_decl, lldb::eFunctionNameTypeBase,
                             function_options, sc_list);
  } else if (target && !namespace_decl) {
    ModuleFunctionSearchOptions function_options;
    function_options.include_inlines = false;
    function_options.include_symbols = true;
    target->GetImages().FindFunctions(
        name, lldb::eFunctionNameTypeFull | lldb::eFunctionNameTypeBase, function_options,
        sc_list);
  }

  if (!sc_list.GetSize()) {
    return result;
  }

  Log *log = GetLog(LLDBLog::Expressions);
  auto num_indices = sc_list.GetSize();
  for (uint32_t index = 0; index < num_indices; ++index) {
    SymbolContext sym_ctx;
    sc_list.GetContextAtIndex(index, sym_ctx);
    if (sym_ctx.function) {
      CompilerDeclContext decl_ctx = sym_ctx.function->GetDeclContext();
      if (!decl_ctx) {
        continue;
      }
      // Filter out class/instance methods.
      if (decl_ctx.IsClassMethod(nullptr, nullptr, nullptr)) {
        continue;
      }
      std::string funcName = SafeAsCString(sym_ctx.function->GetNameNoArguments());
      auto type = sym_ctx.function->GetCompilerType();
      CompilerTypeInfo tempInfo(type, sym_ctx.GetFunctionStartLineEntry().line,
                                sym_ctx.GetFunctionName(Mangled::NamePreference::ePreferMangled).AsCString());
      tempInfo.SetDeclInfo(DeclKind::FuncDecl, funcName, true);
      if (log) {
        LLDB_LOGF(log, "lookup add func [%s] 's type %s to m_parsed_functions. \n",
                  ConstString(name).AsCString(), type.GetTypeName().AsCString());
      }
      result.emplace_back(tempInfo);
    }
  }

  return result;
}

static bool IsLocalFunction(std::string& localScope, std::string& funcScope) {
  if (localScope == funcScope) {
    return true;
  }
  auto pos = localScope.rfind("::");
  if (pos != std::string::npos) {
    auto subLocal = localScope.substr(0, pos);
    if (subLocal.rfind("::") == std::string::npos) {
      // If there is no "::" in subLocal, then subLocal is actually globalScope.
      return false;
    }
    return IsLocalFunction(subLocal, funcScope);
  }
  return false;
}

std::string CangjieDeclMap::GetFuncNameFromComplierType(const CompilerType& funcType) {
  // For example, a func name can be (default.(Int64, Int64) -> Int64)
  std::string funcname = "(";
  bool firstParam = true;
  auto retTypeName = GetTypeNameWithoutPrefix(funcType.GetFunctionReturnType().GetTypeName(), m_current_pkgname);
  for (int i = 0; i < funcType.GetFunctionArgumentCount(); i++) {
    std::string tempargs = funcType.GetFunctionArgumentAtIndex(i).GetTypeName().AsCString();
    if (firstParam) {
      funcname = funcname + tempargs;
      firstParam = false;
    } else {
      funcname = funcname + "," + tempargs;
    }
  }
  funcname = funcname + ")->" + retTypeName;
  return funcname;
}

bool CheckNestedGenericFunction(lldb_private::ConstString funcname) {
    int count = 0;
    size_t pos = 0;
    auto str = funcname.GetStringRef();
    llvm::StringRef substr = "<T";
    while ((pos = str.find(substr, pos)) != llvm::StringRef::npos) {
        ++count;
        pos += substr.size();
    }

    return count > 1;
}

std::vector<CangjieDeclMap::CompilerTypeInfo> CangjieDeclMap::LookUpFunction(std::string name, bool find_global) {
  std::vector<CangjieDeclMap::CompilerTypeInfo> result;
  auto target = m_exe_ctx.GetTargetPtr();
  auto frame = m_exe_ctx.GetFramePtr();
  if (!target || !frame) {
    return result;
  }
  SymbolContextList sc_list;
  SymbolContextList sc_generic_list;
  ModuleFunctionSearchOptions function_options;
  function_options.include_inlines = false;
  function_options.include_symbols = true;

  target->GetImages().FindFunctions(ConstString(name),
      lldb::eFunctionNameTypeFull | lldb::eFunctionNameTypeBase, function_options, sc_list);

  std::string re_cstr = std::string("^") + llvm::Regex::escape(name) + std::string("<.+>$");
  target->GetImages().FindFunctions(RegularExpression(llvm::StringRef(re_cstr)), function_options, sc_generic_list);
  sc_list.Append(sc_generic_list);
  if (sc_list.IsEmpty()) {
    return result;
  }
  std::string globalScope = m_current_pkgname;
  std::string localScope;
  // Collect some info about our frame's context.
  auto frame_sym_ctx = frame->GetSymbolContext(lldb::eSymbolContextFunction | lldb::eSymbolContextBlock);
  if (frame_sym_ctx.function) {
    localScope = SafeAsCString(frame_sym_ctx.function->GetNameNoArguments());
  }
  Log *log = GetLog(LLDBLog::Expressions);
  auto num_indices = sc_list.GetSize();
  for (uint32_t index = 0; index < num_indices; ++index) {
    SymbolContext sym_ctx;
    sc_list.GetContextAtIndex(index, sym_ctx);
    if (!sym_ctx.function) {
      continue;
    }
    if (CheckNestedGenericFunction(sym_ctx.function->GetNameNoArguments())) {
      return result;
    }
    std::string funcNameWithPkg = SafeAsCString(sym_ctx.function->GetNameNoArguments());
    if (funcNameWithPkg.find(m_current_pkgname) == std::string::npos) {
      builder.m_type_names.insert(funcNameWithPkg);
      continue;
    }
    CompilerDeclContext decl_ctx = sym_ctx.function->GetDeclContext();
    if (!decl_ctx || decl_ctx.IsClassMethod(nullptr, nullptr, nullptr)) {
      // Filter out class/instance methods.
      continue;
    }
    std::string funcName = m_current_pkgname + PACKAGE_SUFFIX + decl_ctx.GetName().AsCString();
    std::string mangleName = SafeAsCString(sym_ctx.function->GetNameNoArguments());
    auto funcScope = mangleName.substr(0, mangleName.rfind(PACKAGE_SUFFIX));
    if (IsLocalFunction(localScope, funcScope)) {
      funcName = decl_ctx.GetName().AsCString();
    }
    std::string identifier = GetGenericDeclName(funcName);
    auto funcType = sym_ctx.function->GetCompilerType();
    std::string type_name = GetFuncNameFromComplierType(funcType);
    if ((find_global && globalScope == funcScope) ||
      (!find_global && IsLocalFunction(localScope, funcScope))) {
        if (log) {
          LLDB_LOGF(log, "add func [%s] 's type %s to m_parsed_functions. genericName[%s]\n",
                    ConstString(identifier).AsCString(), ConstString(type_name).AsCString(), ConstString(identifier).AsCString());
        }
        CompilerTypeInfo tempInfo(sym_ctx.function->GetCompilerType(), sym_ctx.GetFunctionStartLineEntry().line,
                                  sym_ctx.GetFunctionName(Mangled::NamePreference::ePreferMangled).AsCString());
        tempInfo.SetDeclInfo(DeclKind::FuncDecl, identifier, find_global, funcName);
        result.emplace_back(tempInfo);
    } else {
      builder.m_type_names.insert(identifier);
    }
    m_parsed_functions.emplace_back(ParsedFunction(funcScope, funcName, type_name, funcType));
  }
  return result;
}

CompilerType CangjieDeclMap::LookUpType(std::string name)
{
  TypeList types;
  llvm::DenseSet<SymbolFile *> searched_symbol_files;
  auto target = m_exe_ctx.GetTargetSP();
  name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
  target->GetImages().FindTypes(nullptr, ConstString(name), false, 1, searched_symbol_files, types);

  size_t num_types = types.GetSize();
  for (size_t ti = 0; ti < num_types; ++ti) {
    lldb::TypeSP type_sp = types.GetTypeAtIndex(ti);
    auto type = type_sp->GetFullCompilerType();
    std::string enumName = type.GetTypeName().AsCString();
    if (type.IsValid() && type.GetTypeClass() == lldb::eTypeClassEnumeration) {
      if (enumName.find(m_current_pkgname) != std::string::npos) {
        enumName = enumName.substr(m_current_pkgname.size() + std::string(PACKAGE_SUFFIX).size());
      }
      auto enumType = LookUpType(ENUM_PREFIX_NAME + enumName);
      if (!enumType.IsValid()) {
        enumType = LookUpType(E0_PREFIX_NAME + enumName);
      }
      if (!enumType.IsValid()) {
        enumType = LookUpType(E2_PREFIX_NAME_OPTION_LIKE + enumName);
      }
      if (!enumType.IsValid()) {
        enumType = LookUpType(E3_PREFIX_NAME + enumName);
      }
      if (!enumType.IsValid()) {
        continue;
      }
      if (m_parsed_types.find(name) != m_parsed_types.end()) {
        auto var_type = m_parsed_types[name];
        if (var_type.GetTypeClass() != lldb::eTypeClassPointer ||
            !var_type.GetTypeName().GetStringRef().contains(E2_PREFIX_NAME_OPTION_LIKE)) {
          m_parsed_types[name] = enumType;
        }
      } else {
        m_parsed_types.insert({type.GetTypeName().AsCString(), enumType});
      }
      return enumType;
    }
    if (type.IsValid()) {
      ReplaceTypeDefWithInterfaceType(type);
      return type;
    }
  }

  return CompilerType();
}

OwnedPtr<Cangjie::AST::VarDecl> CangjieDeclMap::CreateVariable(const CompilerType& type,
                                                               std::string& name, Ptr<AST::Decl> pdecl)
{
  if (!pdecl) {
    pdecl = CreateTypeDecl(type);
  }
  auto vardecl = builder.CreateVarDecl(type, name, m_current_pkgname, pdecl);
  vardecl->isVar = !type.IsConst();
  auto var = FindParsedVarByName(ConstString(name));
  if (var) {
    // Add position info of vardecl for diag.
    auto line = var->GetDeclaration().GetLine();
    vardecl->begin = Position(m_file_id, line, 1);
    vardecl->identifier.SetPos(vardecl->begin,
        vardecl->begin + vardecl->identifier.Val().size());
    vardecl->end = vardecl->begin + Position(0, 1, 1);
  }
  return vardecl;
}

lldb_private::CompilerType CangjieDeclMap::GetDynamicTypeFromGenericTypeInfo(Ptr<AST::Ty> ty, std::string& demangled_name) {
  std::string genericDeclName = lldb_private::GetGenericDeclName(demangled_name);
  auto genericType = this->FindParsedTypesByName(genericDeclName);
  if (!genericType.IsValid()) {
    return CompilerType();
  }
  Log *log = GetLog(LLDBLog::Expressions);
  if (log) {
    LLDB_LOGF(log, " ----------- [Generic] start instantiated Generic CompilerType by AST::Ty ----------- \n");
  }
  return GetDynamicTypeFromTy(ty, demangled_name, genericType);
}

void CangjieDeclMap::SetMemberDeclParent(Ptr<AST::FuncDecl> funcdecl, Ptr<AST::Decl> decl) {
  funcdecl->outerDecl = decl;
  if (decl->astKind == ASTKind::CLASS_DECL) {
    funcdecl->funcBody->parentClassLike = RawStaticCast<AST::ClassDecl *>(decl);
  } else if (decl->astKind == ASTKind::INTERFACE_DECL) {
    funcdecl->funcBody->parentClassLike = RawStaticCast<AST::InterfaceDecl *>(decl);
  } else if (decl->astKind == ASTKind::STRUCT_DECL) {
    funcdecl->funcBody->parentStruct = RawStaticCast<AST::StructDecl *>(decl);
  } else if (decl->astKind == ASTKind::ENUM_DECL) {
    funcdecl->funcBody->parentEnum = RawStaticCast<AST::EnumDecl *>(decl);
  }
}

void CangjieDeclMap::CreateMemberFuncDecls(CompilerType& type, Ptr<AST::Decl> &decl, std::vector<OwnedPtr<Decl>>& members) {
  if (type.GetTypeName().GetStringRef().contains(LOCAL_FUNC_CAPTUREDVAR)) {
    return;
  }
  Log *log = GetLog(LLDBLog::Expressions);
  for (size_t i = 0; i < type.GetNumMemberFunctions(); i++) {
    TypeMemberFunctionImpl func = type.GetMemberFunctionAtIndex(i);
    if (func.IsValid()) {
      std::string memberFunc = func.GetName().AsCString();
      if (IsGenericTypeParameters(memberFunc)) {
        memberFunc = GetGenericDeclName(memberFunc);
      }
      if (memberFunc == CJDB_ADD_GENERIC_MEMBER_FUNC_NAME) {
        continue;
      }
      std::string func_type_str = GetFuncNameFromComplierType(func.GetType());
      if (log) {
        LLDB_LOGF(log, "add func [%s:%s] 's type %s to m_parsed_functions. \n", ConstString(decl->identifier.Val()).AsCString(),
                  ConstString(memberFunc).AsCString(), ConstString(func_type_str).AsCString());
      }
      m_parsed_functions.emplace_back(ParsedFunction(decl->identifier, memberFunc, func_type_str, func.GetType()));
      auto funcdecl = builder.CreateFuncDecl(func.GetType(), memberFunc, 1, decl, func.GetName().AsCString());
      if (func.GetKind() == lldb::eMemberFunctionKindStaticMethod) {
        funcdecl->modifiers.emplace(Modifier(TokenKind::STATIC, BEGIN_POSITION));
        funcdecl->EnableAttr(Attribute::STATIC);
      }
      funcdecl->fullPackageName = m_current_pkgname;
      SetMemberDeclParent(funcdecl, decl);
      members.emplace_back(std::move(funcdecl));
    }
  }
}

void CangjieDeclMap::CreateStaticVarDecls(
    CompilerType& type, Ptr<AST::Decl> &decl, std::vector<OwnedPtr<Decl>>& members) {
    if (!m_static_variables || m_static_variables->Empty()) {
      return;
    }
    // Check and add static variable
    std::string prefixname = SafeAsCString(type.GetTypeName()) + std::string("::");
    auto pos = prefixname.find("<");
    if (pos != std::string::npos) {
      // "default::Rect2<$G_T>""  ==> "default::Rect2<"
      prefixname = prefixname.substr(0, pos + 1);
    }
    auto varSize = m_static_variables->GetSize();
    for (size_t i = 0; i < varSize; i++) {
      auto var = m_static_variables->GetVariableAtIndex(i);
      std::string varname = SafeAsCString(var->GetName());
      if (varname.find(prefixname) != 0) {
        continue;
      }
      auto varPos = varname.rfind("::");
      if (varPos != std::string::npos) {
        varname = varname.substr(varPos + 2); // 2 is size of string "::"
      }
      auto vartype = var->GetType()->GetFullCompilerType();
      if (!vartype.IsValid()) {
        continue;
      }
      std::string vtypename = vartype.GetTypeName().AsCString();
      auto type_static = LookUpVariable(varname, VariableKind::StaticVariable);
      Ptr<Decl> target = CreateTypeDecl(type_static);
      if (!target) {
        target = decl;
      }
      // static variable of decl.
      auto vardecl = builder.CreateVarDecl(vartype, varname, m_current_pkgname, target);
      vardecl->modifiers.emplace(Modifier(TokenKind::STATIC, BEGIN_POSITION));
      vardecl->EnableAttr(Attribute::STATIC);
      vardecl->fullPackageName = m_current_pkgname;
      vardecl->outerDecl = decl;
      vardecl->mangledName = BaseMangler().Mangle(*vardecl);
      members.emplace_back(std::move(vardecl));
    }
}

void CangjieDeclMap::CreateMemberVarDecls(
    CompilerType& type, Ptr<AST::Decl> &decl, std::vector<OwnedPtr<Decl>>& members) {
  Log *log = GetLog(LLDBLog::Expressions);
  // Add all member variable.
  for (uint32_t i = 0; i < type.GetNumFields(); i++) {
    std::string member_name;
    auto child_type = type.GetFieldAtIndex(i, member_name, nullptr, nullptr, nullptr);
    if (member_name == "$ti" || member_name == "$ti*") {
      continue;
    }
    if (child_type.IsPointerType()) {
      child_type = child_type.GetPointeeType();
    }
    auto child_type_name = GetTypeNameWithoutPrefix(child_type.GetTypeName(), m_current_pkgname);
    Ptr<Decl> target = nullptr;
    if (m_parsed_types.find(child_type_name) == m_parsed_types.end()) {
      m_parsed_types.insert({child_type_name, child_type});
      target = CreateTypeDecl(child_type);
    }
    if (!target) {
      target = decl;
    }
    auto vardecl = CreateVariable(child_type, member_name, target);
    vardecl->DisableAttr(Attribute::GLOBAL);
    // If outDecl is genericDecl and field is generic type, set ref type here.
    if (log) {
      LLDB_LOGF(log, "[Generic] set member varDecl's param[%s.%s]. \n",
                decl->identifier.Val().c_str(), member_name.c_str());
    }
    vardecl->fullPackageName = m_current_pkgname;
    vardecl->outerDecl = decl;
    vardecl->mangledName = BaseMangler().Mangle(*vardecl);
    members.emplace_back(std::move(vardecl));
  }
}

void CangjieDeclMap::CreateMemberDecls(
    CompilerType type, Ptr<AST::Decl> &decl, std::vector<OwnedPtr<Decl>>& members) {
  CreateMemberVarDecls(type, decl, members);
  CreateStaticVarDecls(type, decl, members);
  CreateMemberFuncDecls(type, decl, members);
}

CompilerType CangjieDeclMap::GetSuperClassDefinedType(CompilerType& type) {
  for (size_t i = 0; i < type.GetNumMemberFunctions(); i++) {
    TypeMemberFunctionImpl func = type.GetMemberFunctionAtIndex(i);
    if (!func.IsValid()) {
      continue;
    }
    std::string memberFunc = func.GetName().AsCString();
    if (memberFunc == CJDB_ADD_GENERIC_MEMBER_FUNC_NAME) {
      auto superType = func.GetType().GetFunctionReturnType();
      return superType;
    }
  }
 return CompilerType();
}

void CangjieDeclMap::SetGenericDeclBySubclass(Ptr<AST::RefType> refType, std::string name,
                                              Ptr<AST::InheritableDecl>& subclass) {
  if (!subclass->generic) {
    return;
  }
  name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
  if (name.find(INTERFACE_PREFIX_NAME) != std::string::npos) {
    name = DeletePrefixOfType(name);
  }
  auto idLen = refType->ref.identifier.Val().size();
  if (name.size() < idLen + 2) {  // 2 is size of string "<>"
    return;
  }
  auto args = name.substr(idLen + 1, name.size() - idLen - 2);
  auto elements = builder.SplitTypeName(args);
  // The generic type parameter of the superclass comes from the subclass or instancetiated.
  // for example, class B<T, K> <: A<T, Int64>.
  for (auto& ele:elements) {
    bool isFromSubclass = false;
    for (auto& param:subclass->generic->typeParameters) {
      if (param->identifier.Val() == GetGenericParamDeclName(ele)) {
        refType->typeArguments.emplace_back(builder.CreateAstType(AstTypeInfo(ele), param.get(), m_current_pkgname));
        isFromSubclass = true;
        break;
      }
    }
    if (!isFromSubclass) {
      refType->typeArguments.emplace_back(builder.CreateAstType(AstTypeInfo(ele), nullptr, m_current_pkgname));
    }
  }
}

void CangjieDeclMap::ReplaceTypeDefWithInterfaceType(CompilerType& type) {
  bool isTypeDef = type.IsValid() && type.GetTypeClass() == lldb::eTypeClassTypedef;
  if (!isTypeDef) {
    return;
  }
  auto underlyingType = type.GetTypedefedType();
  if (IsInterfaceType(underlyingType)) {
    Log *log = GetLog(LLDBLog::Expressions);
    LLDB_LOGF(log, "[Generic] replace typedef [%s] with inteface[%s]. \n",
              type.GetTypeName().AsCString(), underlyingType.GetTypeName().AsCString());
    type = underlyingType;
  }
}

void CangjieDeclMap::CreateClassLikeParentDecls(CompilerType type, Ptr<AST::InheritableDecl> id) {
  Log *log = GetLog(LLDBLog::Expressions);
  for (size_t i = 0; i < type.GetNumDirectBaseClasses(); i++) {
    auto superType = type.GetDirectBaseClassAtIndex(i, nullptr);
    ReplaceTypeDefWithInterfaceType(superType);
    std::string superName = SafeAsCString(superType.GetTypeName());
    if (!superType.IsValid()) {
      continue;
    }
    // The generic parameters of a certain class may differ from those of its superclass.
    // For example, interface I2<T> {}  class A<U> <: I2<U> {}
    // A's superType is type I2<U>. And the return type of the A's member function $GetGenericDef is type I2<T>.
    auto definedType = GetSuperClassDefinedType(superType);
    ReplaceTypeDefWithInterfaceType(definedType);
    if (definedType.IsValid()) {
      if (log) {
        LLDB_LOGF(log, "[Generic] replace parent type [%s] -> %s . \n",
                  superType.GetTypeName().AsCString(), definedType.GetTypeName().AsCString());
      }
      superType = definedType;
    }
    // If superType have memberFunc called $GetGenericDef, real type should be replaced.
    // Create the parent class.
    Ptr<Decl> target = nullptr;
    target = CreateTypeDecl(superType);
    if (superName == "Object" || superName == "std.core::Object") {
      id->inheritedTypes.emplace_back(builder.CreateAstType(AstTypeInfo("std.core::Object"),
                                                            target, m_current_pkgname));
    } else if (superName.find(GENERIC_TYPE_PREFIX_NAME) == std::string::npos) {
      // The generic type parameter of the superclass not comes from the subclass.
      // For example, class B <: A<Int64>, class B<T> <: A<Int64>
      id->inheritedTypes.emplace_back(builder.CreateAstType(AstTypeInfo(superName), target, m_current_pkgname));
    } else {
      // The generic type parameter of the superclass comes from the subclass.
      // For example, class B<T> <: A<T>, class B<T,K> <: A<T,Int64>.
      OwnedPtr<AST::Type> refType = builder.CreateAstType(AstTypeInfo(superName), target, m_current_pkgname, false);
      auto rt = RawStaticCast<AST::RefType *>(refType.get());
      SetGenericDeclBySubclass(rt, superName, id);
      id->inheritedTypes.emplace_back(std::move(refType));
    }
  }
}

bool CangjieDeclMap::IsGenericTypeWithConstraints(CompilerType& type) {
  if (type.GetTypeClass() != lldb::eTypeClassStruct) {
    return false;
  }
  std::string name = type.GetTypeName().AsCString();
  if (name.find(GENERIC_TYPE_PREFIX_NAME) != 0) {
    return false;
  }
  if (type.GetNumDirectBaseClasses() > 0) {
    auto str = type.GetDirectBaseClassAtIndex(0, nullptr).GetTypeName();
    if (str == "Equatable<$G_T>" || str == "Hashable") {
      return false;
    }
    return true;
  }
  return false;
}

std::set<CompilerType> CangjieDeclMap::GetGenericTypeWithConstraints(CompilerType& type) {
  std::set<CompilerType> types;
  for (uint32_t i = 0; i < type.GetNumFields(); i++) {
    std::string member_name;
    auto child_type = type.GetFieldAtIndex(i, member_name, nullptr, nullptr, nullptr);
    if (IsGenericTypeWithConstraints(child_type)) {
      types.insert(child_type);
    }
  }
  for (size_t i = 0; i < type.GetNumMemberFunctions(); i++) {
    TypeMemberFunctionImpl func = type.GetMemberFunctionAtIndex(i);
    if (func.GetName() == "$GenericVirtualFunc") {
      continue;
    }
    auto funcType = func.GetType();
    for (auto j = 0; j < funcType.GetFunctionArgumentCount(); j++) {
      auto argType = funcType.GetFunctionArgumentAtIndex(j);
      if (IsGenericTypeWithConstraints(argType)) {
        types.insert(argType);
      }
    }
  }
  return types;
}

OwnedPtr<GenericConstraint> CangjieDeclMap::CreateGenericConstraintsDecl(Ptr<AST::Decl> &decl, CompilerType& type) {
  auto gc = MakeOwned<GenericConstraint>();
  OwnedPtr<RefType> t = MakeOwned<RefType>();
  std::string gn = type.GetTypeName().AsCString();
  if (GENERIC_TYPE_PREFIX_NAME.size() > gn.size()) {
    t->ref.identifier = gn;
  } else {
    t->ref.identifier = gn.substr(GENERIC_TYPE_PREFIX_NAME.size(), gn.size());
  }
  for (auto& gen:decl->generic->typeParameters) {
    if (gen->identifier.Val() == t->ref.identifier.Val()) {
      t->ref.target = gen;
    }
    break;
  }
  gc->type = std::move(t);
  for (size_t i = 0; i < type.GetNumDirectBaseClasses(); i++) {
    auto superType = type.GetDirectBaseClassAtIndex(i, nullptr);
    ReplaceTypeDefWithInterfaceType(superType);
    if (!superType.IsValid()) {
      continue;
    }
    Ptr<AST::Decl> decltmp = CreateTypeDecl(superType);
    std::string up = DeletePrefixOfType(superType.GetTypeName().AsCString());
    OwnedPtr<AST::Type> ctype = builder.CreateAstType(AstTypeInfo(up), decltmp, m_current_pkgname);

    gc->upperBounds.emplace_back(std::move(ctype));
    std::string superName = SafeAsCString(superType.GetTypeName());
  }
  return gc;
}

void CangjieDeclMap::CreateGenericConstraints(Ptr<AST::Decl> &decl, CompilerType& type) {
  std::set<CompilerType> types;
  if (type.GetTypeClass() == lldb::eTypeClassClass || type.GetTypeClass() == lldb::eTypeClassStruct) {
    types = GetGenericTypeWithConstraints(type);
  }
  for (auto type:types) {
    auto constraint = CreateGenericConstraintsDecl(decl, type);
    decl->generic->genericConstraints.emplace_back(std::move(constraint));
  }
}

void CangjieDeclMap::CreateClassMemberDecls(CompilerType type, Ptr<AST::Decl> &decl) {
  if (decl == nullptr || !type.IsValid()) {
    return;
  }
  auto cd = RawStaticCast<AST::ClassDecl *>(decl);
  if (!cd->body->decls.empty()) {
    return;
  }
  CreateClassLikeParentDecls(type, cd);
  CreateMemberDecls(type, decl, cd->body->decls);
}

Ptr<AST::Decl> CangjieDeclMap::CreateClassDecl(CompilerType type, std::string prefix) {
  auto name = GetTypeNameWithoutPrefix(type.GetTypeName(), prefix);
  if (lldb_private::IsGenericTypeParameters(name)) {
    name = GetGenericDeclName(name);
  }
  auto decl = MakeOwned<AST::ClassDecl>();
  decl->body = MakeOwned<ClassBody>();
  decl->identifier = name;
  decl->fullPackageName = m_current_pkgname;
  decl->mangledName = BaseMangler().Mangle(*decl);
  std::string typeName = SafeAsCString(type.GetTypeName());
  builder.CreateDeclGeneric(decl.get(), typeName);
  Ptr<AST::Decl> cla = RawStaticCast<AST::Decl *>(decl.get());
  CreateGenericConstraints(cla, type);
  decl->EnableAttr(Attribute::PUBLIC, Attribute::GLOBAL);
  decl->EnableAttr(Attribute::OPEN);
  auto target = GetSameGlobalDecl(std::move(decl), true);
  CreateClassMemberDecls(type, target);
  return target;
}

void CangjieDeclMap::CreateInterfaceMemberDecls(Ptr<AST::Decl> &decl, CompilerType type) {
  if (decl == nullptr || !type.IsValid()) {
    return;
  }
  auto id = RawStaticCast<AST::InterfaceDecl *>(decl);
  if (!id->body->decls.empty()) {
    return;
  }
  CreateClassLikeParentDecls(type, id);
  CreateMemberDecls(type, decl, id->body->decls);
}

void CangjieDeclMap::CreateStructMemberDecls(CompilerType type, Ptr<AST::Decl> &decl) {
  auto sd = RawStaticCast<AST::StructDecl *>(decl);
  if (!sd->body->decls.empty()) {
    return;
  }
  CreateClassLikeParentDecls(type, sd);
  CreateMemberDecls(type, decl, sd->body->decls);
}

Ptr<AST::Decl> CangjieDeclMap::CreateEnumDecl(CompilerType type, std::string prefix) {
  if (type.IsPointerType()) {
    type = type.GetPointeeType();
  }
  auto decl = builder.CreateEnumDecl(type);
  std::string typeName = SafeAsCString(type.GetTypeName());
  builder.CreateDeclGeneric(decl.get(), typeName);
  CreateEnumConstructors(decl, type);
  auto eDecl = Ptr(RawStaticCast<AST::Decl *>(decl.get()));
  CreateMemberFuncDecls(type, eDecl, decl->members);
  return GetSameGlobalDecl(std::move(decl));
}

Ptr<AST::Decl> CangjieDeclMap::CreateStructDecl(CompilerType type, std::string prefix) {
  auto name = GetTypeNameWithoutPrefix(type.GetTypeName(), prefix);
  auto decl = MakeOwned<AST::StructDecl>();
  decl->body = MakeOwned<StructBody>();
  decl->identifier = name;
  decl->fullPackageName = m_current_pkgname;
  decl->mangledName = BaseMangler().Mangle(*decl);
  decl->EnableAttr(Attribute::PUBLIC, Attribute::GLOBAL);
  std::string typeName = SafeAsCString(type.GetTypeName());
  builder.CreateDeclGeneric(decl.get(), typeName);
  Ptr<AST::Decl> cla = RawStaticCast<AST::Decl *>(decl.get());
  CreateGenericConstraints(cla, type);
  auto target = GetSameGlobalDecl(std::move(decl), true);
  CreateStructMemberDecls(type, target);
  return target;
}

Ptr<AST::Decl> CangjieDeclMap::CreateInterfaceDecl(CompilerType type, std::string prefix) {
  auto name = GetTypeNameWithoutPrefix(type.GetTypeName(), prefix);
  auto decl = MakeOwned<AST::InterfaceDecl>();
  decl->body = MakeOwned<InterfaceBody>();
  decl->identifier = name;
  decl->fullPackageName = m_current_pkgname;
  decl->mangledName = BaseMangler().Mangle(*decl);
  decl->EnableAttr(Attribute::PUBLIC, Attribute::GLOBAL);
  std::string typeName = SafeAsCString(type.GetTypeName());
  typeName = DeletePrefixOfType(typeName);
  builder.CreateDeclGeneric(decl.get(), typeName);
  auto target = GetSameGlobalDecl(std::move(decl), true);
  CreateInterfaceMemberDecls(target, type);
  return target;
}

bool CangjieDeclMap::IsInstantiatedOfGenericDecl(std::string typeName) {
  auto declName = ConstString(GetGenericDeclName(typeName));
  if (m_generic_types.find(declName) != m_generic_types.end()) {
    return true;
  }
  return false;
}

CompilerType CangjieDeclMap::CreateOrGetGenericType(const CompilerType& type)
{
  Log *log = GetLog(LLDBLog::Expressions);
  std::string refTypeName = type.GetTypeName().AsCString();
  auto declName = ConstString(GetGenericDeclName(refTypeName));
  if (m_generic_types.find(declName) != m_generic_types.end()) {
    return m_generic_types[declName];
  }
  std::string name = declName.AsCString();
  auto userDefinedType = GetGenericTypeByPartName(name);
  if (userDefinedType.IsValid()) {
    LLDB_LOGF(log, "[Generic]: get generic type [%s] from variable[%s]. \n",
              userDefinedType.GetTypeName().GetCString(), ConstString(declName).AsCString());
    m_generic_types.insert({ConstString(declName), userDefinedType});
  }
  return userDefinedType;
}

Ptr<AST::Decl> CangjieDeclMap::CreateTypeDecl(CompilerType type) {
  if (!type.IsValid()) {
    return nullptr;
  }
  std::string typeName = SafeAsCString(type.GetTypeName());
  if (CangjieASTBuiler::IsFunctionType(ConstString(typeName.c_str()))) {
    return nullptr;
  }
  auto subName = GetSubNameWithoutPkgname(typeName, m_current_pkgname);
  if (subName.empty()) {
    // If the type is not from the current package, no need to create decl.
    if (typeName.find("::") < typeName.find("<")) {
      builder.m_type_names.insert(typeName);
      typeName = DeletePrefixOfType(typeName);
      m_parsed_types.insert({typeName, type});
    }
    return nullptr;
  }
  // If it is the instantiated type of a generic, generic type is needed here.
  if (lldb_private::IsGenericInstanced(typeName)) {
    type = CreateOrGetGenericType(type);
  }
  if (type.GetTypeClass() == lldb::eTypeClassEnumeration || IsEnumName(typeName)) {
    return CreateEnumDecl(type, m_current_pkgname);
  }
  if (type.GetTypeClass() == lldb::eTypeClassStruct) {
    return CreateStructDecl(type, m_current_pkgname);
  }
  if (IsInterfaceType(type)) {
    return CreateInterfaceDecl(type, m_current_pkgname);
  }
  return CreateClassDecl(type, m_current_pkgname);
}

void CangjieDeclMap::CreateInjectedSuper(Ptr<AST::Decl>& thisdecl)
{
  // Add $__lldb_injected_super: Object
  auto supervar = MakeOwned<AST::VarDecl>();
  supervar->identifier = INJECTED_SUPER_NAME;
  supervar->EnableAttr(Attribute::PUBLIC, Attribute::GLOBAL);
  supervar->isVar = true;
  auto thiscd = RawStaticCast<AST::ClassDecl *>(thisdecl);
  for (auto& type : thiscd->inheritedTypes) {
    supervar->type = ASTCloner::Clone<AST::Type>(type.get());
  }
  supervar->fullPackageName = "default";
  supervar->mangledName = BaseMangler().Mangle(*supervar);
  m_global_decls.emplace_back(std::move(supervar));
}

void CangjieDeclMap::CreateInjectedThis()
{
  m_parsed_vars.insert({ConstString("this"), m_this_variable->GetVariable()});
  std::string name = SafeAsCString(m_this_variable->GetTypeName());
  CompilerType realType = m_this_variable->GetCompilerType();
  if (IsGenericTypeParameters(name)) {
    realType = m_this_variable->GetDynamicType();
    Log *log = GetLog(LLDBLog::Expressions);
    if (log) {
      LLDB_LOGF(log, "[Generic] this's dynamic type [%s] -> %s . \n",
                ConstString(name).AsCString(), realType.GetTypeName().AsCString());
    }
    CollectGenericTypeOfVariable(realType);
  }
  auto decl = CreateTypeDecl(realType);
  if (!decl) {
    return;
  }
  // Add __lldb_injected_self: A
  auto thisvar = builder.CreateVarDecl(realType, "__lldb_injected_self", "default", decl);
  thisvar->fullPackageName = "default";
  thisvar->mangledName = BaseMangler().Mangle(*thisvar);
  if (decl->astKind == ASTKind::CLASS_DECL) {
    CreateInjectedSuper(decl);
  }
  m_global_decls.emplace_back(std::move(thisvar));
}

void CangjieDeclMap::InitVariable() {
  StackFrame *frame = m_exe_ctx.GetFramePtr();
  if (!frame) {
    return;
  }
  SymbolContext sym_ctx = frame->GetSymbolContext(lldb::eSymbolContextCompUnit);
  if (sym_ctx.comp_unit) {
    // Get current package name
    m_current_pkgname = GetPkgNameFromCompilerUnit(*sym_ctx.comp_unit);
    builder.m_current_pkgname = m_current_pkgname;
  }
  sym_ctx = frame->GetSymbolContext(lldb::eSymbolContextFunction | lldb::eSymbolContextBlock);
  if (sym_ctx.block) {
    CompilerDeclContext frame_decl_context = sym_ctx.block->GetDeclContext();
    if (frame_decl_context) {
      m_type_system = llvm::dyn_cast_or_null<TypeSystemClang>(frame_decl_context.GetTypeSystem());
    }
  }
  if (m_type_system == nullptr) {
    m_type_system = ScratchTypeSystemClang::GetForTarget(*m_exe_ctx.GetTargetPtr(),
      ScratchTypeSystemClang::DefaultAST, false);
  }

  builder.m_file_id = m_file_id;
  // Find this variable if in the context of a class.
  m_this_variable = frame->FindVariable(ConstString("this"));
  if (m_this_variable) {
    CreateInjectedThis();
  }
  m_local_variables = frame->GetInScopeVariableList(false);
  m_static_variables = frame->GetInScopeVariableList(true);
  auto svarSize = m_static_variables->GetSize();
  for (size_t i = svarSize - 1; i >= 0 && i < svarSize; i--) {
    auto svar = m_static_variables->GetVariableAtIndex(i);
    if (svar->GetScope() != lldb::eValueTypeVariableStatic) {
      m_static_variables->RemoveVariableAtIndex(i);
    }
  }
  m_global_variables = frame->GetInScopeVariableList(true);
  auto varSize = m_global_variables->GetSize();
  if (varSize == 0) {
    return;
  }
  // Check and delete local variable
  for (size_t i = varSize - 1; i >= 0 && i < varSize; i--) {
    auto var = m_global_variables->GetVariableAtIndex(i);
    if (var->GetScope() != lldb::eValueTypeVariableGlobal) {
      m_global_variables->RemoveVariableAtIndex(i);
    }
  }
}

Ptr<Decl> CangjieDeclMap::GetSameGlobalDecl(OwnedPtr<AST::Decl> decl, bool insert_begin)
{
  for (auto& d : m_global_decls) {
    if (d->mangledName == decl->mangledName) {
      return d.get();
    }
  }
  auto ret = decl.get();
  if (insert_begin) {
    m_global_decls.emplace(m_global_decls.begin(), std::move(decl));
  } else {
    m_global_decls.emplace_back(std::move(decl));
  }
  return ret;
}

bool CangjieDeclMap::AddPackageNameSpace(ConstString name) {
  if (m_parsed_packages.find(name) != m_parsed_packages.end()) {
    return true;
  }
  return LookUpPackage(name);
}

std::vector<ConstString> CangjieDeclMap::GetEnumeratorName(CompilerType& enumType) {
  lldb_private::TypeEnumMemberListImpl enum_member_list;
  enumType.ForEachEnumerator([&enum_member_list](
                                  const CompilerType &integer_type,
                                  ConstString name,
                                  const llvm::APSInt &value) -> bool {
    auto enum_member = lldb::TypeEnumMemberImplSP(
        new TypeEnumMemberImpl(lldb::TypeImplSP(new TypeImpl(integer_type)), name, value));
    enum_member_list.Append(enum_member);
    return true;
  });
  std::vector<ConstString> enumCtorNameList;
  for (size_t i = 0; i < enum_member_list.GetSize(); i++) {
    lldb::TypeEnumMemberImplSP temp = enum_member_list.GetTypeEnumMemberAtIndex(i);
    enumCtorNameList.emplace_back(temp->GetName());
  }
  return enumCtorNameList;
}

CompilerType CangjieDeclMap::GetEnumerationType(const CompilerType& type, bool hasArgs) {
  CompilerType enumType;
  auto typeName = type.GetTypeName().GetStringRef();
  if (!hasArgs || typeName.contains(E2_PREFIX_NAME_OPTION_LIKE)) {
    for (uint32_t i = 0; i < type.GetNumFields(); i++) {
      std::string member_name;
      auto tmpType = type.GetFieldAtIndex(i, member_name, nullptr, nullptr, nullptr);
      if (member_name == "constructor" || member_name == "constructor_R") {
        enumType = tmpType;
        break;
      }
    }
  } else {
    CompilerType ctorFuncType = type.GetDirectBaseClassAtIndex(0, nullptr);
    std::string member_name;
    ctorFuncType = ctorFuncType.GetFieldAtIndex(0, member_name, nullptr, nullptr, nullptr);
    if (type.GetTypeName().GetStringRef().contains(E3_PREFIX_NAME) ||
        type.GetTypeName().GetStringRef().contains(ENUM_PREFIX_NAME)) {
      CJC_ASSERT(member_name == "constructor");
      return ctorFuncType;
    }
    if (ctorFuncType.IsPointerType()) {
      ctorFuncType = ctorFuncType.GetPointeeType();
    }
    enumType = ctorFuncType.GetFieldAtIndex(0, member_name, nullptr, nullptr, nullptr);
  }
  return enumType;
}

void CangjieDeclMap::CreateEnumOptionLikeCtors(OwnedPtr<AST::EnumDecl> &decl, const CompilerType& type,
                                               std::vector<ConstString>& enumCtorNameList) {
  for (size_t i = 0; i < enumCtorNameList.size(); i++) {
    std::string ctorName = enumCtorNameList[i].AsCString();
    if (ctorName.find(E2_CTOR_PREFIX_NAME) == std::string::npos) {
      std::string member_name;
      // Val is arg1
      auto argType = type.GetFieldAtIndex(1, member_name, nullptr, nullptr, nullptr);
      auto funcDecl = builder.CreateEnumFuncDecl(type, argType, ctorName, decl);
      funcDecl->outerDecl = decl;
      funcDecl->EnableAttr(AST::Attribute::ENUM_CONSTRUCTOR);
      auto curFunc = Ptr(funcDecl.get());
      SetMemberDeclParent(curFunc, decl);
      decl->constructors.emplace_back(std::move(funcDecl));
    } else {
      ctorName = ctorName.substr(E2_CTOR_PREFIX_NAME.size(), ctorName.size());
      auto vardecl = builder.CreateEnumVarDecl(type, ctorName, m_current_pkgname, decl);
      vardecl->fullPackageName = m_current_pkgname;
      vardecl->outerDecl = decl.get();
      vardecl->mangledName = BaseMangler().Mangle(*vardecl);
      vardecl->EnableAttr(AST::Attribute::ENUM_CONSTRUCTOR);
      decl->constructors.emplace_back(std::move(vardecl));
    }
  }
}

void CangjieDeclMap::CreateEnumConstructors(OwnedPtr<AST::EnumDecl> &decl, const CompilerType& type) {
  // For one cangjie enumDecl with param, debugInfo save two types.
  // for example,  enum RGBColor { Red | Green | Blue(UInt8)} have enum type and struct type.
  // DW_TAG_enumeration_type Ctor-RGBColor with member Red,Green,Blue.
  // DW_TAG_structure_type Enum$RGBColor with RGBColor_ctor_0, RGBColor_ctor_1, RGBColor_ctor_2.
  // For one cangjie enumDecl without param, debugInfo save two types.
  // for example,  enum RGBColor { Red | Green} have enum type RGBColor.
  // step1: find enum type Ctor-RGBColor.
  // for example, enum RGBColor's type name is Enum$RGBColor, subName is RGBColor.
  std::string typeName = SafeAsCString(type.GetTypeName());
  EnumLayout kind = GetEnumLayout(typeName);
  // for enum type
  decl->hasArguments = (kind != EnumLayout::E0Int);
  // for an enum type with args, like RGBColor {Red(Int64)}, enumName is default::Enum$RGBColor
  // for an enum type without args, like RGBColor {Red}, enumName is default::RGBColor, RGBColor is needed.
  AddPackageNameSpace(ConstString(m_current_pkgname.c_str()));

  CompilerType enumType = GetEnumerationType(type, decl->hasArguments);
  std::vector<ConstString> enumCtorNameList = GetEnumeratorName(enumType);
  if (kind == EnumLayout::E2OptionLike) {
    CreateEnumOptionLikeCtors(decl, type, enumCtorNameList);
    return;
  } else if (kind == EnumLayout::E0Int) {
    for (size_t i = 0; i < enumCtorNameList.size(); i++) {
        auto vardecl = builder.CreateEnumVarDecl(type, enumCtorNameList[i].AsCString(), m_current_pkgname, decl);
        vardecl->fullPackageName = m_current_pkgname;
        vardecl->outerDecl = decl.get();
        vardecl->mangledName = BaseMangler().Mangle(*vardecl);
        vardecl->EnableAttr(AST::Attribute::ENUM_CONSTRUCTOR);
        decl->constructors.emplace_back(std::move(vardecl));
    }
    return;
  }
  // for structure type
  // VarDecl Red {*Ty: Enum-RGBColor have no type and no initializer}.
  for (size_t i = 0; i < type.GetNumDirectBaseClasses(); i++) {
      CompilerType ctorFuncType = type.GetDirectBaseClassAtIndex(i, nullptr);
      ReplaceTypeDefWithInterfaceType(ctorFuncType);
      if (IsInterfaceType(ctorFuncType)) {
        CreateInterfaceDecl(ctorFuncType, m_current_pkgname);
        continue;
      }
      std::string member_name;
      if (!type.GetTypeName().GetStringRef().contains(E3_PREFIX_NAME) &&
          !type.GetTypeName().GetStringRef().contains(ENUM_PREFIX_NAME)) {
        ctorFuncType = ctorFuncType.GetFieldAtIndex(0, member_name, nullptr, nullptr, nullptr);
      }
      if (ctorFuncType.IsPointerType()) {
        ctorFuncType = ctorFuncType.GetPointeeType();
      }
      if (ctorFuncType.GetNumFields() == 1) {
        // for struct type RGBColor_ctor_0, VarDecl have only one filed.
        auto vardecl = builder.CreateEnumVarDecl(type, enumCtorNameList[i].AsCString(), m_current_pkgname, decl);
        vardecl->fullPackageName = m_current_pkgname;
        vardecl->outerDecl = decl.get();
        vardecl->mangledName = BaseMangler().Mangle(*vardecl);
        vardecl->EnableAttr(AST::Attribute::ENUM_CONSTRUCTOR);
        vardecl->outerDecl = decl;
        decl->constructors.emplace_back(std::move(vardecl));
      } else {
        std::string funcName = enumCtorNameList[i].AsCString();
        auto funcDecl = builder.CreateEnumFuncDecl(type, ctorFuncType, funcName, decl);
        funcDecl->outerDecl = decl;
        funcDecl->EnableAttr(AST::Attribute::ENUM_CONSTRUCTOR);
        auto curFunc = Ptr(funcDecl.get());
        SetMemberDeclParent(curFunc, decl);
        decl->constructors.emplace_back(std::move(funcDecl));
      }
  }
}

bool CangjieDeclMap::IsLocalConstVariable(const std::string& mangledName)
{
  auto name = Cangjie::Demangle(mangledName).GetFullName();
  if (name.find("::") != std::string::npos) {
    return true;
  }
  return false;
}

void CangjieDeclMap::CreateVarDeclByType(const CompilerTypeInfo& typeInfo, std::string name)
{
  auto type = typeInfo.Type;
  bool find_global = typeInfo.Global;
  auto vardecl = CreateVariable(type, name);
  vardecl->isVar = !typeInfo.IsLet;
  if (IsLocalConstVariable(typeInfo.mangle_name)) {
    // Because of local const variables are promoted to global variables in the CHIR stage,
    // special handling is required here.
    find_global = false;
  }
  if (Cangjie::Demangle(typeInfo.mangle_name).IsPrivateDeclaration()) {
    vardecl->EnableAttr(Attribute::PRIVATE);
    vardecl->begin = Position(0, vardecl->begin.line, 1);
    vardecl->end = vardecl->begin + Position(0, 1, 1);
  }
  if (find_global) {
    vardecl->fullPackageName = m_current_pkgname;
    vardecl->mangledName = BaseMangler().Mangle(*vardecl);
    GetSameGlobalDecl(std::move(vardecl));
  } else {
    vardecl->fullPackageName = LOCAL_PACKAGE_NAME;
    vardecl->mangledName = BaseMangler().Mangle(*vardecl);
    for (auto& d : m_local_decls) {
      if (d->mangledName == vardecl->mangledName) {
        return;
      }
    }
    m_local_decls.emplace_back(std::move(vardecl));
  }
}

void CangjieDeclMap::CreateTypeDeclByType(const CompilerType& type)
{
  CreateTypeDecl(type);
}

void CangjieDeclMap::CreateFuncDeclByType(const CompilerTypeInfo& ft, std::string name, bool find_global)
{
  auto funcdecl = builder.CreateFuncDecl(ft.Type, name, ft.Line, nullptr, ft.genericName);
  if (find_global) {
    if (m_local_func.find(ft.Type.GetTypeName()) != m_local_func.end()) {
      return;
    }
    // Create global funcdecl.
    funcdecl->fullPackageName = m_current_pkgname;
    m_global_decls.emplace_back(std::move(funcdecl));
  } else {
    // Create local funcdecl.
    funcdecl->fullPackageName = LOCAL_PACKAGE_NAME;
    funcdecl->mangledName = ft.mangle_name;
    m_local_decls.emplace_back(std::move(funcdecl));
    m_local_func.insert(ft.Type.GetTypeName());
  }
}

void CangjieDeclMap::CreatePackage(const std::string &name) {
  if (m_parsed_packages.find(ConstString(name.c_str())) == m_parsed_packages.end()) {
    return;
  }
  auto namespace_map = m_parsed_packages[ConstString(name.c_str())];
  for (const auto &element : m_identifys) {
    for (auto it = namespace_map->begin(); it != namespace_map->end(); ++it) {
      // Lookup global var first.
      auto type = LookUpGlobalVariable(it->first, ConstString(element.c_str()), it->second);
      if (type.IsValid()) {
        auto vardecl = builder.CreateVarDecl(type, element, name);
        // Check and add variable type of decl.
        if (vardecl != nullptr) {
          vardecl->fullPackageName = name;
          vardecl->mangledName = BaseMangler().Mangle(*vardecl);
          m_global_decls.emplace_back(std::move(vardecl));
          continue;
        }
      }
      // lookup function
      auto funcs = LookUpFunction(it->first, ConstString(element.c_str()), it->second);
      // look up type
      type = LookUptype(it->first, ConstString(element.c_str()), it->second);
      if (type.IsValid()) {
        auto typedecl = CreateTypeDecl(type);
        if (typedecl != nullptr) {
          typedecl->identifier = element;
          continue;
        }
      }
    }
  }
  return;
}

void CangjieDeclMap::AddImportSpec(OwnedPtr<Cangjie::AST::File>& file, std::string curPkgName)
{
  for (auto& fullpkg : m_fullpkgs) {
    if (curPkgName == fullpkg) {
      continue;
    }
    auto import = MakeOwned<ImportSpec>();
    auto pos = fullpkg.find(":");
    if (pos != std::string::npos) {
      // fullpkg: "org:a"
      import->content.hasDoubleColon = true;
      import->content.prefixPaths.emplace_back(fullpkg.substr(0, pos));
      std::vector<std::string> paths = Utils::SplitQualifiedName(fullpkg.substr(pos + 1));
      for (auto path : paths) {
        import->content.prefixPaths.emplace_back(path);
      }
    } else {
      import->content.prefixPaths = Utils::SplitQualifiedName(fullpkg);
    }
    const size_t prefixLen = import->content.prefixPaths.size();
    import->content.prefixPoses.resize(prefixLen);
    import->content.prefixDotPoses.resize(prefixLen);
    import->content.identifier = "*";
    import->content.kind = ImportKind::IMPORT_ALL;
    import->EnableAttr(Attribute::IMPLICIT_ADD, Attribute::COMPILER_ADD);
    import->curFile = file.get();
    file->imports.emplace_back(std::move(import));
  }
  return;
}

void CangjieDeclMap::CollectImportPkgName()
{
  std::for_each(builder.m_type_names.begin(), builder.m_type_names.end(), [&](auto& name) {
    auto pkgname = name;
    auto pos = pkgname.find("<");
    if (pos != std::string::npos) {
      pkgname = pkgname.substr(0, pos);
    }
    pos = pkgname.find("(");
    if (pos != std::string::npos) {
      pkgname = pkgname.substr(0, pos);
    }
    pos = pkgname.rfind(PACKAGE_SUFFIX);
    if (pos == std::string::npos) {
      return;
    }
    pkgname = pkgname.substr(0, pos);
    if (pkgname.empty() || pkgname == "std") {
      return;
    }
    pos = pkgname.find(PACKAGE_SUFFIX);
    if (pos != std::string::npos) {
      // "org::a" -> "org:a"
      pkgname.replace(pos, 1, "");
    }
    pos = pkgname.find("/");
    if (pos != std::string::npos) {
      // "org/a" -> "org:a"
      pkgname.replace(pos, 1, ":");
    }
    m_fullpkgs.insert(pkgname);
    Log *log = GetLog(LLDBLog::Expressions);
    if (log) {
      LLDB_LOGF(log, "collect import package [%s]. \n", ConstString(pkgname).AsCString());
    }
  });
}

std::vector<OwnedPtr<AST::Package>> CangjieDeclMap::CreateFakePackage() {
  std::vector<OwnedPtr<AST::Package>> packages;
  // Collect other package names from typenames for import.
  CollectImportPkgName();
  if (!m_captured_vars.empty()) {
    auto file = MakeOwned<AST::File>();
    file->fileName = std::string("$CapturedVars.cj");
    file->decls = std::move(m_captured_vars);
    auto package = MakeOwned<AST::Package>(LOCAL_FUNC_CAPTUREDVAR);
    package->EnableAttr(Attribute::IMPORTED);
    package->files.emplace_back(std::move(file));
    packages.push_back(std::move(package));
  }
  if (!m_local_decls.empty()) {
    auto file = MakeOwned<AST::File>();
    file->fileName = std::string("locals.cj");
    AddImportSpec(file, LOCAL_PACKAGE_NAME);
    file->decls = std::move(m_local_decls);
    auto package = MakeOwned<AST::Package>(LOCAL_PACKAGE_NAME);
    package->EnableAttr(Attribute::IMPORTED);
    package->files.emplace_back(std::move(file));
    packages.push_back(std::move(package));
  }
  auto file = MakeOwned<AST::File>();
  file->fileName = m_current_filename;
  AddImportSpec(file, m_current_pkgname);
  for (auto& tmpDecl:m_global_decls) {
    if (tmpDecl->TestAttr(Attribute::PRIVATE)) {
      file->begin = tmpDecl->begin;
      tmpDecl->curFile = file.get();
    }
  }
  file->decls = std::move(m_global_decls);
  auto package = MakeOwned<AST::Package>(!m_current_pkgname.empty() ? m_current_pkgname : "default");
  package->EnableAttr(Attribute::IMPORTED);
  package->files.emplace_back(std::move(file));
  packages.push_back(std::move(package));
  return packages;
}

void CangjieDeclMap::CollectGenericTypeOfVariable(const CompilerType& type)   {
  Log *log = GetLog(LLDBLog::Expressions);
  if (!type.IsValid() || type.GetTypeClass() == lldb::eTypeClassEnumeration) {
    return;
  }
  std::string fullName = SafeAsCString(type.GetTypeName());
  if (!lldb_private::IsGenericInstanced(fullName)) {
    return;
  }
  auto name = lldb_private::GetGenericDeclName(fullName);
  auto userDefinedType = GetGenericTypeByPartName(name);
  if (userDefinedType.IsValid()) {
    LLDB_LOGF(log, "[Generic]: get generic type [%s] from variable[%s]. \n",
              userDefinedType.GetTypeName().GetCString(), ConstString(name).AsCString());
    m_generic_types.insert({ConstString(name), userDefinedType});
  }
}

CompilerType CangjieDeclMap::GetGenericTypeByPartName(const std::string& name) {
  SymbolFile *symfile = m_type_system->GetSymbolFile();
  if (!symfile) {
    return CompilerType();
  }
  lldb_private::TypeList type_list;
  symfile->GetTypes(nullptr, lldb::eTypeClassClass|lldb::eTypeClassStruct, type_list);
  size_t ntypes = type_list.GetSize();
  std::string combine_name = name + "<" + GENERIC_TYPE_PREFIX_NAME;
  auto subName = name;
  if (subName = GetSubNameWithoutPkgname(subName, m_current_pkgname); !subName.empty()) {
    combine_name = subName + "<" + GENERIC_TYPE_PREFIX_NAME;
  }
  for (size_t i = 0; i < ntypes; ++i) {
    lldb::TypeSP type = type_list.GetTypeAtIndex(i);
    std::string tmpName = type->GetName().AsCString();
    auto tpos = tmpName.find("<");
    auto dcpos = tmpName.find(PACKAGE_SUFFIX);
    if (dcpos != std::string::npos && tpos > dcpos) {
      // tmpName: "ohos.ark_interop::Slab<T>"  -->  "Slab<T>"
      tmpName = tmpName.substr(dcpos + PACKAGE_SUFFIX.size());
    }
    auto tlpos = tmpName.find(combine_name);
    if (tlpos == 0) {
      CompilerType full_type = type->GetFullCompilerType();
      return full_type;
    }
    //  tmpName: "E1$Time<Default::Bar>"  -->  "Time<Default::Bar>"
    tmpName = DeletePrefixOfType(tmpName);
    tlpos = tmpName.find(combine_name);
    if (tlpos == 0) {
      CompilerType full_type = type->GetFullCompilerType();
      return full_type;
    }
  }
  return CompilerType();
}

void CangjieDeclMap::LookUpTypeByName(const std::string& name, std::vector<CompilerTypeInfo>& m_translate_decls) {
  Log *log = GetLog(LLDBLog::Expressions);
  // Find and create local, global variables.
  CompilerType type_local = LookUpVariable(name, VariableKind::LocalVariable);
  CollectVarDecl(name, type_local, VariableKind::LocalVariable, m_translate_decls);
  CompilerType type_global = LookUpVariable(name, VariableKind::GlobalVariable);
  CollectVarDecl(name, type_global, VariableKind::GlobalVariable, m_translate_decls);
  CompilerType type_static = LookUpVariable(name, VariableKind::StaticVariable);
  CollectVarDecl(name, type_static, VariableKind::StaticVariable, m_translate_decls);
  // Find and create local, global funcdecls.
  auto localFuncTypes = LookUpFunction(name, false);
  for (auto& ft : localFuncTypes) {
    if (!ft.Type.IsValid()) {
      continue;
    }
    m_translate_decls.emplace_back(ft);
  }
  auto globalFuncTypes = LookUpFunction(name, true);
  for (auto& ft : globalFuncTypes) {
    if (!ft.Type.IsValid()) {
      continue;
    }
    auto retTypeName = GetTypeNameWithoutPrefix(ft.Type.GetFunctionReturnType().GetTypeName(), m_current_pkgname);
    LookUpTypeByName(retTypeName, m_translate_decls);
    m_translate_decls.emplace_back(ft);
  }
  // Find structDecl, enumDecl, classDecl, interfaceDecl.
  auto userDefinedType = LookUpType(name);
  if (!userDefinedType.IsValid() || userDefinedType.GetTypeName().GetStringRef().contains("MapleRuntime::MemMap")) {
    userDefinedType = GetGenericTypeByPartName(name);
    if (userDefinedType.IsValid()) {
      LLDB_LOGF(log, "[Generic]: find type decl with generic param by part name[%s] -> [%s]. \n",
                ConstString(name).AsCString(), userDefinedType.GetTypeName().GetCString());
      m_generic_types.insert({ConstString(name), userDefinedType});
      builder.m_type_names.insert(userDefinedType.GetTypeName().AsCString());
    }
  }
  if (userDefinedType.IsValid()) {
    CompilerTypeInfo tempInfo(userDefinedType, 0, "");
    tempInfo.SetDeclInfo(DeclKind::UserDefinedDecl, name, true);
    m_translate_decls.emplace_back(tempInfo);
  }
}

void CangjieDeclMap::CreateDeclByType(const CompilerTypeInfo& typeInfo) {
  std::string name = typeInfo.name;
  switch (typeInfo.Kind) {
    case DeclKind::VarDecl:{
      // Find and create local ,global variables.
      CreateVarDeclByType(typeInfo, name);
      break;
    }
    case DeclKind::FuncDecl:{
      // Find and create local funcdecls.
      CreateFuncDeclByType(typeInfo, name,  typeInfo.Global);
      break;
    }
    case DeclKind::UserDefinedDecl:{
      // Find and create types.
      CreateTypeDeclByType(typeInfo.Type);
      break;
    }
    default :
      break;
  }
}

bool CangjieDeclMap::CheckAndModifyCapturedVardecl(VarDecl& vd, OwnedPtr<AST::FuncParamList>& paramList) {
    if (vd.type && vd.type->astKind == AST::ASTKind::REF_TYPE) {
      auto rt = RawStaticCast<AST::RefType *>(vd.type.get());
      static std::string CAPTURED_PREFIX = "$Captured_";
      if (auto pos = rt->ref.identifier.Val().find(CAPTURED_PREFIX); pos != std::string::npos) {
        OwnedPtr<FuncParam> param = MakeOwned<FuncParam>();
        param->identifier = vd.identifier;
        rt->ref.identifier = rt->ref.identifier.Val().substr(pos + CAPTURED_PREFIX.size());
        auto capturedType = builder.CreateAstType(AstTypeInfo(rt->ref.identifier.Val()), nullptr);
        param->type = capturedType->astKind == AST::ASTKind::REF_TYPE ?
          std::move(vd.type) : std::move(capturedType);
        paramList->params.emplace_back(std::move(param));
        // Modify captured variable: "var aa: $Captured_Int64 --> var aa: Int64 = 0x7fff...".
        auto primType = MakeOwned<AST::PrimitiveType>();
        primType->kind = AST::TypeKind::TYPE_INT64;
        primType->str = "Int64";
        vd.type = std::move(primType);
        auto lit = MakeOwned<AST::LitConstExpr>();
        lit->kind = AST::LitConstKind::INTEGER;
        // For the captured variable, the pointer transferred by the front end is offset by 8 bytes.
        // Therefore, the pointer needs to be offset back 8 bytes to the original $Captured_xxx.
        lit->stringValue = std::to_string(m_captured_var_addrs[vd.identifier.Val()] - 8);
        vd.initializer = std::move(lit);
        return true;
      }
    }
    return false;
}

void CreateCapturedVirtualFunc(OwnedPtr<AST::ClassDecl>& cd) {
    // For example, a func name can be _CN7default4mainHvE4foo2Hv$i
    auto vdi = MakeOwned<AST::VarDecl>();
    vdi->identifier = "$Captured$i";
    vdi->initializer = CreateCallExpr(CreateRefExpr("Object"), {});
    cd->body->decls.emplace(cd->body->decls.begin(), std::move(vdi));

    // For example, a func name can be _CN7default4mainHvE4foo2Hv$g
    auto vdg = MakeOwned<AST::VarDecl>();
    vdg->identifier = "$Captured$g";
    vdg->initializer = CreateCallExpr(CreateRefExpr("Object"), {});
    cd->body->decls.emplace(cd->body->decls.begin(), std::move(vdg));
}

void CangjieDeclMap::CreateCapturedVars() {
  // $CapturedVars: {i8*, i8*, ...}
  for (auto& [fn, type] : builder.m_capture_vars) {
    // $CapturedVars is a pointer now, get the base type.
    type = type.GetPointeeType();
    // Create $CapturedVars class
    auto cd = MakeOwned<AST::ClassDecl>();
    cd->body = MakeOwned<AST::ClassBody>();
    cd->identifier = fn + "$class" + LOCAL_FUNC_CAPTUREDVAR;
    cd->fullPackageName = m_current_pkgname;
    cd->mangledName = BaseMangler().Mangle(*cd);
    std::string typeName = SafeAsCString(type.GetTypeName());
    builder.CreateDeclGeneric(cd.get(), typeName);
    Ptr<AST::Decl> cla = RawStaticCast<AST::Decl *>(cd.get());
    CreateGenericConstraints(cla, type);
    CreateClassMemberDecls(type, cla);
    // Create class init
    auto initFunc = MakeOwned<AST::FuncDecl>();
    initFunc->funcBody = MakeOwned<AST::FuncBody>();
    initFunc->funcBody->body = MakeOwned<AST::Block>();
    initFunc->identifier = "init";
    initFunc->EnableAttr(Attribute::CONSTRUCTOR, Attribute::COMPILER_ADD);
    SetMemberDeclParent(initFunc.get(), cla);
    auto paramList = MakeOwned<AST::FuncParamList>();
    for (auto& d : cd->body->decls) {
      if (d->astKind == AST::ASTKind::VAR_DECL) {
        // Create local variable.
        std::vector<CompilerTypeInfo> typeinfos;
        auto type_local = LookUpVariable(d->identifier.Val(), VariableKind::LocalVariable);
        CollectVarDecl(d->identifier.Val(), type_local, VariableKind::LocalVariable, typeinfos);
        for (auto& type: typeinfos) {
          CreateDeclByType(type);
        }
        auto vd = RawStaticCast<AST::VarDecl*>(d.get());
        // If the capturedVar is not let variable, 'var aa: $Captured_Int64'.
        if (CheckAndModifyCapturedVardecl(*vd, paramList)) {
          continue;
        }
        // Create assignExpr 'this.aa = aa' in initFunc.
        auto ma = CreateMemberAccess(CreateRefExpr("this"), d->identifier);
        auto ae = CreateAssignExpr(std::move(ma), CreateRefExpr(d->identifier));
        initFunc->funcBody->body->body.emplace_back(std::move(ae));
        OwnedPtr<FuncParam> param = MakeOwned<AST::FuncParam>();
        param->identifier = d->identifier;
        param->type = ASTCloner::Clone<AST::Type>(vd->type.get());
        paramList->params.emplace_back(std::move(param));
      }
    }
    initFunc->funcBody->paramLists.emplace_back(std::move(paramList));
    cd->body->decls.emplace_back(std::move(initFunc));
    CreateCapturedVirtualFunc(cd);
    m_captured_vars.emplace_back(std::move(cd));
  }
}

std::vector<OwnedPtr<Cangjie::AST::Package>> CangjieDeclMap::CreateExternalAST() {
  for (const auto &element : m_identifys) {
    if (LookUpPackage(ConstString(element.c_str()))) {
      CreatePackage(element);
    }
  }
  Log *log = GetLog(LLDBLog::Expressions);
  // First, collect all type
  for (const auto& element : m_identifys) {
    std::vector<CompilerTypeInfo> translate_decls;
    LookUpTypeByName(element, translate_decls);
    // Translate types to decls.
    if (log) {
      LLDB_LOGF(log, "%s 's decls size is %ld. \n", ConstString(element).AsCString(), translate_decls.size());
    }
    for (const auto& decl: translate_decls) {
      CreateDeclByType(decl);
    }
  }
  for (auto& name:builder.m_type_names) {
    if (std::find(m_identifys.begin(), m_identifys.end(), name) != m_identifys.end()) {
      continue;
    }
    auto lookup_name = name;
    if (size_t pos = lookup_name.rfind("::"); pos != std::string::npos) {
      lookup_name = lookup_name.substr(pos + std::string("::").size());
    }
    std::vector<CompilerTypeInfo> ctypeinfos;
    LookUpTypeByName(lookup_name, ctypeinfos);
    for (auto& ti: ctypeinfos) {
      CreateDeclByType(ti);
    }
  }
  CreateCapturedVars();
  return CreateFakePackage();
}

lldb::VariableSP CangjieDeclMap::FindParsedVarByName(lldb_private::ConstString name) {
  if (m_parsed_vars.find(name) != m_parsed_vars.end()) {
    return m_parsed_vars[name];
  }
  return nullptr;
}

lldb_private::CompilerType CangjieDeclMap::FindParsedTypesByName(std::string name) {
  name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
  if (m_parsed_types.find(name) != m_parsed_types.end()) {
    return m_parsed_types[name];
  }

  if (m_generic_types.find(ConstString(name)) != m_generic_types.end()) {
    return m_generic_types[ConstString(name)];
  }
  return GetPrimitiveTypeByName(name);
}

static std::map<std::string, lldb::BasicType> basic_clang_type_map = {
    { "Bool", lldb::eBasicTypeBool },
    { "Unit", lldb::eBasicTypeSignedChar },
    { "Rune", lldb::eBasicTypeChar32 },
    { "Int8", lldb::eBasicTypeSignedChar },
    { "Int16", lldb::eBasicTypeShort },
    { "Int32", lldb::eBasicTypeInt },
    { "Int64", lldb::eBasicTypeLongLong },
    { "IntNative", lldb::eBasicTypeLongLong },
    { "UInt8", lldb::eBasicTypeUnsignedChar },
    { "UInt16", lldb::eBasicTypeUnsignedShort },
    { "UInt32", lldb::eBasicTypeUnsignedInt },
    { "UInt64", lldb::eBasicTypeUnsignedLongLong },
    { "UIntNative", lldb::eBasicTypeUnsignedLongLong },
    { "Float16", lldb::eBasicTypeHalf },
    { "Float32", lldb::eBasicTypeFloat },
    { "Float64", lldb::eBasicTypeDouble },
};

CompilerType CangjieDeclMap::GetPrimitiveTypeByName(std::string &name) {
  if (!m_type_system) {
    return CompilerType();
  }
  if (basic_clang_type_map.find(name) != basic_clang_type_map.end()) {
    return m_type_system->GetBasicType(basic_clang_type_map[name]).CreateTypedef(
      name.c_str(), m_type_system->CreateDeclContext(m_type_system->GetTranslationUnitDecl()), 0);
  }
  return CompilerType();
}

CompilerType CangjieDeclMap::GetDynamicTypeFromTy(Ptr<AST::Ty> ty, std::string typeName, CompilerType& genericType) {
  Log *log = GetLog(LLDBLog::Expressions);
  if (log && !ty->IsPrimitive()) {
    LLDB_LOGF(log, "[Generic]: instantiated [%s] by [%s]. \n", genericType.GetTypeName().AsCString(), ConstString(typeName).AsCString());
  }
  if (ty->IsPrimitive()) {
    return GetPrimitiveTypeByName(typeName);
  } else if (ty->IsString()) {
    auto type = this->FindParsedTypesByName(typeName);
    if (!type.IsValid()) {
      type = LookUptype(lldb::ModuleSP(), lldb_private::ConstString(typeName.c_str()),
                        lldb_private::CompilerDeclContext());
      m_parsed_types.insert({"std.core::String", type});
    }
    return type;
  } else if (ty->IsStructArray()) {
    return GetDynamicArrayType(ty, typeName, genericType);
  }

  switch (ty->kind) {
    case AST::TypeKind::TYPE_CLASS: {
      return GetDynamicClassType(ty, typeName, genericType);
    }
    case AST::TypeKind::TYPE_STRUCT: {
      return GetDynamicStructType(ty, typeName, genericType);
    }
    case AST::TypeKind::TYPE_TUPLE:{
      return GetDynamicTupleType(ty, typeName, genericType);
    }
    case AST::TypeKind::TYPE_ENUM: {
      return GetDynamicEnumType(ty, typeName, genericType);
    }
    case AST::TypeKind::TYPE_FUNC: {
      return GetDynamicFuncType(ty, typeName, genericType);
    }
    default:
      return CompilerType();
  }
  return CompilerType();
}

void CangjieDeclMap::AddFieldToRecordType(CompilerType& instancedType, Ptr<AST::Ty> ty, std::string& typeName, CompilerType& genericType)
{
  Log *log = GetLog(LLDBLog::Expressions);
  CJC_ASSERT(ty->IsStruct() || ty->IsEnum() || ty->IsClass() || ty->IsInterface()|| ty->IsArray() );
  Ptr<Cangjie::AST::Decl> decl = AST::Ty::GetDeclOfTy(ty);
  if (log) {
    LLDB_LOGF(log, "|-- [%s]  [%s]:[%s] start add member! \n", ConstString(AST::Ty::KindName(ty->kind)).AsCString(),
              ConstString(decl->identifier.Val()).AsCString(), ConstString(typeName).AsCString());
  }
  std::vector<std::string> instantiatedNames;
  if (decl->generic) {
    for (auto arg : ty->typeArgs) {
      instantiatedNames.emplace_back(arg->String());
    }
  }
  for (uint32_t i = 0; i < genericType.GetNumFields(); i++) {
    std::string member_name;
    CompilerType field_type;
    auto child_type = genericType.GetFieldAtIndex(i, member_name, nullptr, nullptr, nullptr);
    if (member_name == "$ti" || member_name == "$ti*") {
      continue;
    }
    if (log) {
      LLDB_LOGF(log, "    start add [%s.%s : %s]. \n", ConstString(decl->identifier.Val()).AsCString(),
                ConstString(member_name).AsCString(), child_type.GetTypeName().AsCString());
    }
    std::string compilerTypeName = SafeAsCString(child_type.GetTypeName());
    auto subTy = GetMemberDeclTyByName(ty, member_name, typeName);
    // There are three kind cases, for example
    // class A<T> {
    //    public var value3: Int64          case: non-generic type
    //    public var value1: T              case: generic type
    //    public var value2: Array<T>       case: compound type with generic
    // }
    if (subTy->IsGeneric()) {
      // Case generic type.
      auto index = GetSubGenericTyIndex(decl, subTy->name);
      std::string subTypeName = instantiatedNames[index];
      subTy = ty->typeArgs[index];
      auto tempGenericType = this->GetGenericTypeByPartName(subTy->name);
      field_type = GetDynamicTypeFromTy(subTy, subTypeName, tempGenericType);
    } else if (compilerTypeName.find(GENERIC_TYPE_PREFIX_NAME) != std::string::npos) {
      // Case compound type with Generic.
      field_type = GetDynamicTypeFromTy(subTy, subTy->String(), child_type);
    } else {
      // Case non-generic type
      field_type = child_type;
    }
    CJC_ASSERT(field_type.IsValid());
    if (builder.IsRefType(subTy)) {
      field_type = field_type.GetPointerType();
    }
    if (log) {
      LLDB_LOGF(log, "  [%s] add field [%s : %s]. \n", ConstString(decl->identifier.Val()).AsCString(),
                ConstString(member_name).AsCString(), field_type.GetTypeName().AsCString());
    }
    this->GetTypeSystem()->AddFieldToRecordType(instancedType, member_name.c_str(), field_type, lldb::eAccessPublic, 0);
  }
}

Ptr<AST::Ty> CangjieDeclMap::GetMemberDeclTyByName(Ptr<AST::Ty> ty, std::string& memberName, std::string& typeName)
{
  // class A<T> {
  //     public var value: T
  //     init(a:T) {
  //       value = a
  //     }
  // }
  // memberName is value
  Ptr<Cangjie::AST::Decl> decl = AST::Ty::GetDeclOfTy(ty);
  auto& memberDecls = decl->GetMemberDecls();
  for (size_t i = 0; i < memberDecls.size(); i++) {
    if (memberDecls[i]->astKind != AST::ASTKind::VAR_DECL) {
      continue;
    }
    if (memberDecls[i]->identifier.Val() == memberName) {
      return memberDecls[i]->GetTy();
    }
  }
  return decl->GetTy();
}

CompilerType CangjieDeclMap::GetEnumType(Ptr<AST::Ty> ty, std::string enum_name, CompilerType& genericType) {
  TypeSystemClang* ast = this->GetTypeSystem();
  CompilerType basic_type = ast->GetBasicType(lldb::eBasicTypeInt).CreateTypedef(
          "Int32", ast->CreateDeclContext(ast->GetTranslationUnitDecl()), 0);
  CompilerType enumType = ast->CreateEnumerationType(enum_name.c_str(),
          ast->GetTranslationUnitDecl(), OptionalClangModuleID(), Declaration(), basic_type, false);
  ast->StartTagDeclarationDefinition(enumType);

  std::string member_name;
  bool hasArgs = RawStaticCast<EnumTy*>(ty.get())->decl->hasArguments;
  auto genericEnumType = GetEnumerationType(genericType, hasArgs);
  auto enum_member_list = GetEnumeratorName(genericEnumType);
  for (size_t i = 0; i < enum_member_list.size(); i++) {
    const char* ctorName = enum_member_list[i].AsCString();
    ast->AddEnumerationValueToEnumerationType(enumType, Declaration(), ctorName, i, 4);
  }
  ast->CompleteTagDeclarationDefinition(enumType);
  return enumType;
}

void CangjieDeclMap::CreateAndAddInheritTypeToRecordType(Ptr<AST::Ty> ty, CompilerType& enum_type,
                                                         CompilerType& instancetiatedType, CompilerType& genericType) {
  TypeSystemClang* ast = this->GetTypeSystem();
  auto ti_type = ast->GetBasicType(lldb::eBasicTypeVoid).GetPointerType();
  std::string enum_name(enum_type.GetTypeName().AsCString());
  std::vector<std::unique_ptr<clang::CXXBaseSpecifier>> bases;  // DW_TAG_inheritance
  for (size_t i = 0; i < genericType.GetNumDirectBaseClasses(); i++) {
    CompilerType ctorFuncType = genericType.GetDirectBaseClassAtIndex(i, nullptr);
    auto ctor_name = enum_name + "_ctor_" + std::to_string(i);
    auto ctor_type = ast->CreateRecordType(nullptr, OptionalClangModuleID(), lldb::eAccessPublic,
      ctor_name.c_str(), clang::TTK_Struct, lldb::eLanguageTypeC);
    ast->StartTagDeclarationDefinition(ctor_type);
    ast->AddFieldToRecordType(ctor_type, "$ti*", ti_type, lldb::eAccessPublic, 0);
    ast->AddFieldToRecordType(ctor_type, "constructor", enum_type, lldb::eAccessPublic, 0);

    Ptr<Cangjie::AST::Decl> decl = AST::Ty::GetDeclOfTy(ty);
    auto& enumDecl = RawStaticCast<AST::EnumDecl *>(decl)->constructors;
    auto ctorDecl = Ptr(RawStaticCast<AST::FuncDecl *>(enumDecl[i].get()));
    size_t paramSize = 0;
    if (ctorDecl->astKind == ASTKind::FUNC_DECL) {
      auto& params = ctorDecl->funcBody->paramLists[0]->params;
      paramSize = params.size();
    }
    std::string member_name;
    std::vector<std::string> instantiatedNames = builder.SplitCollectionName(enum_name);
    for (size_t j = 0; j < paramSize; j++) {
      auto subTy = ctorDecl->funcBody->paramLists[0]->params[j].get()->GetTy();
      CompilerType ctor_para_type = ctorFuncType.GetFieldAtIndex(j + 1, member_name, nullptr, nullptr, nullptr);
      auto tempGenericType = ctor_para_type;
      if (subTy->IsGeneric()) {
        // case generic type.
        auto index = GetSubGenericTyIndex(decl, subTy->name);
        std::string subTypeName = instantiatedNames[index];
        subTy = ty->typeArgs[index];
        // Find generic type by name.
        ctor_para_type = GetDynamicTypeFromTy(subTy, subTypeName, tempGenericType);
      }
      std::string ctor_para_name = "arg_" + std::to_string(j + 1);
      auto argType = builder.IsRefType(subTy) ? ctor_para_type.GetPointerType() : ctor_para_type;
      ast->AddFieldToRecordType(ctor_type, ctor_para_name.c_str(), argType, lldb::eAccessPublic, 0);
    }

    ast->CompleteTagDeclarationDefinition(ctor_type);
    auto inherit_type = ast->CreateBaseClassSpecifier(ctor_type.GetOpaqueQualType(), lldb::eAccessPublic, false, true);
    auto type_source_info = inherit_type->getTypeSourceInfo();
    if (type_source_info) {
      auto type = ast->GetType(type_source_info->getType());
      ast->StartTagDeclarationDefinition(type);
      ast->CompleteTagDeclarationDefinition(type);
    }
    bases.push_back(std::move(inherit_type));
  }
  ast->TransferBaseClasses(instancetiatedType.GetOpaqueQualType(), std::move(bases));
}

CompilerType CangjieDeclMap::GetDynamicEnumType(Ptr<AST::Ty> ty, std::string typeName, CompilerType& genericType) {
  std::string genericName = genericType.GetTypeName().AsCString();
  EnumLayout enumKind = GetEnumLayout(genericName);
  // case: enum without args , compilerType is enumType.
  auto enum_type = GetEnumType(ty, typeName, genericType);
  if (enumKind == EnumLayout::E0Int) {
    return enum_type;
  }
  // case: enum with args , compilerType is struct type with ctor.
  auto pos = typeName.find(PACKAGE_SUFFIX);
  if (pos != std::string::npos) {
    pos = pos + PACKAGE_SUFFIX.size();
    auto enumPrefix = GetEnumPrefix(genericName);
    typeName = typeName.substr(0, pos) + enumPrefix + typeName.substr(pos);
  }

  TypeSystemClang* ast = this->GetTypeSystem();
  CompilerType dynamic_type = ast->CreateRecordType(nullptr, OptionalClangModuleID(), lldb::eAccessPublic,
                  ConstString(typeName).GetCString(), clang::TTK_Struct, lldb::eLanguageTypeC);
  ast->StartTagDeclarationDefinition(dynamic_type);
  if (enumKind == EnumLayout::E2OptionLike) {
    ast->AddFieldToRecordType(dynamic_type, "constructor", enum_type, lldb::eAccessPublic, 0);
    auto args = builder.GetInstantiatedParamDeclName(typeName);
    auto valType = GetDynamicTypeFromTy(ty->typeArgs[0], args[0], genericType);
    ast->AddFieldToRecordType(dynamic_type, "val", valType, lldb::eAccessPublic, 0);
  } else {
      // add typeinfo*.
    auto ti_type = ast->GetBasicType(lldb::eBasicTypeVoid).GetPointerType();
    ast->AddFieldToRecordType(dynamic_type, "$ti*", ti_type, lldb::eAccessPublic, 0);
    // E1Arg and E3ArgNoRef, set inheritance.
    CreateAndAddInheritTypeToRecordType(ty, enum_type, dynamic_type, genericType);
  }
  ast->CompleteTagDeclarationDefinition(dynamic_type);
  return dynamic_type;
}

lldb_private::CangjieDeclMap::EnumLayout CangjieDeclMap::GetEnumLayout(const std::string& eName) {
  std::string name = eName;
  name = GetSubNameWithoutPkgname(name, m_current_pkgname);
  if (name.empty()) {
    name = eName;
  }
  if (name.find(E0_PREFIX_NAME) == 0) {
    return EnumLayout::E0Int;
  }
  if (name.find(E2_PREFIX_NAME_OPTION_LIKE) == 0) {
    return EnumLayout::E2OptionLike;
  }
  if (name.find(E3_PREFIX_NAME) == 0) {
    return EnumLayout::E3ArgNoRef;
  }
  return EnumLayout::E1Arg;
}

CompilerType CangjieDeclMap::GetDynamicFuncType(Ptr<AST::Ty> ty, std::string typeName, CompilerType& genericType) {
  CJC_ASSERT(ty->IsFunc());
  TypeSystemClang* ast = this->GetTypeSystem();
  auto funcTy = RawStaticCast<FuncTy*>(ty.get());
  typeName.erase(std::remove(typeName.begin(), typeName.end(), ' '), typeName.end());
  CompilerType dynamic_type = ast->CreateRecordType(nullptr, lldb_private::OptionalClangModuleID(),
                                                    lldb::eAccessPublic, ConstString(typeName).GetStringRef(),
                                                    clang::TTK_Class, lldb::eLanguageTypeC);
  ast->StartTagDeclarationDefinition(dynamic_type);
  // Add typeinfo* to class member.
  CompilerType ti_type = ast->GetBasicType(lldb::eBasicTypeVoid).GetPointerType();
  ast->AddFieldToRecordType(dynamic_type, "$ti*", ti_type, lldb::eAccessPublic, 0);
  // The first element of the type_args, contains information about the return type.
  Ptr<AST::Ty> retTy = Ptr(funcTy->retTy.get());
  std::string retName = retTy->String();
  lldb_private::CompilerType return_type = GetDynamicTypeFromGenericTypeInfo(retTy, retName);
  std::vector<lldb_private::CompilerType> param_types;
  // para_typeinfo: type_args[1..type_arg_num-1]
  for (auto& param: funcTy->paramTys) {
    Ptr<AST::Ty> tmpParam = Ptr(param.get());
    std::string tmpName = param->String();
    param_types.push_back(GetDynamicTypeFromGenericTypeInfo(tmpParam, tmpName));
  }
  // type -> subroutine -> pointer
  unsigned type_quals = 0;
  auto subroutine_type = ast->CreateFunctionType(return_type, param_types.data(), param_types.size(),
                                                 false, type_quals, clang::CC_C);
  ast->AddFieldToRecordType(dynamic_type, "ptr", subroutine_type.GetPointerType(), lldb::eAccessPublic, 0);
  ast->CompleteTagDeclarationDefinition(dynamic_type);
  return dynamic_type;
}

CompilerType CangjieDeclMap::GetDynamicClassType(Ptr<AST::Ty> ty, std::string typeName, CompilerType& genericType) {
  Ptr<Cangjie::AST::Decl> decl = AST::Ty::GetDeclOfTy(ty);
  CJC_ASSERT(decl->generic && !decl->generic->typeParameters.empty());

  lldb_private::CompilerType type = this->GetTypeSystem()->CreateRecordType(nullptr, lldb_private::OptionalClangModuleID(), lldb::eAccessPublic,
                  lldb_private::ConstString(typeName).GetStringRef(), clang::TTK_Class, lldb::eLanguageTypeC);
  this->GetTypeSystem()->StartTagDeclarationDefinition(type);
  // Add typeinfo* to class member.
  CompilerType ti_type = this->GetTypeSystem()->GetBasicType(lldb::eBasicTypeVoid).GetPointerType();
  this->GetTypeSystem()->AddFieldToRecordType(type, "$ti*", ti_type, lldb::eAccessPublic, 0);

  AddFieldToRecordType(type, ty, typeName, genericType);
  this->GetTypeSystem()->CompleteTagDeclarationDefinition(type);
  return type;
}

CompilerType CangjieDeclMap::GetDynamicStructType(Ptr<AST::Ty> ty, std::string typeName, CompilerType& genericType) {
  lldb_private::CompilerType type = this->GetTypeSystem()->CreateRecordType(nullptr, lldb_private::OptionalClangModuleID(), lldb::eAccessPublic,
                  lldb_private::ConstString(typeName).GetStringRef(), clang::TTK_Struct, lldb::eLanguageTypeC);
  this->GetTypeSystem()->StartTagDeclarationDefinition(type);
  AddFieldToRecordType(type, ty, typeName, genericType);
  this->GetTypeSystem()->CompleteTagDeclarationDefinition(type);
  return type;
}

CompilerType CangjieDeclMap::GetDynamicTupleType(Ptr<AST::Ty> ty, std::string typeName, CompilerType& genericType)
{
  CompilerType instancedType = this->GetTypeSystem()->CreateRecordType(nullptr, OptionalClangModuleID(), lldb::eAccessPublic,
                  lldb_private::ConstString(typeName).GetStringRef(), clang::TTK_Struct, lldb::eLanguageTypeC);
  this->GetTypeSystem()->StartTagDeclarationDefinition(instancedType);
  auto tupleTy = RawStaticCast<TupleTy*>(ty.get());
  std::vector<std::string> instantiatedNames = builder.SplitTupleName(typeName);
  CJC_ASSERT(instantiatedNames.size() == tupleTy->typeArgs.size());
  for (size_t i = 0; i < tupleTy->typeArgs.size(); i++) {
    auto field_type = GetDynamicTypeFromTy(tupleTy->typeArgs[i], instantiatedNames[i], genericType);
    std::string field_name = "_" + std::to_string(i);
    this->GetTypeSystem()->AddFieldToRecordType(instancedType, field_name.c_str(), field_type, lldb::eAccessPublic, 0);
  }
  this->GetTypeSystem()->CompleteTagDeclarationDefinition(instancedType);
  return instancedType;
}

CompilerType CangjieDeclMap::GetDynamicRawArrayType(Ptr<AST::Ty> ty, std::string typeName, CompilerType& genericType) {
  auto tyName = ty->String();
  Ptr<Cangjie::AST::Decl> decl = AST::Ty::GetDeclOfTy(ty);
  if (decl) {
    auto demangle_info = Cangjie::Demangle(decl->mangledName);
    std::string pkgname = demangle_info.GetPkgName();
    std::string fullname = demangle_info.GetFullName();
    tyName = pkgname.empty() ? fullname : pkgname + std::string(PACKAGE_SUFFIX) + fullname;
    typeName = std::string("RawArray<") + tyName + std::string(">");
  } else {
    typeName = std::string("RawArray<") + typeName + std::string(">");
  }
  CompilerType type = this->GetTypeSystem()->CreateRecordType(nullptr, OptionalClangModuleID(), lldb::eAccessPublic,
                  ConstString(typeName).GetCString(), clang::TTK_Struct, lldb::eLanguageTypeC);
  this->GetTypeSystem()->StartTagDeclarationDefinition(type);
  CompilerType basic_type = this->GetTypeSystem()->GetBasicType(lldb::eBasicTypeLongLong).CreateTypedef(
          "Int64", this->GetTypeSystem()->CreateDeclContext(this->GetTypeSystem()->GetTranslationUnitDecl()), 0);
  // Add _typeinfo to raw-array member.
  this->GetTypeSystem()->AddFieldToRecordType(type, "_typeinfo", basic_type.GetPointerType(), lldb::eAccessPublic, 0);
  // Add size to raw-array member.
  this->GetTypeSystem()->AddFieldToRecordType(type, "size", basic_type, lldb::eAccessPublic, 0);

  // Add elements to raw-array member.
  CompilerType field_type;
  // Reftype may contain enum.
  if (builder.IsRefType(ty)) {
    field_type = GetDynamicTypeFromTy(ty, tyName, genericType).GetPointerType();
  } else {
    field_type = GetDynamicTypeFromTy(ty, tyName, genericType);
  }
  this->GetTypeSystem()->AddFieldToRecordType(type, "elements", field_type, lldb::eAccessPublic, 0);
  this->GetTypeSystem()->CompleteTagDeclarationDefinition(type);
  return type;
}

CompilerType CangjieDeclMap::GetDynamicArrayType(Ptr<AST::Ty> ty, std::string typeName, CompilerType& genericType) {
  Ptr<Cangjie::AST::Decl> decl = AST::Ty::GetDeclOfTy(ty);
  if (decl) {
    auto demangle_info = Cangjie::Demangle(decl->mangledName);
    std::string pkgname = demangle_info.GetPkgName();
    std::string fullname = demangle_info.GetFullName();
    typeName = pkgname.empty() ? fullname : pkgname + std::string(PACKAGE_SUFFIX) + fullname;
  }
  lldb_private::CompilerType type = this->GetTypeSystem()->CreateRecordType(nullptr, lldb_private::OptionalClangModuleID(), lldb::eAccessPublic,
                  ConstString(typeName).GetCString(), clang::TTK_Struct, lldb::eLanguageTypeC);
  this->GetTypeSystem()->StartTagDeclarationDefinition(type);

  auto arrayTy = RawStaticCast<ArrayTy*>(ty.get());
  CJC_ASSERT(arrayTy->typeArgs.size() == 1);
  // public struct Array<T> {
  //    let rawptr: RawArray<T>
  //    let start: Int64
  //    let len: Int64
  //    ...
  // }
  // Add elements to array member.
  std::vector<std::string> instantiatedNames = builder.SplitCollectionName(typeName);
  auto field_type = GetDynamicRawArrayType(arrayTy->typeArgs[0], instantiatedNames[0], genericType).GetPointerType();
  this->GetTypeSystem()->AddFieldToRecordType(type, "rawptr", field_type, lldb::eAccessPublic, 0);

  CompilerType basic_type = this->GetTypeSystem()->GetBasicType(lldb::eBasicTypeLongLong).CreateTypedef(
          "Int64", this->GetTypeSystem()->CreateDeclContext(this->GetTypeSystem()->GetTranslationUnitDecl()), 0);
  this->GetTypeSystem()->AddFieldToRecordType(type, "start", basic_type, lldb::eAccessPublic, 0);
  this->GetTypeSystem()->AddFieldToRecordType(type, "len", basic_type, lldb::eAccessPublic, 0);
  this->GetTypeSystem()->CompleteTagDeclarationDefinition(type);
  return type;
}

size_t CangjieDeclMap::GetSubGenericTyIndex(const Ptr<Cangjie::AST::Decl>& decl, std::string typeName)
{
  CJC_ASSERT(decl->generic && !decl->generic->typeParameters.empty());
  if (!decl->generic || decl->generic->typeParameters.empty()){
    return 0;
  }
  // Get generic decl index.
  size_t index = 0;
  for (size_t i = 0; i < decl->generic->typeParameters.size(); i++) {
    if (decl->generic->typeParameters[i]->identifier.Val() == typeName) {
      index = i;
      break;
    }
  }
  return index;
}

CompilerType CangjieDeclMap::CreateOptionReturnType(Ptr<Cangjie::AST::Ty> ty, std::string& typeName) {
  auto valTypeName = builder.GetInstantiatedParamDeclName(typeName);
  auto valType = CompilerType();
  if (m_parsed_types.find(valTypeName[0]) != m_parsed_types.end()) {
    valType = m_parsed_types[valTypeName[0]];
  } else {
    valType = GetPrimitiveTypeByName(valTypeName[0]);
  }

  if (!valType.IsValid()) {
    if (m_generic_types.find(ConstString(valTypeName[0])) != m_generic_types.end()) {
      valType = GetDynamicTypeFromTy(ty->typeArgs[0], valTypeName[0], m_generic_types[ConstString(valTypeName[0])]);
    }
  }

  if (!valType.IsValid()) {
    valType = LookUpType(valTypeName[0]);
  }

  if (!valType.IsValid()) {
    return CompilerType();
  }

  TypeSystemClang* ast = this->GetTypeSystem();
  CompilerType type = ast->CreateRecordType(nullptr, OptionalClangModuleID(), lldb::eAccessPublic,
      ConstString(typeName).GetCString(), clang::TTK_Struct, lldb::eLanguageTypeC);
  ast->StartTagDeclarationDefinition(type);
  if (!CangjieIRForTarget::IsReferenceType(valType)) {
    CompilerType basic_type = ast->GetBasicType(lldb::eBasicTypeInt);
    CompilerType enumType = ast->CreateEnumerationType(std::string("created"),
      ast->GetTranslationUnitDecl(), OptionalClangModuleID(), Declaration(), basic_type, false);
    ast->StartTagDeclarationDefinition(enumType);
    ast->AddEnumerationValueToEnumerationType(enumType, Declaration(), "Some", 0, 4);
    ast->AddEnumerationValueToEnumerationType(enumType, Declaration(), "None", 1, 4);
    ast->CompleteTagDeclarationDefinition(enumType);
    auto field = ast->AddFieldToRecordType(type, "constructor", enumType, lldb::eAccessPublic, 0);
    uint64_t size = valType.GetByteSize(nullptr).value_or(0);
    clang::ASTContext &clang_ast =  ast->getASTContext();
    llvm::APInt bitfield_bit_size_apint(clang_ast.getTypeSize(clang_ast.IntTy), size < 8? 1 : 64);
    auto bit_width = new (clang_ast)
        clang::IntegerLiteral(clang_ast, bitfield_bit_size_apint,
                              clang_ast.IntTy, clang::SourceLocation());
    field->setBitWidth(bit_width);
    ast->AddFieldToRecordType(type, "val", valType, lldb::eAccessPublic, 0);
  } else {
    ast->AddFieldToRecordType(type, "val", valType.GetPointerType(), lldb::eAccessPublic, 0);
  }
  ast->CompleteTagDeclarationDefinition(type);
  m_parsed_types.insert({typeName, type});

  return type;
}
