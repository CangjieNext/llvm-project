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
#include "lldb/Utility/Instrumentation.h"
#include "lldb/Symbol/CompileUnit.h"
#include "Utils.h"

using namespace lldb;
using namespace lldb_private;

SBCJThread::SBCJThread() : m_opaque_sp(new ExecutionContextRef()) {
  LLDB_INSTRUMENT_VA(this);
}

SBCJThread::SBCJThread(const lldb::CJThreadSP &lldb_object_sp)
    : m_opaque_sp(new ExecutionContextRef(lldb_object_sp)) {
  LLDB_INSTRUMENT_VA(this, lldb_object_sp);
}

SBCJThread::SBCJThread(const SBCJThread &rhs) {
  LLDB_INSTRUMENT_VA(this, rhs);

  m_opaque_sp = clone(rhs.m_opaque_sp);
}

const lldb::SBCJThread &SBCJThread::operator=(const SBCJThread &rhs) {
  LLDB_INSTRUMENT_VA(this, rhs);

  if (this != &rhs)
    m_opaque_sp = clone(rhs.m_opaque_sp);
  return *this;
}

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

void SBCJThread::SetCJThread(const lldb::CJThreadSP &lldb_object_sp) {
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
  if (!cjthread) {
    error.SetErrorString("No cjthread in SBCJThread::StepInstruction");
    return;
  }
  Status new_plan_status;
  if (cjthread->GetCJThreadState() != CJThreadState::eRunning) {
    CJThread::WaitUntilCJThreadScheduled(exe_ctx.GetTargetRef(), cjthread, new_plan_status);
  }
  ProcessSP process_sp = exe_ctx.GetProcessSP();
  if (!process_sp) {
    error.SetErrorString("No process in SBCJThread::StepInstruction");
    return;
  }
  ThreadSP thread = process_sp->GetThreadList().GetSelectedThread();
  if (!thread) {
    error.SetErrorString("No thread in SBCJThread::StepInstruction");
    return;
  }
  ThreadPlanSP new_plan_sp(thread->QueueThreadPlanForStepSingleInstruction(
      step_over, true, true, new_plan_status));

  if (new_plan_status.Success()) {
    if (new_plan_sp) {
      error = ResumeNewPlan(exe_ctx, new_plan_sp.get());
    } else {
      error.SetErrorString("Failed to create thread plan");
    }
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
  if (!cjthread) {
    error.SetErrorString("No cjthread in SBCJThread::StepOver");
    return;
  }
  Status new_plan_status;
  if (cjthread->GetCJThreadState() != CJThreadState::eRunning) {
    CJThread::WaitUntilCJThreadScheduled(exe_ctx.GetTargetRef(), cjthread, new_plan_status);
  }
  ProcessSP process_sp = exe_ctx.GetProcessSP();
  if (!process_sp) {
    error.SetErrorString("No process in SBCJThread::StepOver");
    return;
  }
  ThreadSP thread = process_sp->GetThreadList().GetSelectedThread();
  if (!thread) {
    error.SetErrorString("No thread in SBCJThread::StepOver");
    return;
  }
  bool abort_other_plans = false;
  StackFrameSP frame_sp(thread->GetStackFrameAtIndex(0));

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
  if (new_plan_sp) {
    error = ResumeNewPlan(exe_ctx, new_plan_sp.get());
  } else {
    error.SetErrorString("Failed to create thread plan");
  }
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
  if (!cjthread) {
    error.SetErrorString("No cjthread in SBCJThread::StepInto");
    return;
  }
  Status new_plan_status;
  if (cjthread->GetCJThreadState() != CJThreadState::eRunning) {
    CJThread::WaitUntilCJThreadScheduled(exe_ctx.GetTargetRef(), cjthread, new_plan_status);
  }
  ProcessSP process_sp = exe_ctx.GetProcessSP();
  if (!process_sp) {
    error.SetErrorString("No process in SBCJThread::StepInto");
    return;
  }
  ThreadSP thread = process_sp->GetThreadList().GetSelectedThread();
  if (!thread) {
    error.SetErrorString("No thread in SBCJThread::StepInto");
    return;
  }
  StackFrameSP frame_sp(thread->GetStackFrameAtIndex(0));
  ThreadPlanSP new_plan_sp;

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

  if (new_plan_status.Success()) {
    if (new_plan_sp) {
      error = ResumeNewPlan(exe_ctx, new_plan_sp.get());
    } else {
      error.SetErrorString("Failed to create thread plan");
    }
  } else {
    error.SetErrorString(new_plan_status.AsCString());
  }
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
    CJThread::WaitUntilCJThreadScheduled(exe_ctx.GetTargetRef(), cjthread, new_plan_status);
  }
  ProcessSP process_sp = exe_ctx.GetProcessSP();
  if (!process_sp) {
    error.SetErrorString("No process in SBCJThread::StepOut");
    return;
  }
  ThreadSP thread = process_sp->GetThreadList().GetSelectedThread();
  if (!thread) {
    error.SetErrorString("No thread in SBCJThread::StepOut");
    return;
  }

  const LazyBool avoid_no_debug = eLazyBoolCalculate;
  ThreadPlanSP new_plan_sp(thread->QueueThreadPlanForStepOut(
      abort_other_plans, nullptr, false, stop_other_threads, eVoteYes,
      eVoteNoOpinion, 0, new_plan_status, avoid_no_debug));

  if (new_plan_status.Success())
    error = ResumeNewPlan(exe_ctx, new_plan_sp.get());
  else
    error.SetErrorString(new_plan_status.AsCString());
}

