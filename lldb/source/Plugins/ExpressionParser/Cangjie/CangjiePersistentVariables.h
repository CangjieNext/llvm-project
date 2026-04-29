//===-- CangjiePersistentVariables.h ----------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_CANGJIEPERSISTENTVARIABLES_H
#define LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_CANGJIEPERSISTENTVARIABLES_H

#include "lldb/Expression/ExpressionVariable.h"
#include "lldb/Symbol/CompilerType.h"
#include "lldb/lldb-forward.h"
#include "lldb/Utility/ConstString.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace lldb_private {

/// Cached function information for cross-expression optimization.
/// Similar to Clang's approach, function types are stored in ScratchTypeSystemClang's
/// ASTContext and we only cache the metadata here.
struct CangjieCachedFunction {
  std::string scope_name;      // e.g., "std.collection"
  std::string func_name;       // e.g., "Array"
  std::string func_type_str;   // Function type signature
  lldb_private::CompilerType func_type;  // Stored in ScratchTypeSystemClang's ASTContext
};

/// Cached variable information.
/// Variable types are stored in ScratchTypeSystemClang's ASTContext (persistent),
/// similar to how Clang stores declarations in ASTContext.
struct CangjieCachedVariable {
  lldb::VariableSP var_sp;              // The variable itself
  CompilerType type;                    // Stored in ScratchTypeSystemClang's ASTContext
  std::string mangled_name;             // For static variables
  bool is_let;                          // Whether it's a constant (let)
};

/// \class CangjiePersistentVariables CangjiePersistentVariables.h
/// "lldb/Expression/CangjiePersistentVariables.h"
/// Manages persistent state for Cangjie expressions.
///
/// Similar to ClangPersistentVariables:
/// - CompilerTypes are stored in ScratchTypeSystemClang's ASTContext (persistent)
/// - This class only caches lookup metadata and pointers
/// - Types created via CangjieDeclMap::GetTypeSystem() persist across expressions
///
/// Key difference from Clang:
/// - Clang stores NamedDecl* pointers (decls in ASTContext)
/// - Cangjie can't store Cangjie AST decls (OwnedPtr lifetime limited)
/// - But CompilerType already persists in ScratchTypeSystemClang
class CangjiePersistentVariables : public PersistentExpressionState {
public:
  CangjiePersistentVariables();

  ~CangjiePersistentVariables() override = default;

  static bool classof(const PersistentExpressionState *pv) {
    return pv->getKind() == PersistentExpressionState::eKindCangjie;
  }

  // PersistentExpressionState interface
  lldb::ExpressionVariableSP
  CreatePersistentVariable(const lldb::ValueObjectSP &valobj_sp) override;

  lldb::ExpressionVariableSP CreatePersistentVariable(
      ExecutionContextScope *exe_scope, ConstString name,
      const CompilerType &compiler_type, lldb::ByteOrder byte_order,
      uint32_t addr_byte_size) override;

  void RemovePersistentVariable(lldb::ExpressionVariableSP variable) override;

  ConstString GetNextPersistentVariableName(bool is_error = false) override;

  llvm::Optional<CompilerType>
  GetCompilerTypeFromPersistentDecl(ConstString type_name) override;

  // === Type cache ===
  /// Types are stored in ScratchTypeSystemClang's ASTContext.
  /// This cache only tracks what has been queried to avoid duplicate lookups.
  bool HasType(const std::string &name) const;
  CompilerType GetType(const std::string &name) const;
  void AddType(const std::string &name, const CompilerType &type);

  // === Variable cache ===
  bool HasVariable(ConstString name) const;
  lldb::VariableSP GetVariable(ConstString name) const;
  CompilerType GetVariableType(ConstString name) const;
  void AddVariable(ConstString name, lldb::VariableSP var,
                   const CompilerType &type, const std::string &mangled_name = "",
                   bool is_let = false);

  // === Function cache ===
  bool HasFunction(const std::string &name) const;
  std::vector<CangjieCachedFunction> GetFunctions(const std::string &name) const;
  void AddFunction(const std::string &scope_name, const std::string &func_name,
                   const std::string &func_type_str, const CompilerType &func_type);

  // === Package cache ===
  bool HasPackage(ConstString name) const;
  void AddPackage(ConstString name);

  // === Queried identifiers cache ===
  /// Track identifiers that have been queried to avoid duplicate lookups.
  /// This is the primary optimization - avoid expensive debug info queries.
  bool HasQueriedIdentifier(const std::string &name) const;
  void AddQueriedIdentifier(const std::string &name);

  // === Generic type cache ===
  bool HasGenericType(ConstString name) const;
  CompilerType GetGenericType(ConstString name) const;
  void AddGenericType(ConstString name, const CompilerType &type);

  // === Package name cache ===
  void SetCurrentPackageName(const std::string &pkg_name);
  std::string GetCurrentPackageName() const;

  // === Clear cache ===
  void Clear();

protected:
  llvm::StringRef GetPersistentVariablePrefix(bool is_error = false) const override {
    return "$cj";
  }

private:
  uint32_t m_next_persistent_variable_id = 0;

  // Type cache: name -> CompilerType (type stored in ScratchTypeSystemClang's ASTContext)
  std::map<std::string, CompilerType> m_cached_types;

  // Variable cache: name -> CangjieCachedVariable
  std::map<ConstString, CangjieCachedVariable> m_cached_vars;

  // Function cache: name -> vector of CangjieCachedFunction
  std::map<std::string, std::vector<CangjieCachedFunction>> m_cached_functions;

  // Package cache
  std::set<ConstString> m_cached_packages;

  // Queried identifiers cache - primary optimization mechanism
  std::set<std::string> m_queried_identifiers;

  // Generic type cache
  std::map<ConstString, CompilerType> m_cached_generic_types;

  // Current package name
  std::string m_current_pkg_name;
};

} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_CANGJIEPERSISTENTVARIABLES_H