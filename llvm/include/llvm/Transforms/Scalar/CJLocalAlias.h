//===- CJLocalAlias.h - local aliases for preemptible TypeInfo -*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This pass redirects constant-initializer references to locally-defined
// preemptible TypeInfo globals onto hidden aliases, so that lld can emit
// R_*_RELATIVE (RELR-packable) relocations instead of ABS64.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_CJ_TI_LOCAL_ALIAS_H
#define LLVM_TRANSFORMS_SCALAR_CJ_TI_LOCAL_ALIAS_H

#include "llvm/IR/PassManager.h"

namespace llvm {
struct CJLocalAlias : public PassInfoMixin<CJLocalAlias> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) const;
};
} // namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_CJ_TI_LOCAL_ALIAS_H
