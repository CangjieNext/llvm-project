//===-- CangjiePersistentVariables.cpp ------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#include "CangjiePersistentVariables.h"
#include "lldb/Core/ValueObject.h"
#include "lldb/Target/ExecutionContext.h"
#include "llvm/ADT/StringRef.h"

using namespace lldb;
using namespace lldb_private;

CangjiePersistentVariables::CangjiePersistentVariables()
    : PersistentExpressionState(LLVMCastKind::eKindCangjie) {}

ExpressionVariableSP CangjiePersistentVariables::CreatePersistentVariable(
    const lldb::ValueObjectSP &valobj_sp) {
  return AddNewlyConstructedVariable(new ExpressionVariable(valobj_sp));
}

ExpressionVariableSP CangjiePersistentVariables::CreatePersistentVariable(
    ExecutionContextScope *exe_scope, ConstString name,
    const CompilerType &compiler_type, lldb::ByteOrder byte_order,
    uint32_t addr_byte_size) {
  return AddNewlyConstructedVariable(
      new ExpressionVariable(exe_scope, name, compiler_type, byte_order,
                            addr_byte_size));
}

void CangjiePersistentVariables::RemovePersistentVariable(
    lldb::ExpressionVariableSP variable) {
  RemoveVariable(variable);

  if (m_next_persistent_variable_id == 0)
    return;

  llvm::StringRef name = variable->GetName().GetStringRef();
  if (!name.consume_front(GetPersistentVariablePrefix(false)))
    return;

  uint32_t variable_id;
  if (name.getAsInteger(10, variable_id))
    return;

  if (variable_id == m_next_persistent_variable_id - 1)
    m_next_persistent_variable_id--;
}

llvm::Optional<CompilerType>
CangjiePersistentVariables::GetCompilerTypeFromPersistentDecl(
    ConstString type_name) {
  CompilerType cached_type = GetType(type_name.AsCString());
  if (cached_type.IsValid())
    return cached_type;
  return llvm::None;
}

ConstString CangjiePersistentVariables::GetNextPersistentVariableName(
    bool is_error) {
  llvm::SmallString<64> name;
  {
    llvm::raw_string_ostream os(name);
    os << GetPersistentVariablePrefix(is_error)
       << m_next_persistent_variable_id++;
  }
  return ConstString(name);
}

//=== Type cache implementation ===
// Types are stored in ScratchTypeSystemClang's ASTContext, this cache just
// tracks what we've queried to avoid redundant debug info lookups.

bool CangjiePersistentVariables::HasType(const std::string &name) const {
  return m_cached_types.find(name) != m_cached_types.end();
}

CompilerType CangjiePersistentVariables::GetType(const std::string &name) const {
  auto it = m_cached_types.find(name);
  if (it != m_cached_types.end())
    return it->second;
  return CompilerType();
}

void CangjiePersistentVariables::AddType(const std::string &name,
                                          const CompilerType &type) {
  m_cached_types[name] = type;
}

//=== Variable cache implementation ===

bool CangjiePersistentVariables::HasVariable(ConstString name) const {
  return m_cached_vars.find(name) != m_cached_vars.end();
}

lldb::VariableSP CangjiePersistentVariables::GetVariable(ConstString name) const {
  auto it = m_cached_vars.find(name);
  if (it != m_cached_vars.end())
    return it->second.var_sp;
  return nullptr;
}

CompilerType CangjiePersistentVariables::GetVariableType(ConstString name) const {
  auto it = m_cached_vars.find(name);
  if (it != m_cached_vars.end())
    return it->second.type;
  return CompilerType();
}

void CangjiePersistentVariables::AddVariable(ConstString name,
                                              lldb::VariableSP var,
                                              const CompilerType &type,
                                              const std::string &mangled_name,
                                              bool is_let) {
  m_cached_vars[name] = {var, type, mangled_name, is_let};
}

//=== Function cache implementation ===

bool CangjiePersistentVariables::HasFunction(const std::string &name) const {
  return m_cached_functions.find(name) != m_cached_functions.end();
}

std::vector<CangjieCachedFunction>
CangjiePersistentVariables::GetFunctions(const std::string &name) const {
  auto it = m_cached_functions.find(name);
  if (it != m_cached_functions.end())
    return it->second;
  return {};
}

void CangjiePersistentVariables::AddFunction(const std::string &scope_name,
                                              const std::string &func_name,
                                              const std::string &func_type_str,
                                              const CompilerType &func_type) {
  auto it = m_cached_functions.find(func_name);
  if (it == m_cached_functions.end()) {
    m_cached_functions[func_name] = {};
    it = m_cached_functions.find(func_name);
  }
  // Avoid duplicate entries
  for (const auto &cached : it->second) {
    if (cached.scope_name == scope_name && cached.func_name == func_name)
      return;
  }
  it->second.push_back({scope_name, func_name, func_type_str, func_type});
}

//=== Package cache implementation ===

bool CangjiePersistentVariables::HasPackage(ConstString name) const {
  return m_cached_packages.find(name) != m_cached_packages.end();
}

void CangjiePersistentVariables::AddPackage(ConstString name) {
  m_cached_packages.insert(name);
}

//=== Queried identifiers cache implementation ===
// This is the primary optimization - track what identifiers we've already
// queried to avoid expensive debug info lookups in subsequent expressions.

bool CangjiePersistentVariables::HasQueriedIdentifier(const std::string &name) const {
  return m_queried_identifiers.find(name) != m_queried_identifiers.end();
}

void CangjiePersistentVariables::AddQueriedIdentifier(const std::string &name) {
  m_queried_identifiers.insert(name);
}

//=== Generic type cache implementation ===

bool CangjiePersistentVariables::HasGenericType(ConstString name) const {
  return m_cached_generic_types.find(name) != m_cached_generic_types.end();
}

CompilerType CangjiePersistentVariables::GetGenericType(ConstString name) const {
  auto it = m_cached_generic_types.find(name);
  if (it != m_cached_generic_types.end())
    return it->second;
  return CompilerType();
}

void CangjiePersistentVariables::AddGenericType(ConstString name,
                                                 const CompilerType &type) {
  m_cached_generic_types[name] = type;
}

//=== Package name implementation ===

void CangjiePersistentVariables::SetCurrentPackageName(const std::string &pkg_name) {
  m_current_pkg_name = pkg_name;
}

std::string CangjiePersistentVariables::GetCurrentPackageName() const {
  return m_current_pkg_name;
}

//=== Clear cache implementation ===

void CangjiePersistentVariables::Clear() {
  m_cached_types.clear();
  m_cached_vars.clear();
  m_cached_functions.clear();
  m_cached_packages.clear();
  m_queried_identifiers.clear();
  m_cached_generic_types.clear();
}