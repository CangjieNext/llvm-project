//===-- SBCJThread.h ----------------------------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_API_SBCJTHREAD_H
#define LLDB_API_SBCJTHREAD_H

#include "lldb/API/SBDefines.h"

#include <cstdio>

namespace lldb {

class SBFrame;

class LLDB_API SBCJThread {
public:

  SBCJThread();

  SBCJThread(const lldb::SBCJThread &thread);

  SBCJThread(const lldb::CJThreadSP &lldb_object_sp);

  ~SBCJThread();

  uint32_t GetNumFrames();

  lldb::SBFrame GetFrameAtIndex(uint32_t idx);

  lldb::SBFrame GetSelectedFrame();

  lldb::SBFrame SetSelectedFrame(uint32_t frame_idx);

  bool IsValid() const;

  void StepInstruction(bool step_over);

  void StepInstruction(bool step_over, SBError &error);

  void StepOver(lldb::RunMode stop_other_threads = lldb::eOnlyDuringStepping);

  void StepOver(lldb::RunMode stop_other_threads, SBError &error);

  void StepOut();

  void StepOut(SBError &error);

  void StepInto(lldb::RunMode stop_other_threads = lldb::eOnlyDuringStepping);

  void StepInto(const char *target_name,
                lldb::RunMode stop_other_threads = lldb::eOnlyDuringStepping);

  void StepInto(const char *target_name, uint32_t end_line, SBError &error,
                lldb::RunMode stop_other_threads = lldb::eOnlyDuringStepping);

  bool GetDescription(lldb::SBStream &description) const;

  bool GetDescription(SBStream &description, bool stop_format) const;

  lldb::StopReason GetStopReason();

  lldb::tid_t GetCJThreadID() const;

  const lldb::SBCJThread &operator=(const lldb::SBCJThread &rhs);

  bool operator==(const lldb::SBCJThread &rhs) const;

  operator bool() const;

  bool operator!=(const SBCJThread &rhs) const;

  void SetCJThread(const lldb::CJThreadSP &lldb_object_sp);

  void Clear();

private:

  SBError ResumeNewPlan(lldb_private::ExecutionContext &exe_ctx,
                        lldb_private::ThreadPlan *new_plan);

  lldb::ExecutionContextRefSP m_opaque_sp;

};

} // namespace lldb

#endif // LLDB_API_SBCJTHREAD_H
