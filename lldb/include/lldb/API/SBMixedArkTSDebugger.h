//===-- SBMixedArkTSDebugger.h -------------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_API_SBMIXEDARKTSDEBUGGER_H
#define LLDB_API_SBMIXEDARKTSDEBUGGER_H

#include "lldb/API/SBDefines.h"

namespace lldb {

class LLDB_API SBMixedArkTSDebugger {
public:
  SBMixedArkTSDebugger();

  SBMixedArkTSDebugger(const lldb::SBTarget &rhs);

  SBMixedArkTSDebugger(const lldb::TargetSP &target_sp);

  ~SBMixedArkTSDebugger();

  /// Get the backtrace of ArkTS for current thread. A cstring which contains
  /// the information of backtrace is saved in return.
  ///
  /// If the current thread does not have an ArkTS runtime, "\0" will be returned.
  ///
  /// \param [out] er
  ///     The variable to get error reason, when some error occurred.
  ///
  /// \return
  ///     An lldb::SBData object which contain the raw cstring of ArkTS backtrace.
  lldb::SBData GetBackTrace(SBError &er);

  lldb::SBData OperateDebugMessage(const char *message, SBError &er);

private:
  lldb_private::MixedArkTSDebugger* m_opaque_ptr;
};

} // namespace lldb

#endif // LLDB_API_SBMIXEDARKTSDEBUGGER_H