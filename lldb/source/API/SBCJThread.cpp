//===-- SBCJThread.cpp ------------------------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#include "lldb/API/SBCJThread.h"
#include "lldb/API/SBDebugger.h"
#include "lldb/API/SBFrame.h"
#include "lldb/API/SBProcess.h"
#include "lldb/API/SBStream.h"
#include "lldb/API/SBThreadPlan.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/ValueObject.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"
#include "lldb/Target/CJThread.h"
#include "lldb/Target/ThreadPlan.h"
#include "lldb/Target/ThreadPlanStepInRange.h"
#include "lldb/Target/ThreadPlanStepInstruction.h"
#include "lldb/Target/ThreadPlanStepOut.h"
#include "lldb/Target/ThreadPlanStepRange.h"
#include "lldb/Utility/Stream.h"
#include "lldb/lldb-enumerations.h"
#include "Utils.h"

using namespace lldb;
using namespace lldb_private;

// Constructors
SBCJThread::SBCJThread() : m_opaque_sp(new ExecutionContextRef()) {
  LLDB_INSTRUMENT_VA(this);
}

SBCJThread::SBCJThread(const ThreadSP &lldb_object_sp)
    : m_opaque_sp(new ExecutionContextRef(lldb_object_sp)) {
  LLDB_INSTRUMENT_VA(this, lldb_object_sp);
}

SBCJThread::SBCJThread(const SBCJThread &rhs) {
  LLDB_INSTRUMENT_VA(this, rhs);

  m_opaque_sp = clone(rhs.m_opaque_sp);
}

// Assignment operator

const lldb::SBCJThread &SBCJThread::operator=(const SBCJThread &rhs) {
  LLDB_INSTRUMENT_VA(this, rhs);

  if (this != &rhs)
    m_opaque_sp = clone(rhs.m_opaque_sp);
  return *this;
}

// Destructor
SBCJThread::~SBCJThread() = default;

bool SBCJThread::IsValid() const {
  LLDB_INSTRUMENT_VA(this);
  return this->operator bool();
}

SBCJThread::operator bool() const {
  LLDB_INSTRUMENT_VA(this);

  std::unique_lock<std::recursive_mutex> lock;
  ExecutionContext exe_ctx(m_opaque_sp.get(), lock);

  Target *target = exe_ctx.GetTargetPtr();
  Process *process = exe_ctx.GetProcessPtr();
  if (target && process) {
    Process::StopLocker stop_locker;
    if (stop_locker.TryLock(&process->GetRunLock()))
      return m_opaque_sp->GetCJThreadSP().get() != nullptr;
  }
  // Without a valid target & process, this thread can't be valid.
  return false;
}

void SBCJThread::Clear() {
  LLDB_INSTRUMENT_VA(this);

  m_opaque_sp->Clear();
}

void SBCJThread::SetCJThread(const CJThreadSP &lldb_object_sp) {
  m_opaque_sp->SetCJThreadSP(lldb_object_sp);
}

SBError SBCJThread::ResumeNewPlan(ExecutionContext &exe_ctx,
                                  ThreadPlan *new_plan) {
  SBError sb_error;

  Process *process = exe_ctx.GetProcessPtr();
  if (!process) {
    sb_error.SetErrorString("No process in SBCJThread::ResumeNewPlan");
    return sb_error;
  }

  CJThread *cjthread = exe_ctx.GetCJThreadPtr();
  if (!cjthread) {
    sb_error.SetErrorString("No cjthread in SBCJThread::ResumeNewPlan");
    return sb_error;
  }

  // User level plans should be Controlling Plans so they can be interrupted,
  // other plans executed, and then a "continue" will resume the plan.
  if (new_plan != nullptr) {
    new_plan->SetIsControllingPlan(true);
    new_plan->SetOkayToDiscard(false);
  }

  // Why do we need to set the current thread by ID here???
  process->GetThreadList().SetSelectedThreadByID(cjthread->GetHostThreadID());
  process->GetCJThreadList().SetSelectedThreadByID(cjthread->GetID());
  
  if (process->GetTarget().GetDebugger().GetAsyncExecution())
    sb_error.ref() = process->Resume();
  else
    sb_error.ref() = process->ResumeSynchronous(nullptr);

  return sb_error;
}

