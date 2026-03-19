//===-- CommandObjectThreadUtil.cpp -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CommandObjectThreadUtil.h"

#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/Thread.h"
#include "lldb/Target/CJThread.h"

using namespace lldb;
using namespace lldb_private;
using namespace llvm;

CommandObjectIterateOverThreads::CommandObjectIterateOverThreads(
    CommandInterpreter &interpreter, const char *name, const char *help,
    const char *syntax, uint32_t flags, bool use_cjthread)
    : CommandObjectParsed(interpreter, name, help, syntax, flags) {
  // These commands all take thread ID's as arguments.
  CommandArgumentData thread_arg{eArgTypeThreadIndex, eArgRepeatStar};
  m_arguments.push_back({thread_arg});
  m_use_cjthread = use_cjthread;
}

CommandObjectMultipleThreads::CommandObjectMultipleThreads(
    CommandInterpreter &interpreter, const char *name, const char *help,
    const char *syntax, uint32_t flags, bool use_cjthread)
    : CommandObjectParsed(interpreter, name, help, syntax, flags) {
  // These commands all take thread ID's as arguments.
  CommandArgumentData thread_arg{eArgTypeThreadIndex, eArgRepeatStar};
  m_arguments.push_back({thread_arg});
  m_use_cjthread = use_cjthread;
}

ThreadList &CommandObjectIterateOverThreads::getTargetThreadList() {
  Process *process = m_exe_ctx.GetProcessPtr();
  if (m_use_cjthread)
    return process->GetCJThreadList();
  else
    return process->GetThreadList();
}

bool CommandObjectIterateOverThreads::DoExecute(Args &command,
                                                CommandReturnObject &result) {
  result.SetStatus(m_success_return);

  bool all_threads = false;
  if (command.GetArgumentCount() == 0) {
    if (m_use_cjthread) {
      // this should be optimized using m_exe_ctx, but currently
      // 
      ThreadSP thread = m_exe_ctx.GetProcessPtr()->GetCJThreadList().GetSelectedThread();
      if (!thread || !HandleOneThread(thread->GetID(), result))
        return false;
      return result.Succeeded();
    } else {
      Thread *thread = m_exe_ctx.GetThreadPtr();
      if (!thread || !HandleOneThread(thread->GetID(), result))
        return false;
      return result.Succeeded();
    }
  } else if (command.GetArgumentCount() == 1) {
    all_threads = ::strcmp(command.GetArgumentAtIndex(0), "all") == 0;
    m_unique_stacks = ::strcmp(command.GetArgumentAtIndex(0), "unique") == 0;
  }

  // Use tids instead of ThreadSPs to prevent deadlocking problems which
  // result from JIT-ing code while iterating over the (locked) ThreadSP
  // list.
  std::vector<lldb::tid_t> tids;

  if (all_threads || m_unique_stacks) {
    Process *process = m_exe_ctx.GetProcessPtr();

    if (m_use_cjthread)
      for (ThreadSP thread_sp : process->CJThreads())
        tids.push_back(thread_sp->GetID());
    else
      for (ThreadSP thread_sp : process->Threads())
        tids.push_back(thread_sp->GetID());
  } else {
    const size_t num_args = command.GetArgumentCount();

    std::lock_guard<std::recursive_mutex> guard(
        getTargetThreadList().GetMutex());

    for (size_t i = 0; i < num_args; i++) {
      uint32_t thread_idx;
      if (!llvm::to_integer(command.GetArgumentAtIndex(i), thread_idx)) {
        result.AppendErrorWithFormat("invalid thread specification: \"%s\"\n",
                                     command.GetArgumentAtIndex(i));
        return false;
      }

      ThreadSP thread = getTargetThreadList().FindThreadByIndexID(thread_idx);

      if (!thread) {
        result.AppendErrorWithFormat("no thread with index: \"%s\"\n",
                                     command.GetArgumentAtIndex(i));
        return false;
      }

      tids.push_back(thread->GetID());
    }
  }

  if (m_unique_stacks) {
    // Iterate over threads, finding unique stack buckets.
    std::set<UniqueStack> unique_stacks;
    for (const lldb::tid_t &tid : tids) {
      if (!BucketThread(tid, unique_stacks, result)) {
        return false;
      }
    }

    // Write the thread id's and unique call stacks to the output stream
    Stream &strm = result.GetOutputStream();
    for (const UniqueStack &stack : unique_stacks) {
      // List the common thread ID's
      const std::vector<uint32_t> &thread_index_ids =
          stack.GetUniqueThreadIndexIDs();
      if (m_use_cjthread)
        strm.Format("{0} cangjie thread(s) ", thread_index_ids.size());
      else
        strm.Format("{0} thread(s) ", thread_index_ids.size());
      for (const uint32_t &thread_index_id : thread_index_ids) {
        strm.Format("#{0} ", thread_index_id);
      }
      strm.EOL();

      // List the shared call stack for this set of threads
      uint32_t representative_thread_id = stack.GetRepresentativeThread();
      ThreadSP thread = getTargetThreadList().FindThreadByIndexID(
            representative_thread_id);
      if (!HandleOneThread(thread->GetID(), result)) {
        return false;
      }
    }
  } else {
    uint32_t idx = 0;
    for (const lldb::tid_t &tid : tids) {
      if (idx != 0 && m_add_return)
        result.AppendMessage("");

      if (!HandleOneThread(tid, result))
        return false;

      ++idx;
    }
  }
  return result.Succeeded();
}

