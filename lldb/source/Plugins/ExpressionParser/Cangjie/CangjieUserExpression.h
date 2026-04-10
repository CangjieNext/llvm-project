//===-- CangjieUserExpression.h -------------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_CANGJIEUSEREXPRESSION_H
#define LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_CANGJIEUSEREXPRESSION_H

#include "CangjieExpressionSourceCode.h"
#include "lldb/Expression/LLVMUserExpression.h"

namespace lldb_private {

class CangjieExpressionParser;

class CangjieUserExpression : public LLVMUserExpression {
public:
  CangjieUserExpression(ExecutionContextScope &exe_scope, llvm::StringRef expr,
                        llvm::StringRef prefix, lldb::LanguageType language,
                        ResultType desired_type,
                        const EvaluateExpressionOptions &options,
                        ValueObject *ctx_obj);

  ~CangjieUserExpression() override;

  bool Parse(DiagnosticManager &diagnostic_manager, ExecutionContext &exe_ctx,
             lldb_private::ExecutionPolicy execution_policy,
             bool keep_result_in_memory, bool generate_debug_info) override;
  bool TryParse(ExecutionContext &exe_ctx, bool generate_debug_info);
  bool Complete(ExecutionContext &exe_ctx, CompletionRequest &request,
                unsigned complete_pos) override;
  CangjieExpressionSourceCode::WrapKind GetWrapKind() const;

  lldb::ExpressionVariableSP
  GetResultAfterDematerialization(ExecutionContextScope *exe_scope) override;

  bool SetupPersistentState(DiagnosticManager &diagnostic_manager,
                            ExecutionContext &exe_ctx);

  std::unique_ptr<CangjieExpressionSourceCode> m_source_code;

  bool AddVarToArgs(ExecutionContext &exe_ctx);

private:
  void ScanContext(ExecutionContext &exe_ctx,
                   lldb_private::Status &err) override;

  bool AddArguments(ExecutionContext &exe_ctx, std::vector<lldb::addr_t> &args,
                    lldb::addr_t struct_address,
                    DiagnosticManager &diagnostic_manager) override;

  void CreateSourceCode(DiagnosticManager &diagnostic_manager,
                        ExecutionContext &exe_ctx,
                        std::vector<std::string> modules_to_import,
                        bool for_completion);

 class CangjiePersistentVariableDelegate
      : public Materializer::PersistentVariableDelegate {
  public:
    CangjiePersistentVariableDelegate() {}
    ConstString GetName() override;
    void DidDematerialize(lldb::ExpressionVariableSP &variable) override;
    lldb::ExpressionVariableSP GetVariable() { return m_variable; }
    void RegisterPersistentState(PersistentExpressionState *persistent_state);
  private:
    PersistentExpressionState *m_persistent_state;
    lldb::ExpressionVariableSP m_variable;
  };

  ValueObject *m_ctx_obj;
  /// Class name used for the expression.
  std::string m_class_name;

  /// True if it was parsed when exe_ctx was in a non-static method of class.
  bool m_in_member_method = false;

  /// True if it was parsed when exe_ctx was in a static method of class.
  bool m_in_static_method = false;

  ConstString m_func_name;

  CangjiePersistentVariableDelegate m_result_delegate;
  CangjiePersistentVariableDelegate m_error_result_delegate;

  /// True if "this" or "self" must be looked up and passed in.  False if the
  /// expression doesn't really use them and they can be NULL.
  bool m_needs_object_ptr = false;

  std::unique_ptr<CangjieExpressionParser> m_parser;
};

} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_CANGJIEUSEREXPRESSION_H
