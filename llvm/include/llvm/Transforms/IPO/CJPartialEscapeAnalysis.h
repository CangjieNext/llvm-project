//===- CJPartialEscapeAnalysis.h - escape analysis for Cangjie ------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file provides interface to "PEA" pass.
//
// This file implements escape analysis, and trans non-escape object from heap
// to stack if possible.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_IPO_CJPartialEscapeAnalysis_H
#define LLVM_TRANSFORMS_IPO_CJPartialEscapeAnalysis_H

#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/PassManager.h"

class GCPtr;
class MemPtr;

namespace llvm {
enum EscapeState : unsigned { NotEscape, InfoEscaped, TransEscaped, Escaped };
struct CJPartialEscapeAnalysisPass
    : public PassInfoMixin<CJPartialEscapeAnalysisPass> {
  PreservedAnalyses run(LazyCallGraph::SCC &C, CGSCCAnalysisManager &AM,
                        LazyCallGraph &CG, CGSCCUpdateResult &URx) const;
};

class CJEscapeAnalysis {
public:
  Value *getBaseValue(Value *CV, int &Offset);
  void setInfoEscaped(GCPtr *, BasicBlock *);
  bool isEscapedValue(Value *V);

  Function *ProcessedFunc = nullptr;
  DenseMap<Value *, GCPtr *> AllPtrLocInfo;
  DenseMap<std::pair<Value *, int>, MemPtr *> AllMemLocInfo;
  DenseMap<BasicBlock *, DenseMap<GCPtr *, unsigned>> EscapeBBInfo;
private:
  void clearNullptrInEscapeBBInfo();
};

Type *getAllocaType(GlobalVariable *Klass, bool &HasRef, uint32_t &AS, bool &,
                    Instruction *Val);
void setEscapeMeta(Instruction *I);

GlobalVariable *getNewKlass(CallBase *I);

bool isRewriteableCangjieMallocFunc(Function *F);

bool escapeAnalysisFuncImpl(Function *F,
                            function_ref<LoopInfo &(Function &)> LoopLoop);
} // namespace llvm

#endif
