//===-- CommandObjectCJThread.cpp -----------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
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
    m_exe_ctx.GetProcessPtr()->RefreshCJThreadList(error);
    if (error.Fail()) return false;

    Stream &strm = result.GetOutputStream();
    result.SetStatus(eReturnStatusSuccessFinishNoResult);
    Process *process = m_exe_ctx.GetProcessPtr();
    ThreadList &cjthread_list = process->GetCJThreadList();
    std::vector<lldb::tid_t> thread_id_array;

    for (uint64_t i = 0; i < command.GetArgumentCount(); i ++) {
      uint32_t cjt_id;
      if (!llvm::to_integer(command.GetArgumentAtIndex(i), cjt_id)) {
        result.AppendErrorWithFormat("invalid cjthread index '%s'",
                                    command.GetArgumentAtIndex(i));
        return false;
      }
      thread_id_array.push_back(cjt_id);
    }

    if (thread_id_array.empty()) {
      thread_id_array.push_back(cjthread_list.GetSelectedThread()->GetID());
    }

    for (uint32_t thread_id : thread_id_array) {
      ThreadSP thread_sp(cjthread_list.FindThreadByID(thread_id));
      if (thread_sp) {
        thread_sp->GetStatus(strm, 0, 1, 0, false);
      } else {
        Log *log = GetLog(LLDBLog::Process);
        LLDB_LOGF(log, "Process::GetThreadStatus - cjthread 0x" PRIu64
                       " vanished while running Thread::GetStatus.");
      }
    }

    result.SetStatus(eReturnStatusSuccessFinishNoResult);
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
  bool PrintGlobalVar(const char *name, CommandReturnObject &result) {
    Debugger &debugger = GetCommandInterpreter().GetDebugger();
    TargetSP target_sp = debugger.GetSelectedTarget();
    if (!target_sp) {
      result.AppendMessageWithFormat("No target selected.");
      result.SetStatus(eReturnStatusFailed);
      return false;
    }

    VariableList var_list;
    target_sp->GetImages().FindGlobalVariables(ConstString(name), UINT32_MAX,
                                               var_list);

    if (var_list.GetSize() == 0) {
      result.AppendMessageWithFormat("Global variable '%s' not found.", name);
      result.SetStatus(eReturnStatusFailed);
      return false;
    }

    VariableSP var_sp = var_list.GetVariableAtIndex(0);
    if (!var_sp) {
      result.AppendMessageWithFormat("Global variable '%s' not found.", name);
      result.SetStatus(eReturnStatusFailed);
      return false;
    }

    ExecutionContext exe_ctx(target_sp, true);
    ValueObjectSP valobj_sp = ValueObjectVariable::Create(
        exe_ctx.GetBestExecutionContextScope(), var_sp);

    if (!valobj_sp) {
      result.AppendMessageWithFormat("Failed to create ValueObject for '%s'",
                                     name);
      result.SetStatus(eReturnStatusFailed);
      return false;
    }

    result.AppendMessageWithFormat("Global variable: %s\n",
                                   valobj_sp->GetName().GetCString());
    result.AppendMessageWithFormat("Type: %s\n",
                                   valobj_sp->GetTypeName().GetCString());

    const size_t num_children = valobj_sp->GetNumChildren();
    if (num_children == 0) {
      const char *val_cstr = valobj_sp->GetValueAsCString();
      if (val_cstr)
        result.AppendMessageWithFormat("Value: %s\n", val_cstr);
      else
        result.AppendMessageWithFormat("Value: <unavailable>\n");
    } else {
      result.AppendMessageWithFormat("Fields:\n");
      for (size_t i = 0; i < num_children; ++i) {
        ValueObjectSP child_sp = valobj_sp->GetChildAtIndex(i, true);
        if (!child_sp)
          continue;

        const char *field_name = child_sp->GetName().GetCString();
        const char *field_type = child_sp->GetTypeName().GetCString();
        const char *field_val = child_sp->GetValueAsCString();

        result.AppendMessageWithFormat("  %s (%s) = %s\n",
                                       field_name ? field_name : "<anon>",
                                       field_type ? field_type : "<no type>",
                                       field_val ? field_val : "<unavailable>");
      }
    }

    // result.SetStatus(eReturnStatusSuccessFinishResult);
    return true;
  }

  bool DoExecute(Args &command, CommandReturnObject &result) override {
    // key struct:
    // - ThreadGDBRemote
    // - GDBRemoteRegisterContext
    // - GDBRemoteRegisterContext
    // PrintGlobalVar("CJ_GScheduleManager", result);

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

    Thread *new_thread = cjthread_list.FindThreadByID(cjthread_id).get();
    if (new_thread == nullptr) {
      result.AppendErrorWithFormat("invalid cjthread #%s.\n",
                                   command.GetArgumentAtIndex(0));
      return false;
    }

    cjthread_list.SetSelectedThreadByID(cjthread_id, true);
    result.SetStatus(eReturnStatusSuccessFinishNoResult);
    return result.Succeeded();
  }
};

