//===-- CJSyntheticThread.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
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