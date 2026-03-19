//===-- CommandObjectFrame.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_COMMANDS_COMMANDOBJECTFRAME_H
#define LLDB_SOURCE_COMMANDS_COMMANDOBJECTFRAME_H

#include "lldb/lldb-forward.h"
#include "lldb/Interpreter/CommandObjectMultiword.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/ValueObject.h"
#include "lldb/DataFormatters/DataVisualization.h"
#include "lldb/DataFormatters/ValueObjectPrinter.h"
#include "lldb/Host/Config.h"
#include "lldb/Host/OptionParser.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/CommandOptionArgumentTable.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Interpreter/OptionArgParser.h"
#include "lldb/Interpreter/OptionGroupFormat.h"
#include "lldb/Interpreter/OptionGroupValueObjectDisplay.h"
#include "lldb/Interpreter/OptionGroupVariable.h"
#include "lldb/Interpreter/Options.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/StackFrameRecognizer.h"
#include "lldb/Target/StopInfo.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"

namespace lldb_private {

// CommandObjectMultiwordFrame

class CommandObjectMultiwordFrame : public CommandObjectMultiword {
public:
  CommandObjectMultiwordFrame(CommandInterpreter &interpreter);

  ~CommandObjectMultiwordFrame() override;
};

class CommandObjectFrameSelect : public CommandObjectParsed {
public:
  class CommandOptions : public Options {
  public:
    CommandOptions() { OptionParsingStarting(nullptr); }

    ~CommandOptions() override = default;

    Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                          ExecutionContext *execution_context) override {
      Status error;
      const int short_option = m_getopt_table[option_idx].val;
      switch (short_option) {
      case 'r': {
        int32_t offset = 0;
        if (option_arg.getAsInteger(0, offset) || offset == INT32_MIN) {
          error.SetErrorStringWithFormat("invalid frame offset argument '%s'",
                                         option_arg.str().c_str());
        } else
          relative_frame_offset = offset;
        break;
      }

      default:
        llvm_unreachable("Unimplemented option");
      }

      return error;
    }

    void OptionParsingStarting(ExecutionContext *execution_context) override {
      relative_frame_offset.reset();
    }

    llvm::ArrayRef<OptionDefinition> GetDefinitions() override;

    llvm::Optional<int32_t> relative_frame_offset;
  };

  CommandObjectFrameSelect(CommandInterpreter &interpreter)
      : CommandObjectParsed(interpreter, "frame select",
                            "Select the current stack frame by "
                            "index from within the current thread "
                            "(see 'thread backtrace'.)",
                            nullptr,
                            lldb::eCommandRequiresThread | lldb::eCommandTryTargetAPILock |
                                lldb::eCommandProcessMustBeLaunched |
                                lldb::eCommandProcessMustBePaused) {
    InitOptions();
  }

  ~CommandObjectFrameSelect() override = default;

  void
  HandleArgumentCompletion(CompletionRequest &request,
                           OptionElementVector &opt_element_vector) override {
    if (request.GetCursorIndex() != 0)
      return;

    CommandCompletions::InvokeCommonCompletionCallbacks(
        GetCommandInterpreter(), CommandCompletions::eFrameIndexCompletion,
        request, nullptr);
  }

  Options *GetOptions() override { return &m_options; }

protected:
  CommandObjectFrameSelect(CommandInterpreter &interpreter,
    const char *name, const char *help,
    const char *syntax, uint32_t flags, bool use_cjthread);

  void InitOptions() {
    CommandArgumentEntry arg;
    CommandArgumentData index_arg;

    // Define the first (and only) variant of this arg.
    index_arg.arg_type = lldb::eArgTypeFrameIndex;
    index_arg.arg_repetition = eArgRepeatOptional;

    // There is only one variant this argument could be; put it into the
    // argument entry.
    arg.push_back(index_arg);

    // Push the data for the first argument into the m_arguments vector.
    m_arguments.push_back(arg);
  }

  bool DoExecute(Args &command, CommandReturnObject &result) override;

  bool m_use_cjt = false;
  CommandOptions m_options;
};

class CommandObjectFrameVariable : public CommandObjectParsed {
public:
  CommandObjectFrameVariable(CommandInterpreter &interpreter);

  ~CommandObjectFrameVariable() override = default;

  Options *GetOptions() override { return &m_option_group; }

  void
  HandleArgumentCompletion(CompletionRequest &request,
                           OptionElementVector &opt_element_vector) override {
    // Arguments are the standard source file completer.
    CommandCompletions::InvokeCommonCompletionCallbacks(
        GetCommandInterpreter(), CommandCompletions::eVariablePathCompletion,
        request, nullptr);
  }

protected:
  void InitOptions();

  CommandObjectFrameVariable(
      CommandInterpreter &interpreter, const char *name, const char *help,
      const char *syntax, uint32_t flags, bool use_cjthread);

  llvm::StringRef GetScopeString(lldb::VariableSP var_sp);

  bool DoExecute(Args &command, CommandReturnObject &result) override;

  bool m_use_cjt = false;
  OptionGroupOptions m_option_group;
  OptionGroupVariable m_option_variable;
  OptionGroupFormat m_option_format;
  OptionGroupValueObjectDisplay m_varobj_options;
};

} // namespace lldb_private

#endif // LLDB_SOURCE_COMMANDS_COMMANDOBJECTFRAME_H
