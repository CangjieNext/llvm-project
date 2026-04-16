//===- ParamMergeFunctions.h - Merge functions via parameters ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass is a conservative local-only companion to mergefunc. It merges a
// narrow class of single-basic-block functions that differ only in pointer
// constants referencing globals by creating one internal implementation with
// extra parameters and rewriting the original functions into thunks.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_IPO_PARAMMERGEFUNCTIONS_H
#define LLVM_TRANSFORMS_IPO_PARAMMERGEFUNCTIONS_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Module;

class ParamMergeFunctionsPass : public PassInfoMixin<ParamMergeFunctionsPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_IPO_PARAMMERGEFUNCTIONS_H
