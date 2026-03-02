//===- CJGCInstrRestore.h - -----------------------------------------*- C++
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
// This file provides interface to "Cangjie GC Instruction Restore" pass.
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_CJ_GC_INSTR_RESTORE_H
#define LLVM_TRANSFORMS_CJ_GC_INSTR_RESTORE_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

class CJGCInstrRestore : public PassInfoMixin<CJGCInstrRestore> {
public:
  CJGCInstrRestore() = default;
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

private:
  bool runImpl(Function &F);
  void collectLSInstr(Function &F);
  void dispatchLSInstr(Instruction *Instr);
  bool restoreLSInstrToGCInstr();
  void initContainer();
  SmallVector<Instruction *, 4> LoadForGCReadRefs;
  SmallVector<Instruction *, 4> StoreForGCWriteRefs;
  SmallDenseMap<Intrinsic::ID, SmallVector<Instruction *, 4> *>
      LSInstrDispatchMap;
  SmallDenseMap<Value *, Value *, 4> ValueToBasePointer;
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_CJ_GC_INSTR_RESTORE_H