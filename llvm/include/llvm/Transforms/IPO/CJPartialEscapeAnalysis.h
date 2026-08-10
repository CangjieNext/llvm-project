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

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/PassManager.h"

class GCPtr;
class MemPtr;

namespace llvm {
enum EscapeState : unsigned { NotEscape, InfoEscaped, Escaped };
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

  // Assign a stable unique id to each GCPtr/MemPtr
  // provides a compact node identity for SpreadVisited dedup keys
  unsigned getNextId() { return NextId++; }

  // Record that P is fully escaped in BB
  void setEscaped(GCPtr *P, BasicBlock *BB) {
    if (!EscapeBBInfo[BB].count(P) || EscapeBBInfo[BB][P] < Escaped) {
      EscapeBBInfo[BB][P] = Escaped;
    }
  }

  // Deduplicate spreadMemEscape work
  // ensure the same propagation is done at most once
  void resetSpreadVisited() { SpreadVisited.clear(); }
  bool tryMarkSpreadVisited(GCPtr *P, unsigned ES, bool Direct,
                            SmallVectorImpl<int> &Offsets);

  Function *ProcessedFunc = nullptr;
  // Upper bound of object sizes seen among this analysis' GCNews. Offsets
  // beyond it cannot be legitimate field offsets, so they are collapsed to
  // the escape-all value by MemPtr::create / tryMarkSpreadVisited
  unsigned MaxObjSize = 0;
  // True once computeAllObjSizes() has finished. Before that MaxObjSize is
  // still being accumulated during initialize and must not be used as an
  // offset bound.
  bool MaxObjSizeComputed = false;
  DenseMap<Value *, GCPtr *> AllPtrLocInfo;
  DenseMap<std::pair<Value *, int>, MemPtr *> AllMemLocInfo;
  DenseMap<BasicBlock *, MapVector<GCPtr *, unsigned>> EscapeBBInfo;

private:
  unsigned NextId = 0;
  DenseSet<uint64_t> SpreadVisited;
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
