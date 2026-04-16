//===-- CJThread.cpp ---------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#include "lldb/API/LLDB.h"
#include "lldb/Core/ValueObject.h"
#include "lldb/Expression/UserExpression.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/CJRegisterContext.h"
#include "lldb/Target/CJThread.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContextUnwind.h"
#include "lldb/Target/StopInfo.h"
#include "lldb/Target/Thread.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/lldb-private-types.h"
#include "llvm/ADT/StringRef.h"
#include "lldb/Core/Module.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Core/ValueObjectVariable.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"

using namespace lldb_private;

static size_t GetVariableCallback(void *baton, const char *name,
                                  VariableList &variable_list) {
  size_t old_size = variable_list.GetSize();
  Target *target = static_cast<Target *>(baton);
  if (target)
    target->GetImages().FindGlobalVariables(ConstString(name), UINT32_MAX,
                                            variable_list);
  return variable_list.GetSize() - old_size;
}

struct CJThreadLayoutOffsets {
    int offset_of_ScheduleManager_dot_allCJThreadList;
    int offset_of_CJThread_dot_allCJThreadDulink;
    int offset_of_CJThread_dot_context;
    int offset_of_CJThread_dot_thread;
    int offset_of_CJThread_dot_state;
    int offset_of_CJThread_dot_id;
    int offset_of_CJThread_dot_name;
    int offset_of_Thread_dot_tid;
    int offset_of_boundThread;
    int offset_of_boundCJThread;
};

constexpr CJThreadLayoutOffsets offsets_x86_linux = {
    .offset_of_ScheduleManager_dot_allCJThreadList = 128,
    .offset_of_CJThread_dot_allCJThreadDulink = 120,
    .offset_of_CJThread_dot_context = 24,
    .offset_of_CJThread_dot_thread = 16,
    .offset_of_CJThread_dot_state = 96,
    .offset_of_CJThread_dot_id = 336,
    .offset_of_CJThread_dot_name = 344,
    .offset_of_Thread_dot_tid = 104,
    .offset_of_boundThread = 0x138,
    .offset_of_boundCJThread = 0xc8
};

constexpr CJThreadLayoutOffsets offsets_x86_windows = {
    .offset_of_ScheduleManager_dot_allCJThreadList = 64,
    .offset_of_CJThread_dot_allCJThreadDulink = 320,
    .offset_of_CJThread_dot_context = 24,
    .offset_of_CJThread_dot_thread = 16,
    .offset_of_CJThread_dot_state = 296,
    .offset_of_CJThread_dot_id = 536,
    .offset_of_CJThread_dot_name = 544,
    .offset_of_Thread_dot_tid = 80,
    .offset_of_boundThread = 376,
    .offset_of_boundCJThread = 512
};

constexpr CJThreadLayoutOffsets offsets_aarch64_linux = {
    .offset_of_ScheduleManager_dot_allCJThreadList = 144,
    .offset_of_CJThread_dot_allCJThreadDulink = 320,
    .offset_of_CJThread_dot_context = 24,
    .offset_of_CJThread_dot_thread = 16,
    .offset_of_CJThread_dot_state = 296,
    .offset_of_CJThread_dot_id = 536,
    .offset_of_CJThread_dot_name = 544,
    .offset_of_Thread_dot_tid = 80,
    .offset_of_boundThread = 376,
    .offset_of_boundCJThread = 512
};

constexpr CJThreadLayoutOffsets offsets_aarch64_darwin = {
    .offset_of_ScheduleManager_dot_allCJThreadList = 176,
    .offset_of_CJThread_dot_allCJThreadDulink = 240,
    .offset_of_CJThread_dot_context = 24,
    .offset_of_CJThread_dot_thread = 16,
    .offset_of_CJThread_dot_state = 216,
    .offset_of_CJThread_dot_id = 456,
    .offset_of_CJThread_dot_name = 464,
    .offset_of_Thread_dot_tid = 80,
    .offset_of_boundThread = 432,
    .offset_of_boundCJThread = 296
};

