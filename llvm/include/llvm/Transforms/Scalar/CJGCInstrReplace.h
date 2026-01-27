//===- CJGCInstrReplace.h - -----------------------------------------*- C++
//-*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file provides interface to "Cangjie GC Instruction Replace" pass.
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_CJ_GC_INSTR_REPLACE_H
#define LLVM_TRANSFORMS_CJ_GC_INSTR_REPLACE_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

class CJGCInstrReplace : public PassInfoMixin<CJGCInstrReplace> {
public:
  CJGCInstrReplace() = default;
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static constexpr StringRef GC_READ_REF_IID = "GC_Read_Ref_IID";
  static constexpr StringRef GC_WRITE_REF_IID = "GC_Write_Ref_IID";
  static SmallDenseMap<Intrinsic::ID, StringRef> GCIntrinsicIDToLabel;

private:
  bool runImpl(Function &F);
  void collectGCInstr(Function &F);
  void dispatchGCInstr(CallBase *CB);
  bool changeGCInstrToLSInstr();
  void initContainer();
  SmallVector<CallBase *, 4> GCReadRefs;
  SmallVector<CallBase *, 4> GCWriteRefs;
  SmallDenseMap<Intrinsic::ID, SmallVector<CallBase *, 4> *> GCInstrDispatchMap;
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_CJ_GC_INSTR_REPLACE_H