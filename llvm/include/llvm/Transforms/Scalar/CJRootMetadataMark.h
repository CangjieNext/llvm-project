//===- CJRootMetadataMark.h - Root-aware metadata keep marking --*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Marks the metadata globals that should stay visible to the CJMetadata emitter
// when final static LTO is driven by a root-bc keep set.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_CJ_ROOT_METADATA_MARK_H
#define LLVM_TRANSFORMS_SCALAR_CJ_ROOT_METADATA_MARK_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Module;

struct CJRootReflectionPrunePass
    : public PassInfoMixin<CJRootReflectionPrunePass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) const;
};

struct CJRootMetadataMarkPass : public PassInfoMixin<CJRootMetadataMarkPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) const;
};
} // namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_CJ_ROOT_METADATA_MARK_H