// CommandObjectCJThreadStepInst
class CommandObjectCJThreadStepInst : public CommandObjectParsed {
public:
  explicit CommandObjectCJThreadStepInst(CommandInterpreter &interpreter)
      : CommandObjectParsed(
            interpreter, "cjthread step-inst",
            "Step inst on a selected cjthread",
            "cjthread step-inst",
            eCommandRequiresProcess | eCommandTryTargetAPILock |
            eCommandProcessMustBeLaunched | eCommandProcessMustBePaused) {
  }

  ~CommandObjectCJThreadStepInst() override = default;

  class ContinueUntil {
    Target &m_target;
    ThreadList &m_thlist;
    lldb::break_id_t m_until_bp_id;
    std::vector<lldb::break_id_t> m_disabled_points;
    std::vector<lldb::break_id_t> m_until_points;
  public:
    ContinueUntil(Target &target, ThreadList &thlist, lldb::addr_t pc)
    : m_target(target), m_thlist(thlist), m_until_bp_id(LLDB_INVALID_BREAK_ID) {
      // TODO: disable all other breakpoints which are enabled
      size_t num_bps = target.GetBreakpointList().GetSize();
      for (size_t i = 0; i < num_bps; ++i) {
          BreakpointSP bp = target.GetBreakpointList().GetBreakpointAtIndex(i);
          if (bp && bp->IsEnabled() && bp->IsInternal()) {
            m_disabled_points.push_back(bp->GetID());
            bp->SetEnabled(false);
          }
      }

      for (int i = 0, n = thlist.GetSize(); i < n; i ++) {
        std::vector<addr_t> address_list { pc };
        Status status;
        ThreadSP thread = thlist.GetThreadAtIndex(i);
        
        Breakpoint *until_bp =
            target.CreateBreakpoint(pc, true, false).get();
        if (until_bp != nullptr) {
          m_until_bp_id = until_bp->GetID();
          until_bp->SetThreadID(thread->GetID());
          m_until_points.push_back(until_bp->GetID());
          until_bp->SetBreakpointKind("continue-until-target");
        }
      }
    }

    bool CheckIfStopOnContUntil() {
      for (int i = 0, n = m_thlist.GetSize(); i < n; i ++) {
        ThreadSP thread = m_thlist.GetThreadAtIndex(i);
        lldb::StopInfoSP stop_info = thread->GetStopInfo();
        if (!stop_info) continue;
        lldb::StopReason stop_reason = stop_info->GetStopReason();
        if (stop_reason == lldb::eStopReasonBreakpoint) {
          lldb::break_id_t break_id = stop_info->GetValue();
          if (break_id != LLDB_INVALID_BREAK_ID && break_id == m_until_bp_id) {
            return true;
          }
        }
      }
      return false;
    }

    ~ContinueUntil() {
      for (lldb::break_id_t bid : m_until_points)
        m_target.RemoveBreakpointByID(bid);
      for (lldb::break_id_t bid : m_disabled_points) {
        BreakpointSP bp = m_target.GetBreakpointByID(bid);
        if (bp) bp->SetEnabled(true);
      }
    }
  };

protected:
  std::vector<lldb::break_id_t> m_until_points;

  bool StepInstOnRunningThread(CJThreadSP selected_thread, CommandReturnObject &result) {
    Status error;
    lldb::tid_t cjt_id = selected_thread->GetID();
    ThreadList &thlist = m_exe_ctx.GetProcessPtr()->GetThreadList();
    ThreadSP thread = thlist.FindThreadByID(selected_thread->GetHostThreadID());
    ThreadPlanSP new_plan_sp =
        thread->QueueThreadPlanForStepSingleInstruction(
            false /*step over*/, false /*abort other plans*/,
            false /*stop others*/, error);
    if (error.Fail()) {
      result.AppendErrorWithFormatv(
          "failed to refresh cjthread list: `{0}'", error.AsCString());
      return false;
    }
    if (new_plan_sp) {
      new_plan_sp->SetIsControllingPlan(true);
      new_plan_sp->SetOkayToDiscard(false);
      m_exe_ctx.GetProcessPtr()->ResumeSynchronous(&result.GetOutputStream());

      ThreadSP thread = m_exe_ctx.GetProcessPtr()->GetCJThreadList().FindThreadByID(cjt_id);
      if (!thread) {
        result.AppendError("cjthread vanished while stepping instruction");
        return false;
      }

      CJThreadSP cjthread = std::dynamic_pointer_cast<CJThread>(thread);
      if (!cjthread) {
        result.AppendError("unexpected non-cjthread element in CJThread list");
        return false;
      }

      m_exe_ctx.GetProcessPtr()->GetCJThreadList().SetSelectedThreadByID(
          cjt_id);
      m_exe_ctx.GetProcessPtr()->GetThreadList().SetSelectedThreadByID(
          cjthread->GetHostThreadID());
      return true;
    }
    return false;
  }

