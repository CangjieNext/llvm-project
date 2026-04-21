//===-- CJThread.h ---------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_TARGET_CJTHREAD_H
#define LLDB_TARGET_CJTHREAD_H

#include "lldb/Core/ValueObject.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/CJDynamicRegisterInfo.h"
#include "lldb/Target/RegisterContextUnwind.h"

namespace lldb_private {

enum class CJThreadState {
  eIdle = 0,
  eReady = 1,
  eRunning = 2,
  ePending = 3,
  eSyscall = 4,
  eUnknown = 5
};

inline llvm::StringRef FormatCJThreadState(CJThreadState state) {
    switch (state) {
    case CJThreadState::eIdle: return "idle";
    case CJThreadState::eReady: return "ready";
    case CJThreadState::eRunning: return "running";
    case CJThreadState::ePending: return "pending";
    case CJThreadState::eSyscall: return "syscall";
    default: return "unknown";
    }
  
  return "unknown";
}

struct CJThreadInfoOverview {
  uint64_t id;
  char name[32];
  uint64_t tid;
  CJThreadState state;
  lldb::addr_t thread_ptr;
  lldb::addr_t cjthread_ptr;
  lldb::addr_t context_ptr;
};

struct CJThreadInfoCommon;

/* The abstraction of cangjie's thread
 */
class CJThread : public Thread {
public:
  CJThread(Process &process, CJThreadInfoOverview cjtinfo, bool use_invalid_index_id = false);
  ~CJThread();

  lldb::tid_t GetHostThreadID() const { return m_cjthread_info.tid; }

  lldb::tid_t GetCJThreadID() const { return m_cjthread_info.id; }

  CJThreadState GetCJThreadState() const { return m_cjthread_info.state; }

  virtual lldb::user_id_t GetProtocolID() const override;

  virtual void RefreshStateAfterStop() override;

  virtual const char *GetName() override;

  virtual const char *GetQueueName() override;

  virtual lldb::RegisterContextSP GetRegisterContext() override;

  virtual lldb::RegisterContextSP
  CreateRegisterContextForFrame(StackFrame *frame) override;

  virtual bool CalculateStopInfo() override;

  bool BindCJThreadToOSThread(Process &process);

  bool UnBindCJThreadToOSThread(Process &process);

  void UpdateInfo(const CJThreadInfoOverview &info);

  static bool WaitUntilCJThreadScheduled(Target &target, lldb::CJThreadSP selected_thread, Status error);

  static llvm::StringRef type_decl_of_cjcontext_x86_linux;
  static llvm::StringRef type_decl_of_cjcontext_x86_windows;
  static llvm::StringRef type_decl_of_cjcontext_arm64;
  static llvm::StringRef type_decl_of_cjthreadinfo;

private:
  static bool RetrieveRegisterInfo(Process &process, CJDynamicRegisterInfoSP cjreginfo);
  UnwindLLDB m_unwinder;
  CJThreadInfoOverview m_cjthread_info;
  std::shared_ptr<std::map<ConstString, RegisterValue>> m_regvals;
  std::shared_ptr<CJDynamicRegisterInfo> m_cjreginfo;
};

}
#endif /* LLDB_TARGET_CJTHREAD_H */