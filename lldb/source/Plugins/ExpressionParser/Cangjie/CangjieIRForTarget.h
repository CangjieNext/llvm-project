//===-- CangjieIRForTarget.h ----------------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_IRFORTARGET_H
#define LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_IRFORTARGET_H

#include "lldb/Expression/Materializer.h"
#include "lldb/Target/ExecutionContext.h"

namespace llvm {
class BasicBlock;
class Function;
class GlobalValue;
class GlobalVariable;
class CallInst;
class Instruction;
class Constant;
class ConstantInt;
class Module;
class Value;
}

namespace lldb_private {
class CangjieDeclMap;
}

class CangjieIRForTarget {
public:
  CangjieIRForTarget(lldb_private::Materializer *materializer, llvm::Module *module,
                     lldb_private::ConstString func_name, lldb_private::CangjieDeclMap *decl_map,
                     lldb_private::ExecutionContext &exe_ctx);
  bool HandleIRForExecute();
  bool CreateResultVariable(lldb_private::Materializer::PersistentVariableDelegate* result_delegate,
                            std::string type_name, bool in_member, lldb_private::ConstString result_name);
  void AddVariableToArgs();
  bool ReplaceIRGlobals();
  void RemoveGlobalInitFunctionCall();
  void RemoveGlobalInitFunctionCall(llvm::BasicBlock &basic_block);
  lldb_private::CompilerType GetCompilerTypeByName(std::string &name);
  llvm::StringRef FindExtendFunction();
  void SetPlatformInfo();
  static bool IsReferenceType(const lldb_private::CompilerType &ty);
  bool IsReferenceType(lldb::VariableSP value);
  lldb_private::CompilerType m_expr_result_type;
private:
  class FunctionValueCache {
  public:
    typedef std::function<llvm::Value *(llvm::Function *)> Maker;
    typedef std::map<llvm::Function *, llvm::Value *> FunctionValueMap;

    FunctionValueCache(Maker const &maker);
    ~FunctionValueCache();
    llvm::Value *GetValue(llvm::Function *function);

  private:
    Maker const m_maker;
    FunctionValueMap m_values;
  };

  class ArgsForIR {
  public:
    ArgsForIR(lldb_private::ConstString name, llvm::Value *value,
              int32_t offset, lldb::VariableSP variable);

    ~ArgsForIR();

    lldb_private::ConstString m_name;

    // Value to be replaced in LLVM IR, include result
    llvm::Value *m_llvm_value;

    // The offset to args needs to be replaced.
    int32_t m_offset;

    lldb::VariableSP m_value;

    bool m_is_reference = false;
  };

  lldb_private::Materializer *m_materializer = nullptr;

  // The module being processed, or NULL if that has not been determined yet.
  llvm::Module *m_module = nullptr;

  // Variables that need to be added to arg
  std::vector<ArgsForIR> m_args;

  lldb_private::ConstString m_func_name;

  lldb_private::ExecutionContext m_exe_ctx;

  lldb_private::CangjieDeclMap *m_decl_map;

  FunctionValueCache m_entry_instruction_finder;

  bool m_replace_this_object = false;

  bool m_this_is_replaced = false;

  lldb_private::ConstString m_this_obj_name;

  lldb_private::ConstString m_super_obj_name;

  bool UnfoldConstant(llvm::Constant *old_constant,
                      llvm::Function *llvm_function,
                      FunctionValueCache &value_maker,
                      FunctionValueCache &entry_instruction_finder);
};

#endif /* LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_IRFORTARGET_H */