static bool ReadMemoryChecked(Process &process, lldb::addr_t addr, void *dst,
                              size_t size, Status &status) {
  size_t bytes_read = process.ReadMemory(addr, dst, size, status);
  if (status.Fail() || bytes_read != size) {
    return false;
  }
  return true;
}

lldb::addr_t FindSymbolLoadAddressInProcess(Process &process,
                                      const char *name_cstr) {
  if (!name_cstr || !name_cstr[0])
    return LLDB_INVALID_ADDRESS;

  Target &target = process.GetTarget();
  ConstString name(name_cstr);

  // ------------------------------------------------------------
  // 1) Try DWARF variable path (original behavior)
  // ------------------------------------------------------------
  {
    VariableList variable_list;
    ValueObjectList valobj_list;
    ExecutionContextScope *scope = &process;

    Variable::GetValuesForVariableExpressionPath(
        name_cstr,
        scope,
        GetVariableCallback,
        &target,
        variable_list,
        valobj_list);

    if (valobj_list.GetSize() > 0) {
      lldb::ValueObjectSP vo = valobj_list.GetValueObjectAtIndex(0);
      if (vo) {
        lldb::addr_t addr = vo->GetAddressOf();
        if (addr != LLDB_INVALID_ADDRESS && addr != 0)
          return addr;

        addr = vo->GetValueAsUnsigned(LLDB_INVALID_ADDRESS);
        if (addr != LLDB_INVALID_ADDRESS && addr != 0)
          return addr;
      }
    }
  }

  // ------------------------------------------------------------
  // 2) Fallback: symbol table (.dyn / dlsym-visible exports)
  // ------------------------------------------------------------
  SymbolContextList sc_list;

  // Prefer data symbols (global variables)
  target.GetImages().FindSymbolsWithNameAndType(
      name, lldb::eSymbolTypeData, sc_list);

  // Relax the filter if needed
  if (sc_list.GetSize() == 0) {
    target.GetImages().FindSymbolsWithNameAndType(
        name, lldb::eSymbolTypeAny, sc_list);
  }

  if (sc_list.GetSize() == 0)
    return LLDB_INVALID_ADDRESS;

  SymbolContext sc;
  sc_list.GetContextAtIndex(0, sc);

  if (!sc.symbol)
    return LLDB_INVALID_ADDRESS;

  Address addr = sc.symbol->GetAddress();
  lldb::addr_t load_addr = addr.GetLoadAddress(&target);

  if (load_addr == LLDB_INVALID_ADDRESS || load_addr == 0)
    return LLDB_INVALID_ADDRESS;

  return load_addr;
}