void SBCJThread::StepInstruction(bool step_over) {
  LLDB_INSTRUMENT_VA(this, step_over);

  SBError error; // Ignored
  StepInstruction(step_over, error);
}

void SBCJThread::StepInstruction(bool step_over, SBError &error) {
  LLDB_INSTRUMENT_VA(this, step_over, error);

  std::unique_lock<std::recursive_mutex> lock;
  ExecutionContext exe_ctx(m_opaque_sp.get(), lock);

  if (!exe_ctx.HasCJThreadScope()) {
    error.SetErrorString("this SBCJThread object is invalid");
    return;
  }

  CJThreadSP cjthread = exe_ctx.GetCJThreadSP();
  Status new_plan_status;
  if (cjthread->GetCJThreadState() != CJThreadState::eRunning) {
    CJThread::WaitUntilCJThreadScheduled(exe_ctx.GetTargetRef(), cjthread, status);
  }
  ThreadSP thread = exe_ctx.GetProcessSP()->GetThreadList().GetSelectedThread();
  ThreadPlanSP new_plan_sp(thread->QueueThreadPlanForStepSingleInstruction(
      step_over, true, true, new_plan_status));

  if (new_plan_status.Success()) {
    error = ResumeNewPlan(exe_ctx, new_plan_sp.get());
  } else {
    error.SetErrorString(new_plan_status.AsCString());
  }

  return;
}

void SBCJThread::StepOver(lldb::RunMode stop_other_threads) {
  LLDB_INSTRUMENT_VA(this, stop_other_threads);

  SBError error; // Ignored
  StepOver(stop_other_threads, error);
}

void SBCJThread::StepOver(lldb::RunMode stop_other_threads, SBError &error) {
  LLDB_INSTRUMENT_VA(this, stop_other_threads, error);

  std::unique_lock<std::recursive_mutex> lock;
  ExecutionContext exe_ctx(m_opaque_sp.get(), lock);

  if (!exe_ctx.HasCJThreadScope()) {
    error.SetErrorString("this SBCJThread object is invalid");
    return;
  }

  CJThreadSP cjthread = exe_ctx.GetCJThreadSP();
  Status new_plan_status;
  if (cjthread->GetCJThreadState() != CJThreadState::eRunning) {
    CJThread::WaitUntilCJThreadScheduled(exe_ctx.GetTargetRef(), cjthread, status);
  }
  ThreadSP thread = exe_ctx.GetProcessSP()->GetThreadList().GetSelectedThread();
  bool abort_other_plans = false;
  StackFrameSP frame_sp(thread->GetStackFrameAtIndex(0));

  Status new_plan_status;
  ThreadPlanSP new_plan_sp;
  if (frame_sp) {
    if (frame_sp->HasDebugInformation()) {
      const LazyBool avoid_no_debug = eLazyBoolCalculate;
      SymbolContext sc(frame_sp->GetSymbolContext(eSymbolContextEverything));
      new_plan_sp = thread->QueueThreadPlanForStepOverRange(
          abort_other_plans, sc.line_entry, sc, stop_other_threads,
          new_plan_status, avoid_no_debug);
    } else {
      new_plan_sp = thread->QueueThreadPlanForStepSingleInstruction(
          true, abort_other_plans, stop_other_threads, new_plan_status);
    }
  }
  error = ResumeNewPlan(exe_ctx, new_plan_sp.get());
}

void SBCJThread::StepInto(lldb::RunMode stop_other_threads) {
  LLDB_INSTRUMENT_VA(this, stop_other_threads);

  StepInto(nullptr, stop_other_threads);
}

void SBCJThread::StepInto(const char *target_name,
                        lldb::RunMode stop_other_threads) {
  LLDB_INSTRUMENT_VA(this, target_name, stop_other_threads);

  SBError error; // Ignored
  StepInto(target_name, LLDB_INVALID_LINE_NUMBER, error, stop_other_threads);
}

