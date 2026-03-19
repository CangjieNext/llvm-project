//===-- CommandObjectCJThread.h ---------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_COMMANDS_COMMANDOBJECTCJTHREAD_H
#define LLDB_SOURCE_COMMANDS_COMMANDOBJECTCJTHREAD_H

#include "lldb/Interpreter/CommandObjectMultiword.h"

namespace lldb_private {

class CommandObjectMultiwordCJThread : public CommandObjectMultiword {
public:
  CommandObjectMultiwordCJThread(CommandInterpreter &interpreter);

  ~CommandObjectMultiwordCJThread() override;
};

class CommandObjectMultiwordCJThreadFrame : public CommandObjectMultiword {
public:
  explicit CommandObjectMultiwordCJThreadFrame(CommandInterpreter &interpreter);

  ~CommandObjectMultiwordCJThreadFrame() override;
};

} // namespace lldb_private
#endif // LLDB_SOURCE_COMMANDS_COMMANDOBJECTCJTHREAD_H