std::vector<CJThreadInfoOverview> CollectAllCJThreads(Process &process,
                                                      Status &status) {
  std::vector<CJThreadInfoOverview> results;
  lldb::addr_t base_addr = FindSymbolLoadAddressInProcess(process, "CJ_GScheduleManager");

  if (base_addr == LLDB_INVALID_ADDRESS) {
    status.SetErrorString("Could not resolve CJ_GScheduleManager load address");
    return results;
  }

  llvm::Triple triple = process.GetSystemArchitecture().GetTriple();
  CJThreadLayoutOffsets offsets;

  if (triple.getArch() == llvm::Triple::ArchType::x86_64) {
    if (triple.getOS() == llvm::Triple::OSType::Win32) {
      offsets = offsets_x86_windows;
    } else if (triple.getOS() == llvm::Triple::OSType::Linux) {
      offsets = offsets_x86_linux;
    } else {
      status.SetErrorString("Unsupported OS for x86_64");
      return results;
    }
  } else if (triple.getArch() == llvm::Triple::ArchType::aarch64) {
    if (triple.getOS() == llvm::Triple::OSType::MacOSX) {
      offsets = offsets_aarch64_darwin;
    } else if (triple.getOS() == llvm::Triple::OSType::Linux) {
      offsets = offsets_aarch64_linux;
    } else {
      status.SetErrorString("Unsupported OS for aarch64");
      return results;
    }
  } else {
    status.SetErrorString("Unsupported architecture");
    return results;
  }

  // allCJThreadList is a dulink list head inside ScheduleManager
  lldb::addr_t list_head =
      base_addr + offsets.offset_of_ScheduleManager_dot_allCJThreadList;

  // First dulink entry (head->next)
  lldb::addr_t dulink_entry = 0;
  if (!ReadMemoryChecked(process, list_head + 8, &dulink_entry,
                         sizeof(dulink_entry), status))
    return results;

  // Traverse the circular doubly linked list
  while (dulink_entry != list_head && dulink_entry != 0) {
    lldb::addr_t cjthread_addr =
        dulink_entry - offsets.offset_of_CJThread_dot_allCJThreadDulink;

    CJThreadInfoOverview overview{};
    overview.cjthread_ptr = cjthread_addr;

    // CJThread::state
    if (!ReadMemoryChecked(
            process, cjthread_addr + offsets.offset_of_CJThread_dot_state,
            &overview.state, sizeof(overview.state), status))
      break;

    if (!ReadMemoryChecked(
            process, cjthread_addr + offsets.offset_of_CJThread_dot_name,
            &overview.name, 32, status))
      break;

    // CJThread::thread
    if (!ReadMemoryChecked(
            process, cjthread_addr + offsets.offset_of_CJThread_dot_thread,
            &overview.thread_ptr, sizeof(overview.thread_ptr), status))
      break;

    // CJThread::context
    overview.context_ptr = cjthread_addr + offsets.offset_of_CJThread_dot_context;

    // CJThread::id
    if (!ReadMemoryChecked(process, cjthread_addr + offsets.offset_of_CJThread_dot_id,
                           &overview.id, sizeof(overview.id), status))
      break;

    // Thread::tid (if thread_ptr is valid)
    overview.tid = 0;
    if (overview.thread_ptr != 0) {
      uint64_t tid_val = 0;
      if (!ReadMemoryChecked(process,
                             overview.thread_ptr + offsets.offset_of_Thread_dot_tid,
                             &tid_val, sizeof(tid_val), status))
        break;
      overview.tid = tid_val;
    }

    results.push_back(overview);

    // Move to next dulink (entry->next)
    lldb::addr_t next_entry = 0;
    if (!ReadMemoryChecked(process, dulink_entry + 8, &next_entry,
                           sizeof(next_entry), status))
      break;

    dulink_entry = next_entry;
  }

  return results;
}

bool Process::RefreshCJThreadList(Status &error, bool forceRefresh) {
  if (!forceRefresh && m_cjthreadlist_state == CJThreadListState::HasBeenRefreshed)
    return true;

  std::vector<CJThreadInfoOverview> info_snap = CollectAllCJThreads(*this, error);
  if (error.Fail()) return false;

  ThreadList &cjtlist = GetCJThreadList();
  cjtlist.Clear();
  for (unsigned i = 0; i < info_snap.size(); i ++) {
    auto cjth = std::make_shared<CJThread>(*this, info_snap[i]);
    cjtlist.AddThread(cjth);
  }

  m_cjthreadlist_state = CJThreadListState::HasBeenRefreshed;
  return true;
}

