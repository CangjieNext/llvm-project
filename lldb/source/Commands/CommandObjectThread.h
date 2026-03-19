//===-- CommandObjectThread.h -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_COMMANDS_COMMANDOBJECTTHREAD_H
#define LLDB_SOURCE_COMMANDS_COMMANDOBJECTTHREAD_H

#include "CommandObjectThreadUtil.h"
#include "lldb/Interpreter/CommandObjectMultiword.h"

namespace lldb_private {

class CommandObjectMultiwordThread : public CommandObjectMultiword {
public:
  CommandObjectMultiwordThread(CommandInterpreter &interpreter);

  ~CommandObjectMultiwordThread() override;
};

class CommandObjectThreadBacktrace : public CommandObjectIterateOverThreads {
public:
  class CommandOptions : public Options {
  public:
    CommandOptions() {
      // Keep default values of all options in one place: OptionParsingStarting
      // ()
      OptionParsingStarting(nullptr);
    }

    ~CommandOptions() override = default;

    Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                          ExecutionContext *execution_context) override;

    void OptionParsingStarting(ExecutionContext *execution_context) override {
      m_count = UINT32_MAX;
      m_start = 0;
      m_extended_backtrace = false;
    }

    llvm::ArrayRef<OptionDefinition> GetDefinitions() override;

    // Instance variables to hold the values for command options.
    uint32_t m_count;
    uint32_t m_start;
    bool m_extended_backtrace;
  };

  CommandObjectThreadBacktrace(CommandInterpreter &interpreter);

  ~CommandObjectThreadBacktrace() override = default;

  Options *GetOptions() override { return &m_options; }

  llvm::Optional<std::string> GetRepeatCommand(Args &current_args,
                                               uint32_t idx) override;

protected:
  CommandObjectThreadBacktrace(CommandInterpreter &interpreter,
    const char *name, const char *help,
    const char *syntax, uint32_t flags, bool use_cjthread);

  void DoExtendedBacktrace(Thread *thread, CommandReturnObject &result);

  bool HandleOneThread(lldb::tid_t tid, CommandReturnObject &result) override;

  CommandOptions m_options;
};

} // namespace lldb_private

#endif // LLDB_SOURCE_COMMANDS_COMMANDOBJECTTHREAD_H
