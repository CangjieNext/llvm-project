//===-- MixedDebugger.h --------------------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_MIXEDDEBUGGER_MIXEDDEBUGGER_H
#define LLDB_SOURCE_PLUGINS_MIXEDDEBUGGER_MIXEDDEBUGGER_H

#include "lldb/Utility/DataExtractor.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/Status.h"
#include "lldb/lldb-forward.h"

using namespace lldb;

namespace lldb_private {

class MixedDebugger{
public:
  MixedDebugger(const TargetSP &target_sp);

  virtual ~MixedDebugger() {};

  /// Execute the C API provided by runtime to get debug information.
  ///
  /// \param [in] expr
  ///     A cstring of expression which start with "(const char*)".
  ///
  /// \param [out] error
  ///     The variable to record error reason.
  ///
  /// \return
  ///     A DataExtractorSP contain a cstring which length is less than
  ///     UINT32_MAX.
  DataExtractorSP ExecuteAction(const char* expr, Status &error);

  virtual DataExtractorSP GetCurrentThreadBackTrace(Status &error) = 0;

  virtual DataExtractorSP GetCurrentThreadOperateDebugMessageResult(const char *message, Status &error) = 0;

protected:
  TargetSP m_target_sp;

  MixedDebugger() = delete;
};

} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_MIXEDDEBUGGER_MIXEDDEBUGGER_H