bool CJThread::RetrieveRegisterInfo(Process &process, CJDynamicRegisterInfoSP cjreginfo) {
    // assume that all threads share the same register info
    std::vector<DynamicRegisterInfo::Register> registers;
    ThreadList &tlist = process.GetThreadList();
    if (!tlist.GetSize()) return false;
    lldb::ThreadSP firstThread = tlist.GetThreadAtIndex(0);
    if (!firstThread) return false;
    lldb::RegisterContextSP reg_ctx = firstThread->GetRegisterContext();
    if (!reg_ctx) return false;

    for (int i = 0, n = reg_ctx->GetRegisterCount(); i < n; i ++) {
        const RegisterInfo *reg_info = reg_ctx->GetRegisterInfoAtIndex(i);

        std::vector<uint32_t> value_regs, invalidate_regs;
        for (uint32_t *reg = reg_info->value_regs; reg && *reg != LLDB_INVALID_REGNUM; ++reg)
            value_regs.push_back(*reg);
        for (uint32_t *reg = reg_info->invalidate_regs; reg && *reg != LLDB_INVALID_REGNUM; ++reg)
            invalidate_regs.push_back(*reg);

        registers.emplace_back(DynamicRegisterInfo::Register{
            .name = ConstString(reg_info->name),
            .alt_name = ConstString(reg_info->alt_name),
            .byte_size = reg_info->byte_size,
            .byte_offset = reg_info->byte_offset,
            .encoding = reg_info->encoding,
            .format = reg_info->format,
            .regnum_dwarf = reg_info->kinds[lldb::eRegisterKindDWARF],
            .regnum_ehframe = reg_info->kinds[lldb::eRegisterKindEHFrame],
            .regnum_generic = reg_info->kinds[lldb::eRegisterKindGeneric],
            .regnum_remote = reg_info->kinds[lldb::eRegisterKindProcessPlugin],
            .value_regs = std::move(value_regs),
            .invalidate_regs = std::move(invalidate_regs),
        });
    }

    for (int i = 0, n = reg_ctx->GetRegisterSetCount(); i < n; i++) {
        const RegisterSet *rset = reg_ctx->GetRegisterSet(i);
        for (int j = 0; j < (int)rset->num_registers; j ++) {
            int regno = rset->registers[j];
            registers[regno].set_name = ConstString(rset->name);
        }
    }

    cjreginfo->SetRegisterInfo(std::move(registers), process.GetSystemArchitecture());
    return true;
}

CJThread::CJThread(Process &process, CJThreadInfoOverview cjtinfo, bool use_invalid_index_id)
  : Thread(process, cjtinfo.id, use_invalid_index_id), m_unwinder(*this), m_cjthread_info(cjtinfo)
{
  m_cjreginfo = std::make_shared<CJDynamicRegisterInfo>();
  RetrieveRegisterInfo(process, m_cjreginfo);

  /* retrieve register value */
  Status error;
  llvm::Triple triple = process.GetSystemArchitecture().GetTriple();
  m_regvals = std::make_shared<std::map<ConstString, RegisterValue>>();
  if (triple.getArch() == llvm::Triple::ArchType::x86_64 &&
      triple.getOS() == llvm::Triple::OSType::Win32) {
    CJThreadContext_Windows_X64 ctx{};
    size_t n = process.ReadMemory(m_cjthread_info.context_ptr, &ctx, sizeof(ctx), error);
    if (!error.Fail() && n == sizeof(ctx))
      ctx.CopyTo(*m_regvals, process.GetByteOrder());
  } else if (triple.getArch() == llvm::Triple::ArchType::x86_64) {
    CJThreadContext_Linux_X64 ctx{};
    size_t n = process.ReadMemory(m_cjthread_info.context_ptr, &ctx, sizeof(ctx), error);
    if (!error.Fail() && n == sizeof(ctx))
      ctx.CopyTo(*m_regvals, process.GetByteOrder());
  } else if (triple.getArch() == llvm::Triple::ArchType::aarch64) {
    CJThreadContext_Arm64 ctx{};
    size_t n = process.ReadMemory(m_cjthread_info.context_ptr, &ctx, sizeof(ctx), error);
    if (!error.Fail() && n == sizeof(ctx))
      ctx.CopyTo(*m_regvals, process.GetByteOrder());
  }
  m_reg_context_sp =
      std::make_shared<CJRegisterContext>(*this, m_cjreginfo, m_regvals);
}

CJThread::~CJThread() {
  DestroyThread();
}

lldb::user_id_t CJThread::GetProtocolID() const {
  return static_cast<lldb::user_id_t>(GetHostThreadID());
}

void CJThread::RefreshStateAfterStop() {
}

const char *CJThread::GetName() {
    return m_cjthread_info.name;
}

const char *CJThread::GetQueueName() {
    return "";
}

lldb::RegisterContextSP CJThread::GetRegisterContext() {
    if (m_cjthread_info.state == CJThreadState::eRunning) {
      lldb::ThreadSP holding_osthread_sp = GetProcess()->GetThreadList().FindThreadByID(GetHostThreadID());
      if (holding_osthread_sp) {
        return holding_osthread_sp->GetRegisterContext();
      }
    }
    return m_reg_context_sp;
}

