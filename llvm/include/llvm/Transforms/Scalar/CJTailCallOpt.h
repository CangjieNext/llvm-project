//===- CJTailCallOpt.h - Cangjie tail call optimization ---------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This pass marks safe tail-position Cangjie calls as tail calls before
// CJRewriteStatepoint lowers them to statepoints.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_CJ_TAIL_CALL_OPT_H
#define LLVM_TRANSFORMS_SCALAR_CJ_TAIL_CALL_OPT_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Module;

struct CJTailCallOpt : public PassInfoMixin<CJTailCallOpt> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) const;
};
} // namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_CJ_TAIL_CALL_OPT_H
