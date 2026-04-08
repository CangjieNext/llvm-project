//===-- CommandObjCJThreadCommon.cpp --------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#include "CommandObjCJThreadCommon.h"
#include "lldb/Target/RegisterContextUnwind.h"
#include "lldb/Expression/UserExpression.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/DataFormatters/DumpValueObjectOptions.h"

using namespace lldb;
using namespace lldb_private;

CommandObjCJThreadCommon::CommandObjCJThreadCommon() {
}

CommandObjCJThreadCommon::~CommandObjCJThreadCommon() {
}

const char *CommandObjCJThreadCommon::GetCJThreadInfoExp(Target *target) {
  auto arch_type = target->GetArchitecture().GetTriple().getArch();
  switch (arch_type) {
  case llvm::Triple::aarch64:
    return m_info_aarch64;
  case llvm::Triple::x86_64:
#if defined(__linux__) || defined(__APPLE__)
    return m_info_x86_64;
#endif
#if defined(_WIN32)
    return m_info_x86_64_win;
#endif
    return nullptr;
  default:
    break;
  }
  return nullptr;
}

static void SetUserExpressionOptions(lldb_private::EvaluateExpressionOptions &options, Target *target) {
  options.SetUseDynamic(target->GetPreferDynamicValue());
  options.SetUnwindOnError(true);
  options.SetIgnoreBreakpoints(true);
  options.SetLanguage(target->GetLanguage());
  options.SetCangjieUserExpr(false);
}

uint64_t CommandObjCJThreadCommon::GetCJThreadCount(ExecutionContext exe_ctx, Status &error) {
  ValueObjectSP count_sp;
  lldb_private::EvaluateExpressionOptions options;
  SetUserExpressionOptions(options, exe_ctx.GetTargetPtr());
  UserExpression::Evaluate(exe_ctx, options, m_count_exp, "", count_sp, error);

  if (error.Fail()) {
    return 0;
  }

  if (count_sp) {
    uint64_t count = count_sp->GetValueAsUnsigned(UINT64_MAX);
    if (count == UINT64_MAX) {
      error.SetErrorString("Get cjthread count failed\n");
      return 0;
    }
    m_cjthread_count = count;
    return m_cjthread_count;
  }

  return 0;
}

lldb::ValueObjectSP CommandObjCJThreadCommon::GetCJThreadInfo(ExecutionContext exe_ctx, Status &error) {
  auto info_base_exp = GetCJThreadInfoExp(exe_ctx.GetTargetPtr());
  if (info_base_exp == nullptr) {
    error.SetErrorString("command cjthread does not support for this arch type\n");
    return ValueObjectSP();
  }

  GetCJThreadCount(exe_ctx, error);
  if (error.Fail()) {
    return ValueObjectSP();
  }
  std::string cjthread_count = std::to_string(m_cjthread_count);
  std::string info_tail = std::string("CJThreadInfo cjdb_cjthread_info[") + cjthread_count +
    std::string("] = {0}; void *cjdb_thread_info_ptr = reinterpret_cast<void *>(cjdb_cjthread_info); "
    "(int)CJ_MRT_GetAllCJThreadInfo(cjdb_thread_info_ptr, ") + cjthread_count + std::string("); cjdb_cjthread_info;");
  std::string info_exp = std::string(info_base_exp) + std::string(m_info_common) + info_tail;

  ValueObjectSP info_sp;
  lldb_private::EvaluateExpressionOptions options;
  SetUserExpressionOptions(options, exe_ctx.GetTargetPtr());
  UserExpression::Evaluate(exe_ctx, options, info_exp.c_str(), "", info_sp, error);

  if (error.Fail()) {
    return ValueObjectSP();
  }

  m_cjthread_info = info_sp;
  return m_cjthread_info;
}

void CommandObjCJThreadCommon::SetCJThreadRegisterContext(ValueObjectSP cjthread_context,
                                                          ProcessSP process) {
  ValueObjectSP state_sp = cjthread_context->GetChildMemberWithName(ConstString("state"), true);
  auto state = state_sp->GetValueAsUnsigned(UINT64_MAX);
  if (state == UINT64_MAX) {
    process->CJThreadRegContextClear();
    return;
  }
  if (state != static_cast<uint64_t>(State::eRunning) &&
      state != static_cast<uint64_t>(State::eSyscall)) {
    process->SetCJThreadRegContext(m_regcontext);
  } else {
    process->CJThreadRegContextClear();
  }
}

