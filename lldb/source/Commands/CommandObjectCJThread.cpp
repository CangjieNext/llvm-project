//===-- CommandObjectCJThread.cpp -----------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#include "CommandObjectCJThread.h"
#include "Commands/CommandObjCJThreadCommon.h"
#include "Commands/CommandObjectThreadUtil.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/ValueObject.h"
#include "lldb/Core/ValueObjectVariable.h"
#include "lldb/Host/OptionParser.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/CommandOptionArgumentTable.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Interpreter/OptionArgParser.h"
#include "lldb/Interpreter/OptionGroupPythonClassWithDict.h"
#include "lldb/Interpreter/Options.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Target/ThreadPlan.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Utility/State.h"
#include "lldb/Expression/UserExpression.h"
#include "lldb/DataFormatters/StringPrinter.h"
#include "lldb/DataFormatters/FormattersHelpers.h"
#include "lldb/DataFormatters/DumpValueObjectOptions.h"
#include "lldb/DataFormatters/ValueObjectPrinter.h"

#include "lldb/Target/CJRegisterContext.h"
#include "lldb/Target/CJThread.h"
#include "lldb/Target/StopInfo.h"

#include "lldb/Utility/RegisterValue.h"
#include "lldb/Target/RegisterContext.h"
#include <iostream>
#include <sstream>

#include "CommandObjectThread.h"
#include "CommandObjectFrame.h"

using namespace lldb;
using namespace lldb_private;

// CommandObjectCJThreadBacktrace
#define LLDB_OPTIONS_cjthread_backtrace
#define LLDB_OPTIONS_thread_backtrace
#include "CommandOptions.inc"

// CommandObjectCJThreadList
#define LLDB_OPTIONS_cjthread_list
#include "CommandOptions.inc"

// CommandObjectCJThreadInfo
class CommandObjectCJThreadInfo : public CommandObjectParsed {
public:
  explicit CommandObjectCJThreadInfo(CommandInterpreter &interpreter)
      : CommandObjectParsed(
            interpreter, "cjthread info",
            "Show a summary of a specific cjthread in the current target process.",
            "cjthread info [cjt id] ...",
            eCommandRequiresProcess | eCommandTryTargetAPILock |
            eCommandProcessMustBeLaunched | eCommandProcessMustBePaused) {
    CommandArgumentEntry arg;
    CommandArgumentData thread_idx_arg;

    // Define the first (and only) variant of this arg.
    thread_idx_arg.arg_type = eArgTypeIndex;
    thread_idx_arg.arg_repetition = eArgRepeatPlus;

    // There is only one variant this argument could be; put it into the
    // argument entry.
    arg.push_back(thread_idx_arg);

    // Push the data for the first argument into the m_arguments vector.
    m_arguments.push_back(arg);
  }

  ~CommandObjectCJThreadInfo() override = default;

protected:
  bool DoExecute(Args &command, CommandReturnObject &result) override {
    Status error;
    Process *process = m_exe_ctx.GetProcessPtr();
    if (!process) {
      result.AppendError("no process");
      return false;
    }
    process->RefreshCJThreadList(error);
    if (error.Fail()) return false;

    Stream &strm = result.GetOutputStream();
    result.SetStatus(eReturnStatusSuccessFinishNoResult);
    ThreadList &cjthread_list = process->GetCJThreadList();
    std::vector<lldb::tid_t> thread_id_array;

    for (uint64_t i = 0; i < command.GetArgumentCount(); i++) {
      uint32_t cjt_id;
      if (!llvm::to_integer(command.GetArgumentAtIndex(i), cjt_id)) {
        result.AppendErrorWithFormat("invalid cjthread index '%s'",
                                    command.GetArgumentAtIndex(i));
        return false;
      }
      thread_id_array.push_back(cjt_id);
    }

    if (thread_id_array.empty()) {
      ThreadSP selected_thread = cjthread_list.GetSelectedThread();
      if (!selected_thread) {
        result.AppendError("no selected thread");
        return false;
      }
      thread_id_array.push_back(selected_thread->GetID());
    }

    for (uint32_t thread_id : thread_id_array) {
      ThreadSP thread_sp(cjthread_list.FindThreadByID(thread_id));
      if (thread_sp) {
        thread_sp->GetStatus(strm, 0, 1, 0, false);
      } else {
        Log *log = GetLog(LLDBLog::Process);
        if (log) {
          LLDB_LOGF(log, "Process::GetThreadStatus - cjthread 0x" PRIu64
                         " vanished while running Thread::GetStatus.");
        }
      }
    }

    return result.Succeeded();
  }
};

