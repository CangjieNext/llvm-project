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

class CommandObjectThreadUntil : public CommandObjectParsed {
public:
  class CommandOptions : public Options {
  public:
    uint32_t m_thread_idx = LLDB_INVALID_THREAD_ID;
    uint32_t m_frame_idx = LLDB_INVALID_FRAME_ID;

    CommandOptions() {
      OptionParsingStarting(nullptr);
    }

    void OptionParsingStarting(ExecutionContext *execution_context) override {	 
      m_thread_idx = LLDB_INVALID_THREAD_ID;	 
      m_frame_idx = 0;	 
      m_stop_others = false; 
      m_until_addrs.clear(); 
    }

    ~CommandOptions() override = default;

    Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                          ExecutionContext *execution_context) override;

    llvm::ArrayRef<OptionDefinition> GetDefinitions() override;

    uint32_t m_step_thread_idx = LLDB_INVALID_THREAD_ID;
    bool m_stop_others = false;
    std::vector<lldb::addr_t> m_until_addrs;
  };

  CommandObjectThreadUntil(CommandInterpreter &interpreter);

  ~CommandObjectThreadUntil() override = default;

  Options *GetOptions() override { return &m_options; };

protected:
  bool DoExecute(Args &command, CommandReturnObject &result) override;

  CommandOptions m_options;
};
} // namespace lldb_private

#endif // LLDB_SOURCE_COMMANDS_COMMANDOBJECTTHREAD_H