void SBCJThread::StepInto(const char *target_name, uint32_t end_line,
                        SBError &error, lldb::RunMode stop_other_threads) {
  LLDB_INSTRUMENT_VA(this, target_name, end_line, error, stop_other_threads);

  std::unique_lock<std::recursive_mutex> lock;
  ExecutionContext exe_ctx(m_opaque_sp.get(), lock);

  if (!exe_ctx.HasCJThreadScope()) {
    error.SetErrorString("this SBCJThread object is invalid");
    return;
  }

  bool abort_other_plans = false;

  CJThreadSP cjthread = exe_ctx.GetCJThreadSP();
  Status new_plan_status;
  if (cjthread->GetCJThreadState() != CJThreadState::eRunning) {
    CJThread::WaitUntilCJThreadScheduled(exe_ctx.GetTargetRef(), cjthread, status);
  }
  ThreadSP thread = exe_ctx.GetProcessSP()->GetThreadList().GetSelectedThread();
  StackFrameSP frame_sp(thread->GetStackFrameAtIndex(0));
  ThreadPlanSP new_plan_sp;
  Status new_plan_status;

  if (frame_sp && frame_sp->HasDebugInformation()) {
    SymbolContext sc(frame_sp->GetSymbolContext(eSymbolContextEverything));
    AddressRange range;
    if (end_line == LLDB_INVALID_LINE_NUMBER)
      range = sc.line_entry.range;
    else {
      if (!sc.GetAddressRangeFromHereToEndLine(end_line, range, error.ref()))
        return;
    }

    const LazyBool step_out_avoids_code_without_debug_info =
        eLazyBoolCalculate;
    const LazyBool step_in_avoids_code_without_debug_info =
        eLazyBoolCalculate;
    new_plan_sp = thread->QueueThreadPlanForStepInRange(
        abort_other_plans, range, sc, target_name, stop_other_threads,
        new_plan_status, step_in_avoids_code_without_debug_info,
        step_out_avoids_code_without_debug_info);
  } else {
    new_plan_sp = thread->QueueThreadPlanForStepSingleInstruction(
        false, abort_other_plans, stop_other_threads, new_plan_status);
  }

  if (new_plan_status.Success())
    error = ResumeNewPlan(exe_ctx, new_plan_sp.get());
  else
    error.SetErrorString(new_plan_status.AsCString());
}

void SBCJThread::StepOut() {
  LLDB_INSTRUMENT_VA(this);

  SBError error; // Ignored
  StepOut(error);
}

void SBCJThread::StepOut(SBError &error) {
  LLDB_INSTRUMENT_VA(this, error);

  std::unique_lock<std::recursive_mutex> lock;
  ExecutionContext exe_ctx(m_opaque_sp.get(), lock);

  if (!exe_ctx.HasCJThreadScope()) {
    error.SetErrorString("this SBCJThread object is invalid");
    return;
  }

  bool abort_other_plans = false;
  bool stop_other_threads = false;

  CJThreadSP cjthread = exe_ctx.GetCJThreadSP();
  Status new_plan_status;
  if (cjthread->GetCJThreadState() != CJThreadState::eRunning) {
    CJThread::WaitUntilCJThreadScheduled(exe_ctx.GetTargetRef(), cjthread, status);
  }
  ThreadSP thread = exe_ctx.GetProcessSP()->GetThreadList().GetSelectedThread();

  const LazyBool avoid_no_debug = eLazyBoolCalculate;
  Status new_plan_status;
  ThreadPlanSP new_plan_sp(thread->QueueThreadPlanForStepOut(
      abort_other_plans, nullptr, false, stop_other_threads, eVoteYes,
      eVoteNoOpinion, 0, new_plan_status, avoid_no_debug));

  if (new_plan_status.Success())
    error = ResumeNewPlan(exe_ctx, new_plan_sp.get());
  else
    error.SetErrorString(new_plan_status.AsCString());
}