  bool WaitUntilCJThreadScheduled(CJThreadSP selected_thread, CommandReturnObject &result) {
    unsigned saved_cjt_id = selected_thread->GetID();
    uint64_t pc = selected_thread->GetRegisterContext()->GetPC();
    ThreadList &thlist = m_exe_ctx.GetProcessPtr()->GetThreadList();
    ThreadList &cjtlist = m_exe_ctx.GetProcessPtr()->GetCJThreadList();
    ContinueUntil cont_until(GetSelectedTarget(), thlist, pc);

    // m_exe_ctx.GetProcessPtr()->Resume();
    do {
      StreamString stream_after_resume;
      m_exe_ctx.GetProcessPtr()->ResumeSynchronous(&stream_after_resume);
      m_exe_ctx.GetProcessPtr()->RefreshStateAfterStop();
      llvm::StringRef output_after_resume = stream_after_resume.GetString();

      Status error;
      m_exe_ctx.GetProcessPtr()->RefreshCJThreadList(error, true);

      if (error.Fail()) {
        result.AppendErrorWithFormatv("failed to refresh cjthread list, reason: `{0}'", error.AsCString());
        return false;
      }

      ThreadSP thread = cjtlist.FindThreadByID(saved_cjt_id);
      if (!thread) {
        result.AppendError("cjthread vanished while stepping instruction");
        return false;
      }

      CJThreadSP cjthread = std::dynamic_pointer_cast<CJThread>(thread);
      if (!cjthread) {
        result.AppendError("unexpected thread object occurs in CJThread list");
        return false;
      }

      if (cjthread->GetCJThreadState() == CJThreadState::eRunning) {
        cjtlist.SetSelectedThreadByID(cjthread->GetID());
        ThreadList &thlist = m_exe_ctx.GetProcessPtr()->GetThreadList();
        ThreadSP thread = thlist.FindThreadByID(cjthread->GetHostThreadID());
        if (!thread) {
          result.AppendErrorWithFormatv("cjthread #{0}'s host thread #{1} is vanished",
            cjthread->GetID(), cjthread->GetHostThreadID());
          return false;
        }
        thlist.SetSelectedThreadByID(thread->GetID());
        return true;
      }
      
      if (!cont_until.CheckIfStopOnContUntil()) {
        // another breakpoint is triggered
        result.AppendMessage(output_after_resume);
        break;
      }
    } while (true);
    return false;
  }

  bool DoExecute(Args &command, CommandReturnObject &result) override {
    // thread until
    ThreadList &thlist = m_exe_ctx.GetProcessPtr()->GetThreadList();
    ThreadList &cjtlist = m_exe_ctx.GetProcessPtr()->GetCJThreadList();
    CJThreadSP selected_thread = std::dynamic_pointer_cast<CJThread>(cjtlist.GetSelectedThread());
    if (!selected_thread) {
      result.AppendError("Unknown error, cjthread list contains a non cjthread element");
      return false;
    }

    if (selected_thread->GetCJThreadState() == CJThreadState::eRunning) {
      return StepInstOnRunningThread(selected_thread, result);
    }

    if (!WaitUntilCJThreadScheduled(selected_thread, result)) {
      return false;
    }

    Status error;
    ThreadPlanSP new_plan_sp = thlist.GetSelectedThread()->QueueThreadPlanForStepSingleInstruction(
            false/*step over*/, false/*abort other plans*/,
            false/*stop others*/, error);
    if (error.Fail()) {
      result.AppendErrorWithFormatv("failed to refresh cjthread list: `{0}'", error.AsCString());
      return false;
    }
    if (new_plan_sp) {
      new_plan_sp->SetIsControllingPlan(true);
      new_plan_sp->SetOkayToDiscard(false);
      m_exe_ctx.GetProcessPtr()->ResumeSynchronous(&result.GetOutputStream());

      unsigned cjt_id = selected_thread->GetID();
      ThreadSP thread = cjtlist.FindThreadByID(cjt_id);
      if (!thread) {
        result.AppendError("cjthread vanished when stepping inst");
        return false;
      }

      CJThreadSP cjthread = std::dynamic_pointer_cast<CJThread>(thread);
      if (!cjthread) {
        result.AppendError("a non-cjthread element occurs in CJThread list");
        return false;
      }

      m_exe_ctx.GetProcessPtr()->GetCJThreadList().SetSelectedThreadByID(cjt_id);
      m_exe_ctx.GetProcessPtr()->GetThreadList().SetSelectedThreadByID(cjthread->GetHostThreadID());
    }
    return result.Succeeded();
  }
};

