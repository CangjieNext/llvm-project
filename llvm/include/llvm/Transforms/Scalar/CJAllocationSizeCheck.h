//===- CJAllocationSizeCheck.h - --------------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file provides interface to "Cangjie Allocation Size Check" pass.
//
// This pass rejects oversized allocations whose size computation would
// overflow uint64_t: an alloca whose allocated type is not representable, or
// a global variable whose value type is not representable. It reports a
// complete diagnostic with the demangled Cangjie name, and is target
// independent so it also runs where the backend check cannot (e.g. arm32).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_CJ_ALLOCATION_SIZE_CHECK_H
#define LLVM_TRANSFORMS_SCALAR_CJ_ALLOCATION_SIZE_CHECK_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Module;

struct CJAllocationSizeCheck : public PassInfoMixin<CJAllocationSizeCheck> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) const;
};
} // namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_CJ_ALLOCATION_SIZE_CHECK_H
