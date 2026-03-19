//===-- CJSyntheticThread.h -----------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_TARGET_CJSYNTHETICTHREAD_H
#define LLDB_TARGET_CJSYNTHETICTHREAD_H

#include <string>

#include "lldb/Target/Thread.h"
#include "lldb/Utility/StructuredData.h"

#include "lldb/Plugins/Process/gdb-remote/ThreadGDBRemote.h"

class StringExtractor;

namespace lldb_private {
class Process;

class CJSyntheticThread : public lldb_private::process_gdb_remote::ThreadGDBRemote {
public:
  CJSyntheticThread(Process &process, lldb::tid_t tid)
  : ThreadGDBRemote(process, tid)
  {  }

  bool IsSynthetic() const override { return true; }
};

} // namespace lldb_private

#endif // LLDB_TARGET_CJSYNTHETICTHREAD_H