// CommandObjectCJThreadSelect
class CommandObjectCJThreadSelect : public CommandObjectParsed {
public:
  explicit CommandObjectCJThreadSelect(CommandInterpreter &interpreter)
      : CommandObjectParsed(
            interpreter, "cjthread select",
            "Show a summary of a specific cjthread in the current target process.",
            "cjthread select [cjt id]",
            eCommandRequiresProcess | eCommandTryTargetAPILock |
            eCommandProcessMustBeLaunched | eCommandProcessMustBePaused) {
    CommandArgumentEntry arg;
    CommandArgumentData thread_idx_arg;

    // Define the first (and only) variant of this arg.
    thread_idx_arg.arg_type = eArgTypeIndex;
    thread_idx_arg.arg_repetition = eArgRepeatPlus;

    // There is only one variant this argument could be; put it into the
    // argument entry.
    arg.push_back(thread_idx_arg);

    // Push the data for the first argument into the m_arguments vector.
    m_arguments.push_back(arg);
  }

  ~CommandObjectCJThreadSelect() override = default;

protected:

  bool DoExecute(Args &command, CommandReturnObject &result) override {
    Status error;
    m_exe_ctx.GetProcessPtr()->RefreshCJThreadList(error);
    if (error.Fail()) return false;

    Debugger &debugger = GetCommandInterpreter().GetDebugger();
    TargetSP target_sp = debugger.GetSelectedTarget();
    Process *process = m_exe_ctx.GetProcessPtr();

    if (process == nullptr) {
      result.AppendError("no process");
      return false;
    }

    if (command.GetArgumentCount() != 1) {
      result.AppendError("cjthread select requires exactly one argument");
      return false;
    }

    uint32_t cjthread_id;
    if (!llvm::to_integer(command.GetArgumentAtIndex(0), cjthread_id)) {
      result.AppendErrorWithFormat("invalid cjthread id '%s'",
                                  command.GetArgumentAtIndex(0));
      return false;
    }

    ThreadList &cjthread_list = process->GetCJThreadList();
    CJThreadSP cjthread = std::dynamic_pointer_cast<CJThread>(cjthread_list.FindThreadByID(cjthread_id));
    if (cjthread == nullptr) {
      result.AppendErrorWithFormat("invalid cjthread #%s.\n",
                                   command.GetArgumentAtIndex(0));
      return false;
    }

    cjthread_list.SetSelectedThreadByID(cjthread_id, true);
    if (cjthread->GetCJThreadState() == CJThreadState::eRunning) {
      auto os_thread = process->GetThreadList().FindThreadByID(cjthread->GetHostThreadID());
      if (os_thread) {
        process->GetThreadList().SetSelectedThreadByID(os_thread->GetID(), false);
      }
    }
    result.SetStatus(eReturnStatusSuccessFinishNoResult);
    return result.Succeeded();
  }
};

// CommandObjectCJThreadStepWithType
class CommandObjectCJThreadStepWithType : public CommandObjectParsed {
public:
  explicit CommandObjectCJThreadStepWithType(CommandInterpreter &interpreter, const char *name,
                                             const char *help, StepType step_type)
      : CommandObjectParsed(
            interpreter, name, help, nullptr,
            eCommandRequiresProcess | eCommandTryTargetAPILock |
            eCommandProcessMustBeLaunched | eCommandProcessMustBePaused),
        m_step_type(step_type) {}