bool CommandObjectIterateOverThreads::BucketThread(
    lldb::tid_t tid, std::set<UniqueStack> &unique_stacks,
    CommandReturnObject &result) {
  // Grab the corresponding thread for the given thread id.
  Thread *thread = getTargetThreadList().FindThreadByID(tid).get();
  if (thread == nullptr) {
    result.AppendErrorWithFormatv("Failed to process thread #{0}.\n", tid);
    return false;
  }

  // Collect the each frame's address for this call-stack
  std::stack<lldb::addr_t> stack_frames;
  const uint32_t frame_count = thread->GetStackFrameCount();
  for (uint32_t frame_index = 0; frame_index < frame_count; frame_index++) {
    const lldb::StackFrameSP frame_sp =
        thread->GetStackFrameAtIndex(frame_index);
    const lldb::addr_t pc = frame_sp->GetStackID().GetPC();
    stack_frames.push(pc);
  }

  uint32_t thread_index_id = thread->GetIndexID();
  UniqueStack new_unique_stack(stack_frames, thread_index_id);

  // Try to match the threads stack to and existing entry.
  std::set<UniqueStack>::iterator matching_stack =
      unique_stacks.find(new_unique_stack);
  if (matching_stack != unique_stacks.end()) {
    matching_stack->AddThread(thread_index_id);
  } else {
    unique_stacks.insert(new_unique_stack);
  }
  return true;
}

ThreadList &CommandObjectMultipleThreads::getTargetThreadList() {
  Process *process = m_exe_ctx.GetProcessPtr();
  if (m_use_cjthread)
    return process->GetCJThreadList();
  else
    return process->GetThreadList();
}

bool CommandObjectMultipleThreads::DoExecute(Args &command,
                                             CommandReturnObject &result) {
  Process &process = m_exe_ctx.GetProcessRef();

  std::vector<lldb::tid_t> tids;
  const size_t num_args = command.GetArgumentCount();

  std::lock_guard<std::recursive_mutex> guard(
      getTargetThreadList().GetMutex());

  if (num_args > 0 && ::strcmp(command.GetArgumentAtIndex(0), "all") == 0) {
    if (m_use_cjthread)
      for (ThreadSP thread_sp : process.CJThreads())
        tids.push_back(thread_sp->GetID());
    else
      for (ThreadSP thread_sp : process.Threads())
        tids.push_back(thread_sp->GetID());
  } else {
    if (num_args == 0) {
      Thread &thread = m_exe_ctx.GetThreadRef();
      tids.push_back(thread.GetID());
    }

    for (size_t i = 0; i < num_args; i++) {
      uint32_t thread_idx;
      if (!llvm::to_integer(command.GetArgumentAtIndex(i), thread_idx)) {
        result.AppendErrorWithFormat("invalid thread specification: \"%s\"\n",
                                     command.GetArgumentAtIndex(i));
        return false;
      }

      ThreadSP thread = getTargetThreadList().FindThreadByIndexID(thread_idx);

      if (!thread) {
        result.AppendErrorWithFormat("no thread with index: \"%s\"\n",
                                     command.GetArgumentAtIndex(i));
        return false;
      }

      tids.push_back(thread->GetID());
    }
  }

  return DoExecuteOnThreads(command, result, tids);
}
