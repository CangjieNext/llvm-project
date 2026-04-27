//===-- MixedArkTSDebugger.h ---------------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_MIXEDDEBUGGER_ARKTS_MIXEDARKTSDEBUGGER_H
#define LLDB_SOURCE_PLUGINS_MIXEDDEBUGGER_ARKTS_MIXEDARKTSDEBUGGER_H

#include "lldb/Target/MixedDebugger.h"
using namespace lldb;

namespace lldb_private {

class MixedArkTSDebugger : public MixedDebugger {
public:
  MixedArkTSDebugger(const lldb::TargetSP &target_sp);

  ~MixedArkTSDebugger() {};

  DataExtractorSP GetCurrentThreadBackTrace(Status &error) override;

  DataExtractorSP GetCurrentThreadOperateDebugMessageResult(const char *message, Status &error) override;
};

} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_MIXEDDEBUGGER_MIXEDDEBUGGER_H