  ~CommandObjectCJThreadStepWithType() override = default;

  ThreadPlanSP GetPlanWithType(ThreadSP thread) {
    StackFrameSP frame = thread->GetStackFrameAtIndex(0);
    Status error;
    if (m_step_type == eStepTypeInto) {
      if (frame->HasDebugInformation()) {
        SymbolContext sc = frame->GetSymbolContext(eSymbolContextEverything);
        AddressRange range = sc.line_entry.range;
        return thread->QueueThreadPlanForStepInRange(
          false, range, sc, nullptr, lldb::eOnlyDuringStepping, error);
      }

      return thread->QueueThreadPlanForStepSingleInstruction(false, false, true, error);
    } else if (m_step_type == eStepTypeOver) {
      if (frame->HasDebugInformation()) {
        return thread->QueueThreadPlanForStepOverRange(
            false,
            frame->GetSymbolContext(eSymbolContextEverything).line_entry,
            frame->GetSymbolContext(eSymbolContextEverything),
            lldb::eOnlyDuringStepping, error, lldb_private::eLazyBoolYes);
      }

      return thread->QueueThreadPlanForStepSingleInstruction(true, false, true, error);
    } else if (m_step_type == eStepTypeTrace) {
      return thread->QueueThreadPlanForStepSingleInstruction(false, false, true, error);
    } else if (m_step_type == eStepTypeTraceOver) {
      return thread->QueueThreadPlanForStepSingleInstruction(true, false, true, error);
    } else if (m_step_type == eStepTypeOut) {
      return thread->QueueThreadPlanForStepOut(
          false, nullptr, false, true, lldb_private::eVoteYes,
          lldb_private::eVoteNoOpinion, thread->GetSelectedFrameIndex(), error,
          lldb_private::eLazyBoolNo);
    }
    return nullptr;
  }

  bool DoExecute(Args &command, CommandReturnObject &result) override {
    Process *process = m_exe_ctx.GetProcessPtr();
    if (!process) {
      result.AppendError("no process");
      return false;
    }
    ThreadList &cjtlist = process->GetCJThreadList();
    CJThreadSP selected_cjthread = std::dynamic_pointer_cast<CJThread>(cjtlist.GetSelectedThread());
    if (!selected_cjthread) {
      result.AppendError("Unknown error, cjthread list contains a non cjthread element");
      return false;
    }

    Status error;
    if (selected_cjthread->GetCJThreadState() != CJThreadState::eRunning &&
        !CJThread::WaitUntilCJThreadScheduled(GetSelectedTarget(), selected_cjthread, error)) {
      return false;
    }

    ThreadSP thread = process->GetThreadList().GetSelectedThread();
    if (!thread) {
      result.AppendError("no selected thread");
      return false;
    }
    ThreadPlanSP new_plan_sp = GetPlanWithType(thread);
    if (error.Fail()) {
      result.AppendErrorWithFormatv("failed to refresh cjthread list: `{0}`", error.AsCString());
      return false;
    }
    if (new_plan_sp) {
      new_plan_sp->SetIsControllingPlan(true);
      new_plan_sp->SetOkayToDiscard(false);
      unsigned cjt_id = selected_cjthread->GetID();
      thread = cjtlist.FindThreadByID(cjt_id);
      if (!thread) {
        result.AppendError("cjthread vanished when stepping inst");
        return false;
      }

      CJThreadSP cjthread = std::dynamic_pointer_cast<CJThread>(thread);
      if (!cjthread) {
        result.AppendError("a non-cjthread element occurs in CJThread list");
        return false;
      }

      process->GetCJThreadList().SetSelectedThreadByID(cjt_id);
      process->GetThreadList().SetSelectedThreadByID(cjthread->GetHostThreadID());
      process->ResumeSynchronous(&result.GetOutputStream());
    }
    return result.Succeeded();
  }

