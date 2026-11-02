//===- CJInsertRemoveLocalFinalizer.h - -------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file provides interface to the "Cangjie Insert Remove Local Finalizer"
// pass.
//
// CJRuntimeLowering registers every LocalMode finalizer object with
// CJ_MCC_AddLocalFinalizer. This pass pairs each such registration with a
// CJ_MCC_RemoveLocalFinalizer call at the end of the object's live range, on
// every control-flow path, exactly once.
//
// It must run before CJRewriteStatepoint: the Remove call is a potential
// safepoint, so the statepoint rewriter has to see it in order to relocate the
// object pointer it takes.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_CJ_INSERT_REMOVE_LOCAL_FINALIZER_H
#define LLVM_TRANSFORMS_SCALAR_CJ_INSERT_REMOVE_LOCAL_FINALIZER_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Function;

class CJInsertRemoveLocalFinalizer
    : public PassInfoMixin<CJInsertRemoveLocalFinalizer> {
public:
  CJInsertRemoveLocalFinalizer() = default;
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_CJ_INSERT_REMOVE_LOCAL_FINALIZER_H