void CommandObjCJThreadCommon::HandOneCJThread(ThreadSP thread, ValueObjectSP cjthread_context,
                                               Status &error, bool show_all_frames) {
  auto process = thread->GetProcess();
  m_regcontext.clear();

  if (cjthread_context) {
    auto reg_context = cjthread_context->GetChildMemberWithName(ConstString("context"), true);
    if (!reg_context) {
      error.SetErrorString("Failed to get reg context");
      return;
    }
    auto count = reg_context->GetNumChildren();
    for (size_t i = 0; i < count; i++) {
      auto reg = reg_context->GetChildAtIndex(i, true);
      m_regcontext.insert(std::make_pair(reg->GetName(), reg->GetValueAsUnsigned(UINT64_MAX)));
    }
  }

  SetCJThreadRegisterContext(cjthread_context, process);
  UnwindLLDB unwinder = UnwindLLDB(*thread);
  uint32_t idx = 0;
  m_frames.clear();
  do {
    lldb::addr_t pc = LLDB_INVALID_ADDRESS;
    lldb::addr_t cfa = LLDB_INVALID_ADDRESS;
    bool behaves_like_zeroth_frame = (idx == 0);
    const bool success = unwinder.GetFrameInfoAtIndex(idx, cfa, pc, behaves_like_zeroth_frame);
    if (!success) {
      // We've gotten to the end of the stack.
      break;
    }

    StackFrameSP unwind_frame_sp;
    if (idx == 0) {
      unwind_frame_sp = std::make_shared<StackFrame>(
        thread, m_frames.size(), idx, nullptr, cfa, pc,
        behaves_like_zeroth_frame, nullptr);
    } else {
      unwind_frame_sp = std::make_shared<StackFrame>(
        thread, m_frames.size(), idx, cfa, true,
        pc, StackFrame::Kind::Regular, behaves_like_zeroth_frame, nullptr);
    }
    m_frames.push_back(unwind_frame_sp);
    idx++;
  } while (show_all_frames);

  process->CJThreadRegContextClear();
  return;
}

const char *CommandObjCJThreadCommon::CJThreadStateToString(State state) {
  static std::map<State, std::string> state_map = {
    {State::eIdle, "idle"}, {State::eReady, "ready"}, {State::eRunning, "running"},
    {State::ePending, "pending"}, {State::eSyscall, "syscall"} };
  if (state_map.find(state) != state_map.end()) {
    return state_map[state].c_str();
  }

  return "unknown";
}

std::string CommandObjCJThreadCommon::GetCJThreadName(lldb::ValueObjectSP cjthread_value) {
  ValueObjectSP name_sp = cjthread_value->GetChildMemberWithName(ConstString("name"), true);
  if (name_sp == nullptr) {
    return "";
  }
  StreamString result;
  DumpValueObjectOptions option;
  option.SetHideRootType(true);
  option.SetVariableFormatDisplayLanguage(name_sp->GetPreferredDisplayLanguage());
  name_sp->Dump(result, option);
  std::string value = result.GetData();
  std::string prefix = " = \"";
  size_t start = value.find_first_of(prefix);
  size_t end = value.find_last_of("\"");
  std::string name;
  if (start != std::string::npos && end != std::string::npos && start < end) {
    name = value.substr(start + prefix.size(), end - start - prefix.size()).c_str();
  }

  return name;
}

uint64_t CommandObjCJThreadCommon::GetCJThreadID(lldb::ValueObjectSP cjthread_value) {
  ValueObjectSP id_sp = cjthread_value->GetChildMemberWithName(ConstString("id"), true);
  if (id_sp == nullptr) {
    return UINT64_MAX;
  }
  uint64_t id = id_sp->GetValueAsUnsigned(UINT64_MAX);
  if (id == UINT64_MAX) {
    return UINT64_MAX;
  }

  return id;
}

std::string CommandObjCJThreadCommon::GetCJThreadState(lldb::ValueObjectSP cjthread_value) {
  ValueObjectSP state_sp = cjthread_value->GetChildMemberWithName(ConstString("state"), true);
  if (state_sp == nullptr) {
    return "";
  }
  uint64_t state = state_sp->GetValueAsUnsigned(UINT64_MAX);
  if (state == UINT64_MAX) {
    return "";
  }

  return CJThreadStateToString(static_cast<State>(static_cast<int>(state)));
}

