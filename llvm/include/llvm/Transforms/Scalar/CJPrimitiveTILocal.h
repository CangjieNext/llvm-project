//===- CJPrimitiveTILocal.h - -----------------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file provides the interface to the "Cangjie Primitive TypeInfo
// Localization" pass, which materializes hidden local bodies for imported
// primitive type infos (Unit.ti, Int64.ti, ...) to eliminate ABS64 dynamic
// relocations.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_CANGJIE_PRIMITIVE_TI_LOCAL_H
#define LLVM_TRANSFORMS_SCALAR_CANGJIE_PRIMITIVE_TI_LOCAL_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Module;

struct CJPrimitiveTILocal : public PassInfoMixin<CJPrimitiveTILocal> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) const;
};
} // namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_CANGJIE_PRIMITIVE_TI_LOCAL_H