uint32_t SBCJThread::GetNumFrames() {
  LLDB_INSTRUMENT_VA(this);

  uint32_t num_frames = 0;
  std::unique_lock<std::recursive_mutex> lock;
  ExecutionContext exe_ctx(m_opaque_sp.get(), lock);

  if (exe_ctx.HasCJThreadScope()) {
    Process *process = exe_ctx.GetProcessPtr();
    if (!process) {
      return num_frames;
    }
    Process::StopLocker stop_locker;
    if (stop_locker.TryLock(&process->GetRunLock())) {
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
    Process *process = exe_ctx.GetProcessPtr();
    if (!process) {
      return sb_frame;
    }
    Process::StopLocker stop_locker;
    if (stop_locker.TryLock(&process->GetRunLock())) {
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
    Process *process = exe_ctx.GetProcessPtr();
    if (!process) {
      return sb_frame;
    }
    Process::StopLocker stop_locker;
    if (stop_locker.TryLock(&process->GetRunLock())) {
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
    Process *process = exe_ctx.GetProcessPtr();
    if (!process) {
      return sb_frame;
    }
    Process::StopLocker stop_locker;
    if (stop_locker.TryLock(&process->GetRunLock())) {
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
    exe_ctx.GetCJThreadPtr()->DumpUsingSettingsFormat(strm,
                                                    LLDB_INVALID_THREAD_ID,
                                                    stop_format);
  } else
    strm.PutCString("No value");

  return true;
}

lldb::StopReason SBCJThread::GetStopReason() {
  LLDB_INSTRUMENT_VA(this);

  StopReason reason = eStopReasonInvalid;
  std::unique_lock<std::recursive_mutex> lock;
  ExecutionContext exe_ctx(m_opaque_sp.get(), lock);

  if (exe_ctx.HasCJThreadScope()) {
    Process::StopLocker stop_locker;
    if (stop_locker.TryLock(&exe_ctx.GetProcessPtr()->GetRunLock())) {
      return exe_ctx.GetCJThreadPtr()->GetStopReason();
    }
  }

  return reason;
}

lldb::tid_t SBCJThread::GetCJThreadID() const {
  LLDB_INSTRUMENT_VA(this);

  lldb::tid_t cjthread_id = LLDB_INVALID_THREAD_ID;
  std::unique_lock<std::recursive_mutex> lock;
  ExecutionContext exe_ctx(m_opaque_sp.get(), lock);

  if (exe_ctx.HasCJThreadScope()) {
    CJThread *cjthread = exe_ctx.GetCJThreadPtr();
    if (cjthread) {
      cjthread_id = cjthread->GetCJThreadID();
    }
  }

  return cjthread_id;
}

lldb::tid_t SBCJThread::GetHostThreadID() const {
  LLDB_INSTRUMENT_VA(this);

  lldb::tid_t host_thread_id = LLDB_INVALID_THREAD_ID;
  std::unique_lock<std::recursive_mutex> lock;
  ExecutionContext exe_ctx(m_opaque_sp.get(), lock);

  if (exe_ctx.HasCJThreadScope()) {
    CJThread *cjthread = exe_ctx.GetCJThreadPtr();
    if (cjthread) {
      host_thread_id = cjthread->GetHostThreadID();
    }
  }

  return host_thread_id;
}

const char *SBCJThread::GetName() const {
  LLDB_INSTRUMENT_VA(this);

  const char *name = nullptr;
  std::unique_lock<std::recursive_mutex> lock;
  ExecutionContext exe_ctx(m_opaque_sp.get(), lock);

  if (exe_ctx.HasCJThreadScope()) {
    Process::StopLocker stop_locker;
    if (stop_locker.TryLock(&exe_ctx.GetProcessPtr()->GetRunLock())) {
      name = exe_ctx.GetCJThreadPtr()->GetName();
    }
  }

  return name;
}
