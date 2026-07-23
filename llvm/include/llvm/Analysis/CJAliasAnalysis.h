//===- CJAliasAnalysis.h - --------------------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares a simple AliasAnalysis using special knowledge
// of Cangjie to enhance other optimization passes which rely on the Alias
// Analysis infrastructure.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_CJALIASANALYSIS_H
#define LLVM_ANALYSIS_CJALIASANALYSIS_H

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include <memory>

namespace llvm {

class CJAAResult : public AAResultBase<CJAAResult> {
  friend AAResultBase<CJAAResult>;
  const DataLayout &DL;
  const TargetLibraryInfo &TLI;
  const DominatorTree *DT;

public:
  explicit CJAAResult(const DataLayout &DL, const TargetLibraryInfo &TLI,
                      const DominatorTree *DT)
      : DL(DL), TLI(TLI), DT(DT) {}
  CJAAResult(CJAAResult &&Arg)
      : AAResultBase(std::move(Arg)), DL(Arg.DL), TLI(Arg.TLI), DT(Arg.DT) {}

  AliasResult alias(const MemoryLocation &LocA, const MemoryLocation &LocB,
                    AAQueryInfo &AAQI);

  using AAResultBase::getModRefInfo;
  ModRefInfo getModRefInfo(const CallBase *Call, const MemoryLocation &Loc,
                           AAQueryInfo &AAQI);
  bool invalidate(Function &, const PreservedAnalyses &,
                  FunctionAnalysisManager::Invalidator &);
};

class CangjieAA : public AnalysisInfoMixin<CangjieAA> {
  friend AnalysisInfoMixin<CangjieAA>;
  static AnalysisKey Key;

public:
  using Result = CJAAResult;
  CJAAResult run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm

#endif