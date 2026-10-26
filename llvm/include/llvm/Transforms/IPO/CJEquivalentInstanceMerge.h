//===- CJEquivalentInstanceMerge.h - Merge equivalent CJ instances --------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Canonicalize layout-only type-info uses of equivalent Cangjie primitive
// instantiations, then let MergeFunctions share their implementations without
// changing the language-level type identity seen by reflection or type tests.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_IPO_CJEQUIVALENTINSTANCEMERGE_H
#define LLVM_TRANSFORMS_IPO_CJEQUIVALENTINSTANCEMERGE_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Module;

struct CJEquivalentInstanceMerge
    : public PassInfoMixin<CJEquivalentInstanceMerge> {
  explicit CJEquivalentInstanceMerge(bool RunMergeFunctions = true)
      : RunMergeFunctions(RunMergeFunctions) {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) const;

private:
  bool RunMergeFunctions;
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_IPO_CJEQUIVALENTINSTANCEMERGE_H
