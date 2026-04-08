//===- CJAnnotationOutline.h - Annotation outline pass ----------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This pass outlines profitable Cangjie annotation init / factory functions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_CANGJIE_ANNOTATION_OUTLINE_H
#define LLVM_TRANSFORMS_SCALAR_CANGJIE_ANNOTATION_OUTLINE_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Module;

struct CJAnnotationOutline : public PassInfoMixin<CJAnnotationOutline> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) const;
};
} // namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_CANGJIE_ANNOTATION_OUTLINE_H