// CommandObjectCJThreadBacktrace
class CommandObjectCJThreadBacktrace : public CommandObjectThreadBacktrace {
public:
  CommandObjectCJThreadBacktrace(CommandInterpreter &interpreter)
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
public:
  explicit CommandObjectCJThreadList(CommandInterpreter &interpreter)
      : CommandObjectParsed(
            interpreter, "cjthread list",
            "Show a summary of each cjthread in the current target process.",
            "cjthread list",
            eCommandRequiresProcess | eCommandTryTargetAPILock |
            eCommandProcessMustBeLaunched | eCommandProcessMustBePaused)
      {}

  ~CommandObjectCJThreadList() override = default;

protected:
  bool DoExecute(Args &command, CommandReturnObject &result) override {
    Status error;
    m_exe_ctx.GetProcessPtr()->RefreshCJThreadList(error);
    if (error.Fail()) return false;

    Stream &strm = result.GetOutputStream();
    result.SetStatus(eReturnStatusSuccessFinishNoResult);
    Process *process = m_exe_ctx.GetProcessPtr();
    const bool only_threads_with_stop_reason = false;
    const uint32_t start_frame = 0;
    const uint32_t num_frames = 0;
    const uint32_t num_frames_with_source = 0;
    process->GetStatus(strm);

    ThreadList &cjthread_list = process->GetCJThreadList();

    // size_t num_thread_infos_dumped = 0;

    // You can't hold the thread list lock while calling Thread::GetStatus. That
    // very well might run code (e.g. if we need it to get return values or
    // arguments.)  For that to work the process has to be able to acquire it.
    // So instead copy the thread ID's, and look them up one by one:

    uint32_t num_threads;
    std::vector<lldb::tid_t> thread_id_array;
    // Scope for thread list locker;
    {
      std::lock_guard<std::recursive_mutex> guard(cjthread_list.GetMutex());
      ThreadList &curr_thread_list = cjthread_list;
      num_threads = curr_thread_list.GetSize();
      uint32_t idx;
      thread_id_array.resize(num_threads);
      for (idx = 0; idx < num_threads; ++idx)
        thread_id_array[idx] = curr_thread_list.GetThreadAtIndex(idx)->GetID();
    }

    for (uint32_t i = 0; i < num_threads; i++) {
      ThreadSP thread_sp(cjthread_list.FindThreadByID(thread_id_array[i]));
      if (thread_sp) {
        if (only_threads_with_stop_reason) {
          StopInfoSP stop_info_sp = thread_sp->GetStopInfo();
          if (!stop_info_sp || !stop_info_sp->IsValid())
            continue;
        }
        thread_sp->GetStatus(strm, start_frame, num_frames,
                             num_frames_with_source, false);
      } else {
        Log *log = GetLog(LLDBLog::Process);
        LLDB_LOGF(log, "Process::GetThreadStatus - cjthread 0x" PRIu64
                       " vanished while running Thread::GetStatus.");
      }
    }

    result.SetStatus(eReturnStatusSuccessFinishNoResult);
    return result.Succeeded();
  }
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
    m_exe_ctx.GetProcessPtr()->RefreshCJThreadList(error);
    if (error.Fail()) return false;

    ThreadList &cjlist = m_exe_ctx.GetProcessPtr()->GetCJThreadList();
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

CommandObjectMultiwordCJThreadFrame::CommandObjectMultiwordCJThreadFrame(
    CommandInterpreter &interpreter)
    : CommandObjectMultiword(interpreter, "frame",
                             "Commands for operating cjthread's frame.",
                             "cjthread frame <subcommand> [<subcommand-options>]") {
  LoadSubCommand("select",
                 CommandObjectSP(new CommandObjectCJThreadFrameSelect(interpreter)));
  LoadSubCommand("variable",
                 CommandObjectSP(new CommandObjectCJThreadFrameVariable(interpreter)));
}

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
                 CommandObjectSP(new CommandObjectCJThreadStepInst(interpreter)));
  // LoadSubCommand("var" /* alias to cjthread frame variable */,
  //                CommandObjectSP(new CommandObjectCJThreadFrameVariable(interpreter)));

  /* how to implement si
   * 1. continue to the address of cjthread.rip
   *    how to:
   *      target.CreateBreakpoint(addr, internal=true)
   *      while (true)
   *        Process.resume()
   *        RefreshCJThreadList()
   *        if cjlist.Find(id).state == RUNNING:
   *           break
   *      target.DeleteBreakpoint()
   * 2. invoke normal si
   */
}

CommandObjectMultiwordCJThread::~CommandObjectMultiwordCJThread() = default;