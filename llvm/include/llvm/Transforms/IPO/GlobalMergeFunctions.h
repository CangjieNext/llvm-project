//===- GlobalMergeFunctions.h - Merge const-different functions-*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass merges structurally equivalent functions that differ only in a
// selected subset of constant operands.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_IPO_GLOBALMERGEFUNCTIONS_H
#define LLVM_TRANSFORMS_IPO_GLOBALMERGEFUNCTIONS_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Module;

/// Merge functions with selected constant-operand differences.
class GlobalMergeFuncPass : public PassInfoMixin<GlobalMergeFuncPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_IPO_GLOBALMERGEFUNCTIONS_H