SBError SBCJThread::StepOverUntil(lldb::SBFrame &sb_frame,
                                lldb::SBFileSpec &sb_file_spec, uint32_t line) {
  LLDB_INSTRUMENT_VA(this, sb_frame, sb_file_spec, line);

  SBError sb_error;
  char path[PATH_MAX];

  std::unique_lock<std::recursive_mutex> lock;
  ExecutionContext exe_ctx(m_opaque_sp.get(), lock);

  StackFrameSP frame_sp(sb_frame.GetFrameSP());

  if (exe_ctx.HasCJThreadScope()) {
    Target *target = exe_ctx.GetTargetPtr();
    Thread *thread = exe_ctx.GetThreadPtr();

    if (line == 0) {
      sb_error.SetErrorString("invalid line argument");
      return sb_error;
    }

    if (!frame_sp) {
      frame_sp = thread->GetSelectedFrame();
      if (!frame_sp)
        frame_sp = thread->GetStackFrameAtIndex(0);
    }

    SymbolContext frame_sc;
    if (!frame_sp) {
      sb_error.SetErrorString("no valid frames in thread to step");
      return sb_error;
    }

    // If we have a frame, get its line
    frame_sc = frame_sp->GetSymbolContext(
        eSymbolContextCompUnit | eSymbolContextFunction |
        eSymbolContextLineEntry | eSymbolContextSymbol);

    if (frame_sc.comp_unit == nullptr) {
      sb_error.SetErrorStringWithFormat(
          "frame %u doesn't have debug information", frame_sp->GetFrameIndex());
      return sb_error;
    }

    FileSpec step_file_spec;
    if (sb_file_spec.IsValid()) {
      // The file spec passed in was valid, so use it
      step_file_spec = sb_file_spec.ref();
    } else {
      if (frame_sc.line_entry.IsValid())
        step_file_spec = frame_sc.line_entry.file;
      else {
        sb_error.SetErrorString("invalid file argument or no file for frame");
        return sb_error;
      }
    }

    // Grab the current function, then we will make sure the "until" address is
    // within the function.  We discard addresses that are out of the current
    // function, and then if there are no addresses remaining, give an
    // appropriate error message.

    bool all_in_function = true;
    AddressRange fun_range = frame_sc.function->GetAddressRange();

    std::vector<addr_t> step_over_until_addrs;
    const bool abort_other_plans = false;
    const bool stop_other_threads = false;
    // TODO: Handle SourceLocationSpec column information
    SourceLocationSpec location_spec(
        step_file_spec, line, /*column=*/llvm::None, /*check_inlines=*/true,
        /*exact_match=*/false);

    SymbolContextList sc_list;
    frame_sc.comp_unit->ResolveSymbolContext(location_spec,
                                             eSymbolContextLineEntry, sc_list);
    const uint32_t num_matches = sc_list.GetSize();
    if (num_matches > 0) {
      SymbolContext sc;
      for (uint32_t i = 0; i < num_matches; ++i) {
        if (sc_list.GetContextAtIndex(i, sc)) {
          addr_t step_addr =
              sc.line_entry.range.GetBaseAddress().GetLoadAddress(target);
          if (step_addr != LLDB_INVALID_ADDRESS) {
            if (fun_range.ContainsLoadAddress(step_addr, target))
              step_over_until_addrs.push_back(step_addr);
            else
              all_in_function = false;
          }
        }
      }
    }

    if (step_over_until_addrs.empty()) {
      if (all_in_function) {
        step_file_spec.GetPath(path, sizeof(path));
        sb_error.SetErrorStringWithFormat("No line entries for %s:%u", path,
                                          line);
      } else
        sb_error.SetErrorString("step until target not in current function");
    } else {
      Status new_plan_status;
      ThreadPlanSP new_plan_sp(thread->QueueThreadPlanForStepUntil(
          abort_other_plans, &step_over_until_addrs[0],
          step_over_until_addrs.size(), stop_other_threads,
          frame_sp->GetFrameIndex(), new_plan_status));

      if (new_plan_status.Success())
        sb_error = ResumeNewPlan(exe_ctx, new_plan_sp.get());
      else
        sb_error.SetErrorString(new_plan_status.AsCString());
    }
  } else {
    sb_error.SetErrorString("this SBCJThread object is invalid");
  }
  return sb_error;
}


uint32_t SBCJThread::GetNumFrames() {
  LLDB_INSTRUMENT_VA(this);

  uint32_t num_frames = 0;
  std::unique_lock<std::recursive_mutex> lock;
  ExecutionContext exe_ctx(m_opaque_sp.get(), lock);

  if (exe_ctx.HasCJThreadScope()) {
    Process::StopLocker stop_locker;
    if (stop_locker.TryLock(&exe_ctx.GetProcessPtr()->GetRunLock())) {
      num_frames = exe_ctx.GetCJThreadPtr()->GetStackFrameCount();
    }
  }

  return num_frames;
}

