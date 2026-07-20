//===-- CangjieExpressionParser.h -----------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_CANGJIEEXPRESSIONPARSER_H
#define LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_CANGJIEEXPRESSIONPARSER_H

#include "CangjieDeclMap.h"

#include "cangjie/Basic/SourceManager.h"
#include "lldb/Expression/Materializer.h"
#include "lldb/Expression/DiagnosticManager.h"
#include "lldb/Expression/ExpressionParser.h"
#include "llvm/IR/Module.h"
#include <string>
#include <vector>

namespace lldb_private {

class IRExecutionUnit;
class TypeSystemClang;

class CangjieExpressionParser : public ExpressionParser {
public:
  CangjieExpressionParser(ExecutionContextScope *exe_scope, Expression &expr,
                          bool generate_debug_info);
  ~CangjieExpressionParser() override;

  bool Complete(CompletionRequest &request, unsigned line, unsigned pos,
                unsigned typed_pos) override;

  bool Parse(ExecutionContext &exe_ctx, const std::string text);

  bool RewriteExpression(DiagnosticManager &diagnostic_manager) override;

  Status
  PrepareForExecution(lldb::addr_t &func_addr, lldb::addr_t &func_end,
                      lldb::IRExecutionUnitSP &execution_unit_sp,
                      ExecutionContext &exe_ctx, bool &can_interpret,
                      lldb_private::ExecutionPolicy execution_policy) override;

  void ResetDeclMap(ExecutionContext &exe_ctx,
                    std::vector<std::string> identifiers, unsigned int fileID,
                    std::string fileName);
  void PrepareContextInfo(ExecutionContext &exeCtx, Cangjie::SourceManager& sm);
  void PrepareParse(lldb_private::Materializer *materializer,
                    lldb_private::Materializer::PersistentVariableDelegate *result_delegate,
                    lldb_private::Materializer::PersistentVariableDelegate *error_result_delegate,
                    bool is_static_method);
  std::unique_ptr<CangjieDeclMap> m_expr_decl_map_up;
  lldb::addr_t m_materialized_address;
  /// True if it was parsed when exe_ctx was in a non-static method.
  bool m_in_member_method = false;
private:
  /// The module to build IR into.
  std::unique_ptr<llvm::Module> m_module;

  lldb_private::Materializer *m_materializer = nullptr;   ///< If non-NULL, the materializer
                                          ///to use when reporting used
                                          ///variables.
  lldb_private::Materializer::PersistentVariableDelegate
      *m_result_delegate = nullptr; ///< If non-NULL, used to report expression results to
                          ///ClangUserExpression.

  lldb_private::Materializer::PersistentVariableDelegate
      *m_error_result_delegate = nullptr; ///< If non-NULL, used to report expression results to
                          ///ClangUserExpression.
  std::string m_result_type_name;
  CompilerType m_expr_result_type;
  unsigned int m_file_id = 0;
  bool m_in_class_context = false;
  bool m_in_constructor = false;
  bool m_in_static_method = false;
  bool m_search_func_name = false;
  std::string m_current_filename;
};

} // namespace lldb_private
#endif // LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_CANGJIEEXPRESSIONPARSER_H