lldb::RegisterContextSP CJThread::CreateRegisterContextForFrame(StackFrame *frame) {
  lldb::RegisterContextSP reg_ctx_sp;
  uint32_t concrete_frame_idx = 0;
  if (frame)
    concrete_frame_idx = frame->GetConcreteFrameIndex();

  if (concrete_frame_idx == 0) {
    return GetRegisterContext();
  } else {
    reg_ctx_sp = m_unwinder.CreateRegisterContextForFrame(frame);
  }

  return reg_ctx_sp;
}

bool CJThread::CalculateStopInfo() {
  if (m_stop_info_sp && m_stop_info_sp->IsValid())
    return true;

  m_stop_info_sp = StopInfo::CreateStopReasonToTrace(*reinterpret_cast<Thread*>(this));
  return (m_stop_info_sp && m_stop_info_sp->IsValid());
}

bool CJThread::BindCJThreadToOSThread(Process &process) {
  if (m_cjthread_info.state != CJThreadState::eRunning) {
    return false;
  }

  llvm::Triple triple = process.GetSystemArchitecture().GetTriple();
  CJThreadLayoutOffsets offsets;

  if (triple.getArch() == llvm::Triple::ArchType::x86_64) {
    if (triple.getOS() == llvm::Triple::OSType::Win32) {
      offsets = offsets_x86_windows;
    } else if (triple.getOS() == llvm::Triple::OSType::Linux) {
      offsets = offsets_x86_linux;
    } else {
      return false;
    }
  } else if (triple.getArch() == llvm::Triple::ArchType::aarch64) {
    if (triple.getOS() == llvm::Triple::OSType::MacOSX) {
      offsets = offsets_aarch64_darwin;
    } else if (triple.getOS() == llvm::Triple::OSType::Linux) {
      offsets = offsets_aarch64_linux;
    } else {
      return false;
    }
  } else {
    return false;
  }

  Status error;
  lldb::addr_t addr = 0;
  process.WriteMemory(m_cjthread_info.cjthread_ptr + offsets.offset_of_boundThread,
                      &m_cjthread_info.thread_ptr, sizeof(lldb::addr_t), error);
  if (error.Fail())
    return false;
  process.WriteMemory(m_cjthread_info.thread_ptr + offsets.offset_of_boundCJThread,
                      &m_cjthread_info.cjthread_ptr, sizeof(lldb::addr_t), error);
  if (error.Fail())
    return false;
  process.SetBindCJThreadID(GetCJThreadID());
  Log *log = GetLog(LLDBLog::Step);
  if (log) {
    LLDB_LOGF(log, "bind cjthread %lld to os thread %lld", GetCJThreadID(), GetHostThreadID());
  }
  return true;
}

bool CJThread::UnBindCJThreadToOSThread(Process &process) {
  llvm::Triple triple = process.GetSystemArchitecture().GetTriple();
  CJThreadLayoutOffsets offsets;

  if (triple.getArch() == llvm::Triple::ArchType::x86_64) {
    if (triple.getOS() == llvm::Triple::OSType::Win32) {
      offsets = offsets_x86_windows;
    } else if (triple.getOS() == llvm::Triple::OSType::Linux) {
      offsets = offsets_x86_linux;
    } else {
      return false;
    }
  } else if (triple.getArch() == llvm::Triple::ArchType::aarch64) {
    if (triple.getOS() == llvm::Triple::OSType::MacOSX) {
      offsets = offsets_aarch64_darwin;
    } else if (triple.getOS() == llvm::Triple::OSType::Linux) {
      offsets = offsets_aarch64_linux;
    } else {
      return false;
    }
  } else {
    return false;
  }

  Status error;
  lldb::addr_t addr = 0;
  process.WriteMemory(m_cjthread_info.cjthread_ptr + offsets.offset_of_boundThread,
                      &addr, sizeof(lldb::addr_t), error);
  if (error.Fail())
    return false;
  process.WriteMemory(m_cjthread_info.thread_ptr + offsets.offset_of_boundCJThread,
                      &addr, sizeof(lldb::addr_t), error);
  if (error.Fail())
    return false;
  process.SetBindCJThreadID(UINT64_MAX);
  Log *log = GetLog(LLDBLog::Step);
  if (log) {
    LLDB_LOGF(log, "Unbind cjthread %lld", GetCJThreadID());
  }
  return true;
}