bool CommandObjCJThreadCommon::GetDescription(Stream &strm, Status &error, ValueObjectSP cjthread_value) {
    std::string name = GetCJThreadName(cjthread_value);
    std::string state = GetCJThreadState(cjthread_value);
    uint64_t id = GetCJThreadID(cjthread_value);
    if (id == UINT64_MAX) {
      return false;
    }
    strm.Printf("cjthread #%lu state: %s name: %s\n", id, state.c_str(), name.c_str());

    return true;
}

bool CommandObjCJThreadCommon::GetFramesStatus(Stream &strm, Status &error, uint32_t first_frame,
                                               uint32_t num_frames) {
  if (m_frames.size() == 0) {
    error.SetErrorStringWithFormat("unknow error");
    return false;
  }

  if (first_frame > m_frames.size()) {
    error.SetErrorStringWithFormat("invalid frame index \"%u\"", first_frame);
    return false;
  }

  if (num_frames <= 0) {
    error.SetErrorStringWithFormat("invalid frame count \"%u\"", num_frames);
    return false;
  }

  uint32_t last_frame = first_frame + num_frames;
  if (last_frame > m_frames.size()) {
    last_frame = m_frames.size();
  }

  strm.IndentMore();
  for (uint32_t index = first_frame; index < last_frame; index++) {
    auto frame_sp = m_frames[index];
    if (!frame_sp) {
      break;
    }
    frame_sp->GetStatus(strm, true, false, false);
  }
  strm.IndentLess();
  return true;
}

bool CommandObjCJThreadCommon::GetCurrentCJthreadStatus(ThreadSP thread, CommandReturnObject &result,
                                                        ValueObjectSP &info, uint32_t first_frame,
                                                        uint32_t num_frames) {
  uint32_t thread_tid = thread->GetProtocolID();
  auto cjthread_context_sp = GetCJthreadInfoByThreadID(info, thread_tid);
  return GetCJThreadStatus(thread, result, cjthread_context_sp, first_frame, num_frames);
}

bool CommandObjCJThreadCommon::GetCJThreadStatus(ThreadSP thread, CommandReturnObject &result,
                                                 ValueObjectSP cjthread_context,
                                                 uint32_t first_frame, uint32_t num_frames) {
  Status error;
  Stream &stream = result.GetOutputStream();
  if (!cjthread_context) {
    result.AppendErrorWithFormat("invalid cjthread context");
    return false;
  }
  GetDescription(stream, error, cjthread_context);
  if (error.Fail()) {
    result.AppendErrorWithFormat("%s", error.AsCString());
    return false;
  }
  HandOneCJThread(thread, cjthread_context, error);
  if (error.Fail()) {
    result.AppendErrorWithFormat("%s", error.AsCString());
    return false;
  }
  GetFramesStatus(stream, error, first_frame, num_frames);
  if (error.Fail()) {
    result.AppendErrorWithFormat("%s", error.AsCString());
    return false;
  }

  return true;
}

ValueObjectSP CommandObjCJThreadCommon::GetCJthreadInfoByCJThreadID(ValueObjectSP cjthread_info, uint32_t id) {
  if (!cjthread_info) {
    return nullptr;
  }

  for (uint32_t i = 0; i < cjthread_info->GetNumChildren(); i++) {
    ValueObjectSP child_sp = cjthread_info->GetChildAtIndex(i, true);
    if (!child_sp) {
      continue;
    }
    ValueObjectSP cjthread_id = child_sp->GetChildMemberWithName(ConstString("id"), true);
    if (!cjthread_id) {
      continue;
    }
    if (cjthread_id->GetValueAsUnsigned(UINT64_MAX) == static_cast<uint64_t>(id)) {
      return child_sp;
    }
  }

  return nullptr;
}

ValueObjectSP CommandObjCJThreadCommon::GetCJthreadInfoByThreadID(ValueObjectSP cjthread_info, uint32_t ID) {
  if (!cjthread_info) {
    return ValueObjectSP();
  }

  for (uint32_t i = 0; i < cjthread_info->GetNumChildren(); i++) {
    ValueObjectSP child_sp = cjthread_info->GetChildAtIndex(i, true);
    if (!child_sp) {
      continue;
    }
    ValueObjectSP tid_sp = child_sp->GetChildMemberWithName(ConstString("tid"), true);
    if (!tid_sp) {
      continue;
    }
    auto id = tid_sp->GetValueAsUnsigned(UINT64_MAX);
    if (id == static_cast<uint64_t>(ID)) {
      return child_sp;
    }
  }

  return ValueObjectSP();
}