  StepType m_step_type;
};

// CommandObjectCJThreadBacktrace
class CommandObjectCJThreadBacktrace : public CommandObjectThreadBacktrace {
public:
  explicit CommandObjectCJThreadBacktrace(CommandInterpreter &interpreter)
    : CommandObjectThreadBacktrace(
          interpreter, "cjthread backtrace",
          "Show thread call stacks.  Defaults to the current thread, thread "
          "indexes can be specified as arguments.\n"
          "Use the thread-index \"all\" to see all threads.\n"
          "Use the thread-index \"unique\" to see threads grouped by unique "
          "call stacks.\n"
          "Use 'settings set frame-format' to customize the printing of "
          "frames in the backtrace and 'settings set thread-format' to "
          "customize the thread header.",
          nullptr,
          eCommandRequiresProcess | eCommandRequiresThread | 
              eCommandTryTargetAPILock | eCommandProcessMustBeLaunched |
              eCommandProcessMustBePaused, true) {
  }
};

// CommandObjectCJThreadList
class CommandObjectCJThreadList : public CommandObjectParsed {
  class CommandOptions : public Options {
  public:
    CommandOptions() { OptionParsingStarting(nullptr); }

    ~CommandOptions() override = default;

    Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                          ExecutionContext *execution_context) override {
      Status error;
      const int short_option = m_getopt_table[option_idx].val;

      switch (short_option) {
      case 's':
        if (option_arg.empty()) {
          error.SetErrorString("state option requires a value");
          return error;
        }

        if (option_arg == "idle" || option_arg == "0") {
          m_state_filter = CJThreadState::eIdle;
          m_has_state_filter = true;
        } else if (option_arg == "ready" || option_arg == "1") {
          m_state_filter = CJThreadState::eReady;
          m_has_state_filter = true;
        } else if (option_arg == "running" || option_arg == "2") {
          m_state_filter = CJThreadState::eRunning;
          m_has_state_filter = true;
        } else if (option_arg == "pending" || option_arg == "3") {
          m_state_filter = CJThreadState::ePending;
          m_has_state_filter = true;
        } else if (option_arg == "syscall" || option_arg == "4") {
          m_state_filter = CJThreadState::eSyscall;
          m_has_state_filter = true;
        } else {
          error.SetErrorStringWithFormat(
              "invalid state value '%s'. Valid values: "
              "idle (0), ready (1), running (2), pending (3), syscall (4)",
              option_arg.str().c_str());
          return error;
        }
        break;
      default:
        llvm_unreachable("Unimplemented option");
      }
      return error;
    }

    void OptionParsingStarting(ExecutionContext *execution_context) override {
      m_has_state_filter = false;
      m_state_filter = CJThreadState::eUnknown;
    }

    llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
      return llvm::makeArrayRef(g_cjthread_list_options);
    }

    bool m_has_state_filter;
    CJThreadState m_state_filter;
  };

public:
  explicit CommandObjectCJThreadList(CommandInterpreter &interpreter)
      : CommandObjectParsed(
            interpreter, "cjthread list",
            "Show a summary of each cjthread in the current target process.\n"
            "If --state is specified, only cjthreads with the matching state are shown.\n"
            "Note: cjthreads with 'unknown' state are not displayed.\n"
            "Valid states: idle (0), ready (1), running (2), pending (3), syscall (4).",
            "cjthread list [--state=<state>]",
            eCommandRequiresProcess | eCommandTryTargetAPILock |
            eCommandProcessMustBeLaunched | eCommandProcessMustBePaused) {}

  ~CommandObjectCJThreadList() override = default;

  Options *GetOptions() override { return &m_options; }

