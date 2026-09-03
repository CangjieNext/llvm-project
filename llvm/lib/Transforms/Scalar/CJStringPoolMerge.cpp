//===- CJStringPoolMerge.cpp - merge cangjie string literal pools ---------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Placeholder for the upcoming cjstring pool merge pass. CodeGen now emits
// one array buffer per string literal (deferred pooling, offset always 0);
// this pass will merge/deduplicate those buffers at LTO time. Currently a
// noop.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJStringPoolMerge.h"

#include "llvm/IR/Module.h"

using namespace llvm;

PreservedAnalyses CJStringPoolMerge::run(Module &,
                                         ModuleAnalysisManager &) const {
  return PreservedAnalyses::all();
}