SBFrame SBCJThread::GetFrameAtIndex(uint32_t idx) {
  LLDB_INSTRUMENT_VA(this, idx);

  SBFrame sb_frame;
  StackFrameSP frame_sp;
  std::unique_lock<std::recursive_mutex> lock;
  ExecutionContext exe_ctx(m_opaque_sp.get(), lock);

  if (exe_ctx.HasCJThreadScope()) {
    Process::StopLocker stop_locker;
    if (stop_locker.TryLock(&exe_ctx.GetProcessPtr()->GetRunLock())) {
      frame_sp = exe_ctx.GetCJThreadPtr()->GetStackFrameAtIndex(idx);
      sb_frame.SetFrameSP(frame_sp);
    }
  }

  return sb_frame;
}

lldb::SBFrame SBCJThread::GetSelectedFrame() {
  LLDB_INSTRUMENT_VA(this);

  SBFrame sb_frame;
  StackFrameSP frame_sp;
  std::unique_lock<std::recursive_mutex> lock;
  ExecutionContext exe_ctx(m_opaque_sp.get(), lock);

  if (exe_ctx.HasCJThreadScope()) {
    Process::StopLocker stop_locker;
    if (stop_locker.TryLock(&exe_ctx.GetProcessPtr()->GetRunLock())) {
      frame_sp = exe_ctx.GetCJThreadPtr()->GetSelectedFrame();
      sb_frame.SetFrameSP(frame_sp);
    }
  }

  return sb_frame;
}

lldb::SBFrame SBCJThread::SetSelectedFrame(uint32_t idx) {
  LLDB_INSTRUMENT_VA(this, idx);

  SBFrame sb_frame;
  StackFrameSP frame_sp;
  std::unique_lock<std::recursive_mutex> lock;
  ExecutionContext exe_ctx(m_opaque_sp.get(), lock);

  if (exe_ctx.HasCJThreadScope()) {
    Process::StopLocker stop_locker;
    if (stop_locker.TryLock(&exe_ctx.GetProcessPtr()->GetRunLock())) {
      CJThread *cjthread = exe_ctx.GetCJThreadPtr();
      frame_sp = cjthread->GetStackFrameAtIndex(idx);
      if (frame_sp) {
        cjthread->SetSelectedFrame(frame_sp.get());
        sb_frame.SetFrameSP(frame_sp);
      }
    }
  }

  return sb_frame;
}

bool SBCJThread::operator==(const SBCJThread &rhs) const {
  LLDB_INSTRUMENT_VA(this, rhs);

  return m_opaque_sp->GetThreadSP().get() ==
         rhs.m_opaque_sp->GetThreadSP().get();
}

bool SBCJThread::operator!=(const SBCJThread &rhs) const {
  LLDB_INSTRUMENT_VA(this, rhs);

  return m_opaque_sp->GetThreadSP().get() !=
         rhs.m_opaque_sp->GetThreadSP().get();
}

bool SBCJThread::GetDescription(SBStream &description) const {
  LLDB_INSTRUMENT_VA(this, description);

  return GetDescription(description, false);
}

bool SBCJThread::GetDescription(SBStream &description, bool stop_format) const {
  LLDB_INSTRUMENT_VA(this, description, stop_format);

  Stream &strm = description.ref();

  std::unique_lock<std::recursive_mutex> lock;
  ExecutionContext exe_ctx(m_opaque_sp.get(), lock);

  if (exe_ctx.HasCJThreadScope()) {
    exe_ctx.GetThreadPtr()->DumpUsingSettingsFormat(strm,
                                                    LLDB_INVALID_THREAD_ID,
                                                    stop_format);
  } else
    strm.PutCString("No value");

  return true;
}


lldb_private::Thread *SBCJThread::operator->() {
  return get();
}

lldb_private::Thread *SBCJThread::get() {
  return m_opaque_sp->GetThreadSP().get();
}