protected:
  bool DoExecute(Args &command, CommandReturnObject &result) override {
    Status error;
    Process *process = m_exe_ctx.GetProcessPtr();
    if (!process) {
      result.AppendError("no process");
      return false;
    }
    process->RefreshCJThreadList(error);
    if (error.Fail())
      return false;

    Stream &strm = result.GetOutputStream();
    const uint32_t start_frame = 0;
    const uint32_t num_frames = 0;
    const uint32_t num_frames_with_source = 0;
    process->GetStatus(strm);

    ThreadList &cjthread_list = process->GetCJThreadList();

    std::vector<lldb::tid_t> thread_id_array;
    {
      std::lock_guard<std::recursive_mutex> guard(cjthread_list.GetMutex());
      uint32_t num_threads = cjthread_list.GetSize();
      thread_id_array.reserve(num_threads);
      for (uint32_t idx = 0; idx < num_threads; ++idx) {
        ThreadSP thread = cjthread_list.GetThreadAtIndex(idx);
        if (thread)
          thread_id_array.push_back(thread->GetID());
      }
    }

    bool has_state_filter = m_options.m_has_state_filter;
    CJThreadState state_filter = m_options.m_state_filter;
    bool any_thread_displayed = false;

    for (lldb::tid_t tid : thread_id_array) {
      ThreadSP thread_sp = cjthread_list.FindThreadByID(tid);
      if (!thread_sp) {
        Log *log = GetLog(LLDBLog::Process);
        if (log) {
          LLDB_LOGF(log,
                    "Process::GetThreadStatus - cjthread %" PRIu64
                    " vanished while running Thread::GetStatus.",
                    tid);
        }
        continue;
      }

      CJThreadSP cjthread_sp = std::dynamic_pointer_cast<CJThread>(thread_sp);
      if (!cjthread_sp)
        continue;

      CJThreadState state = cjthread_sp->GetCJThreadState();

      // Skip cjthreads with unknown state
      if (state == CJThreadState::eUnknown)
        continue;

      // Apply state filter if specified
      if (has_state_filter && state != state_filter)
        continue;

      thread_sp->GetStatus(strm, start_frame, num_frames, num_frames_with_source,
                           false);
      any_thread_displayed = true;
    }

    if (!any_thread_displayed && has_state_filter) {
      strm.Printf("No cjthreads found with state '%s'.\n",
                  FormatCJThreadState(state_filter).data());
    }

    result.SetStatus(eReturnStatusSuccessFinishNoResult);
    return true;
  }

  CommandOptions m_options;
};

// CommandObjectCJThreadFrameSelect
class CommandObjectCJThreadFrameSelect : public CommandObjectFrameSelect {
public:
  explicit CommandObjectCJThreadFrameSelect(CommandInterpreter &interpreter)
      : CommandObjectFrameSelect(
            interpreter, "cjthread frame select",
            "Show a summary of each cjthread in the current target process.",
            "cjthread list",
            eCommandRequiresProcess | eCommandTryTargetAPILock |
            eCommandProcessMustBeLaunched | eCommandProcessMustBePaused, true)
        {}


  bool DoExecute(Args &command, CommandReturnObject &result) override {
    Status error;
    Process *process = m_exe_ctx.GetProcessPtr();
    if (!process) {
      result.AppendError("no process");
      return false;
    }
    process->RefreshCJThreadList(error);
    if (error.Fail()) return false;

    ThreadList &cjlist = process->GetCJThreadList();
    m_exe_ctx.SetCJThreadSP(std::dynamic_pointer_cast<CJThread>(cjlist.GetSelectedThread()));
    return CommandObjectFrameSelect::DoExecute(command, result);
  }

  ~CommandObjectCJThreadFrameSelect() override = default;
};

// CommandObjectCJThreadFrameVariable
class CommandObjectCJThreadFrameVariable : public CommandObjectFrameVariable {
public:
  explicit CommandObjectCJThreadFrameVariable(CommandInterpreter &interpreter)
      : CommandObjectFrameVariable(
            interpreter, "cjthread frame variable",
            "Show local variables of a cjthread's stack frame",
            "cjthread frame variable",
            eCommandRequiresProcess | eCommandTryTargetAPILock |
            eCommandProcessMustBeLaunched | eCommandProcessMustBePaused, true)
        {}

