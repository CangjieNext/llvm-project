//===- CJAnnotationInitDedup.h - -------------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This pass deduplicates profitable Cangjie annotation init functions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_CANGJIE_ANNOTATION_INIT_DEDUP_H
#define LLVM_TRANSFORMS_SCALAR_CANGJIE_ANNOTATION_INIT_DEDUP_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Module;

struct CJAnnotationInitDedup : public PassInfoMixin<CJAnnotationInitDedup> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) const;
};
} // namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_CANGJIE_ANNOTATION_INIT_DEDUP_H
