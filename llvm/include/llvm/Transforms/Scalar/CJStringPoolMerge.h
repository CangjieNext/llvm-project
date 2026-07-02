//===- CJStringPoolMerge.h - -----------------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This pass merges the per-literal cjstring byte buffers (emitted by
// CodeGen under full LTO) back into shared pools with substring sharing,
// running after LTO GlobalDCE so only live literals are repacked. See
// requirement/ir_analysis/scenario2_stringpool_trim_design.md section 12.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_CJSTRINGPOOLMERGE_H
#define LLVM_TRANSFORMS_SCALAR_CJSTRINGPOOLMERGE_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Module;

struct CJStringPoolMerge : public PassInfoMixin<CJStringPoolMerge> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) const;
};
} // namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_CJSTRINGPOOLMERGE_H