  ~CommandObjectCJThreadFrameVariable() override = default;
};

class CommandObjectCJThreadUntil : public CommandObjectThreadUntil {
public:
  explicit CommandObjectCJThreadUntil(CommandInterpreter &interpreter)
    : CommandObjectThreadUntil(interpreter) {}

   ~CommandObjectCJThreadUntil() override = default;
};

CommandObjectMultiwordCJThreadFrame::CommandObjectMultiwordCJThreadFrame(
    CommandInterpreter &interpreter)
    : CommandObjectMultiword(interpreter, "frame",
                             "Commands for operating cjthread's frame.",
                             "cjthread frame <subcommand> [<subcommand-options>]") {
  LoadSubCommand("select",
                 CommandObjectSP(new CommandObjectCJThreadFrameSelect(interpreter)));
  LoadSubCommand("variable",
                 CommandObjectSP(new CommandObjectCJThreadFrameVariable(interpreter)));
};

CommandObjectMultiwordCJThreadFrame::~CommandObjectMultiwordCJThreadFrame() = default;

CommandObjectMultiwordCJThread::CommandObjectMultiwordCJThread(
    CommandInterpreter &interpreter)
    : CommandObjectMultiword(interpreter, "cjthread",
                             "Commands for operating on "
                             "one or more cjthread in "
                             "the current process.",
                             "cjthread <subcommand> [<subcommand-options>]") {
  LoadSubCommand("backtrace",
                 CommandObjectSP(new CommandObjectCJThreadBacktrace(interpreter)));
  LoadSubCommand("bt" /* alias to backtrace */,
                 CommandObjectSP(new CommandObjectCJThreadBacktrace(interpreter)));
  LoadSubCommand("list",
                 CommandObjectSP(new CommandObjectCJThreadList(interpreter)));
  LoadSubCommand("info",
                 CommandObjectSP(new CommandObjectCJThreadInfo(interpreter)));
  LoadSubCommand("select",
                 CommandObjectSP(new CommandObjectCJThreadSelect(interpreter)));
  LoadSubCommand("frame",
                 CommandObjectSP(new CommandObjectMultiwordCJThreadFrame(interpreter)));

  LoadSubCommand("si",
                 CommandObjectSP(new CommandObjectCJThreadStepWithType(
                  interpreter, "cjthread step-inst",
                  "Instruction level single step, stepping into calls.  "
                  "Defaults to current cjthread unless specified.",
                  eStepTypeTrace)));
  LoadSubCommand("sn",
                 CommandObjectSP(new CommandObjectCJThreadStepWithType(
                  interpreter, "cjthread step-inst-over",
                  "Instruction level single step, stepping over calls.  "
                  "Defaults to current cjthread unless specified.",
                  eStepTypeTraceOver)));
  LoadSubCommand("s",
                 CommandObjectSP(new CommandObjectCJThreadStepWithType(
                  interpreter, "cjthread step-in",
                  "Source level single step, stepping into calls.  "
                  "Defaults to current cjthread unless specified.",
                  eStepTypeInto)));
  LoadSubCommand("n",
                 CommandObjectSP(new CommandObjectCJThreadStepWithType(
                  interpreter, "cjthread step-over",
                  "Source level single step, stepping over calls.  "
                  "Defaults to current cjthread unless specified.",
                  eStepTypeOver)));
  LoadSubCommand("finish",
                 CommandObjectSP(new CommandObjectCJThreadStepWithType(
                  interpreter, "cjthread step-out",
                  "Finish executing the current stack frame and stop after "
                  "returning.  Defaults to current cjthread unless specified.",
                  eStepTypeOut)));
  LoadSubCommand("until",
                 CommandObjectSP(new CommandObjectCJThreadUntil(interpreter)));
}

CommandObjectMultiwordCJThread::~CommandObjectMultiwordCJThread() = default;