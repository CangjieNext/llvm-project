//===- CJExceptionTailOutline.h - outline cold throw tails ------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file provides the interface to the "CJ Exception Tail Outline" pass.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_CJ_EXCEPTION_TAIL_OUTLINE_H
#define LLVM_TRANSFORMS_SCALAR_CJ_EXCEPTION_TAIL_OUTLINE_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Module;

/// Shares the cold tail of a throw sequence between call sites.
///
/// A tail here is a block that ends with CJ_MCC_ThrowException followed by
/// unreachable. Not every such tail is outlined: the thrown object must also
/// come from the immediately preceding NewObject/ctor pair or factory call.
///
/// This runs on its own, after inlining, rather than as part of
/// CJRuntimeLowering. Shrinking a throw tail also shrinks the enclosing
/// function body, and CJRuntimeLowering runs before the inliner, so doing the
/// extraction there feeds the inliner a smaller cost estimate and makes it
/// inline far more aggressively. On macro-heavy modules that cost more than the
/// extraction saved. Running after inlining keeps the saving without disturbing
/// the cost model, and sees the throw tails duplicated by inlining as well.
struct CJExceptionTailOutline : public PassInfoMixin<CJExceptionTailOutline> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) const;
};
} // namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_CJ_EXCEPTION_TAIL_OUTLINE_H