class ContinueUntil {
  Target &target;
  lldb::break_id_t m_until_bp_id;
  std::vector<lldb::break_id_t> m_disabled_points;
public:
  ContinueUntil(Target &target, lldb::CJThreadSP cjthread)
  : target(target), m_until_bp_id(LLDB_INVALID_BREAK_ID) {
    size_t num_bps = target.GetBreakpointList().GetSize();
    for (size_t i = 0; i < num_bps; ++i) {
        lldb::BreakpointSP bp = target.GetBreakpointList().GetBreakpointAtIndex(i);
        if (bp && bp->IsEnabled() && bp->IsInternal()) {
          m_disabled_points.push_back(bp->GetID());
          bp->SetEnabled(false);
        }
    }

    lldb::StackFrameSP frame = cjthread->GetStackFrameAtIndex(0);
    if (!frame) return;
    lldb::RegisterContextSP reg_ctx = frame->GetRegisterContext();
    if (!reg_ctx) return;
    lldb::addr_t break_addr = reg_ctx->GetPC();
    if (break_addr != LLDB_INVALID_ADDRESS) {
      lldb::BreakpointSP until_bp = target.CreateBreakpoint(break_addr, true, false);
      if (until_bp != nullptr) {
        m_until_bp_id = until_bp->GetID();
        until_bp->SetCJThreadID(cjthread->GetID());
        until_bp->SetBreakpointKind("continue-until-target");
      }
    }
  }

  bool ShouldStop(lldb::break_id_t id) {
    return m_until_bp_id == id;
  }

  ~ContinueUntil() {
    target.RemoveBreakpointByID(m_until_bp_id);
    for (lldb::break_id_t bid : m_disabled_points) {
      lldb::BreakpointSP bp = target.GetBreakpointByID(bid);
      if (bp) bp->SetEnabled(true);
    }
  }
};

bool CJThread::WaitUntilCJThreadScheduled(Target &target, lldb::CJThreadSP selected_thread, Status error) {
  unsigned saved_cjt_id = selected_thread->GetID();
  lldb::ProcessSP process_sp = selected_thread->GetProcess();
  ThreadList &cjtlist = process_sp->GetCJThreadList();
  ContinueUntil cont_until(target, selected_thread);

  Log *log = GetLog(LLDBLog::Step);
  if (log) {
    LLDB_LOGF(log, "WaitUntilCJThreadScheduled for %lld", selected_thread->GetCJThreadID());
  }

  do {
    StreamString stream_after_resume;
    process_sp->ResumeSynchronous(&stream_after_resume);
    lldb::StopInfoSP info = process_sp->GetThreadList().GetSelectedThread()->GetStopInfo();
    if (!info) continue;
    if (info->GetStopReason() != lldb::eStopReasonBreakpoint) continue;
    process_sp->RefreshCJThreadList(error, true);
    if (error.Fail()) {
      return false;
    }

    lldb::ThreadSP thread = process_sp->GetCJThreadList().FindThreadByID(saved_cjt_id);
    if (!thread) {
      error.SetErrorString("cjthread vanished while stepping instruction");
      return false;
    }
    
    lldb::CJThreadSP cjthread = std::dynamic_pointer_cast<CJThread>(thread);
    if (!cjthread) {
      error.SetErrorString("unexpected thread object occurs in CJThread list");
      return false;
    }

    if (cjthread->GetCJThreadState() == CJThreadState::eRunning &&
        cont_until.ShouldStop(info->GetBreakpointID())) {
      cjtlist.SetSelectedThreadByID(cjthread->GetID());
      ThreadList &thlist = process_sp->GetThreadList();
      lldb::ThreadSP thread = thlist.FindThreadByID(cjthread->GetHostThreadID());
      if (!thread) {
        error.SetErrorStringWithFormat("cjthread #{0}'s host thread #{1} is vanished",
          cjthread->GetID(), cjthread->GetHostThreadID());
        return false;
      }
      thlist.SetSelectedThreadByID(thread->GetID());
      return true;
    }
  } while (true);

  return false;
}
