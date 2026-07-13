//===-- GlobalDCE.h - DCE unreachable internal functions ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This transform is designed to eliminate unreachable internal globals from the
// program.  It uses an aggressive algorithm, searching out globals that are
// known to be alive.  After it finds all of the globals which are needed, it
// deletes whatever is left over.  This allows it to delete recursive chunks of
// the program which are unreachable.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_IPO_GLOBALDCE_H
#define LLVM_TRANSFORMS_IPO_GLOBALDCE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/Scalar/CJFillMetadata.h"
#include <unordered_map>

namespace llvm {
class Comdat;
class Constant;
class Function;
class GlobalVariable;
class Metadata;
class Module;
class Value;

/// Pass to remove unused function declarations.
class GlobalDCEPass : public PassInfoMixin<GlobalDCEPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &);

private:
  SmallPtrSet<GlobalValue*, 32> AliveGlobals;

  /// Global -> Global that uses this global.
  DenseMap<GlobalValue *, SmallPtrSet<GlobalValue *, 4>> GVDependencies;

  /// Constant -> Globals that use this global cache.
  std::unordered_map<Constant *, SmallPtrSet<GlobalValue *, 8>>
      ConstantDependenciesCache;

  /// Comdat -> Globals in that Comdat section.
  std::unordered_multimap<Comdat *, GlobalValue *> ComdatMembers;

  /// !type metadata -> set of (vtable, offset) pairs
  DenseMap<Metadata *, SmallSet<std::pair<GlobalVariable *, uint64_t>, 4>>
      TypeIdMap;

  // Global variables which are vtables, and which we have enough information
  // about to safely do dead virtual function elimination.
  SmallPtrSet<GlobalValue *, 32> VFESafeVTables;

  void UpdateGVDependencies(GlobalValue &GV);
  void MarkLive(GlobalValue &GV,
                SmallVectorImpl<GlobalValue *> *Updates = nullptr);
  bool RemoveUnusedGlobalValue(GlobalValue &GV);

  // Dead virtual function elimination.
  void AddVirtualFunctionDependencies(Module &M);
  void ScanVTables(Module &M);
  void ScanTypeCheckedLoadIntrinsics(Module &M);
  void ScanVTableLoad(Function *Caller, Metadata *TypeId, uint64_t CallOffset);

  void ComputeDependencies(Value *V, SmallPtrSetImpl<GlobalValue *> &U);

  friend struct CangjieDCE;
};

struct CangjieDCE {
  // VFE dependency model with lifted generic arity (pattern-table form).
  //
  // ExtensionDef registrations and call sites are both decoded into a flat
  // sequence of type-boundary globals: Seq[0] is the interface root global and
  // Seq[1..] are the instantiated type arguments in DFS order. A null entry
  // stands for the "$g" wildcard of an uninstantiated generic type parameter
  // (or for a label that names no global in this module, conservatively).
  //
  // A def pattern P protects a call-site sequence C iff, positionally,
  //   P[i] == C[i] || P[i] == null || C[i] == null   for i < min(|P|, |C|).
  // For all-concrete sequences this degenerates to prefix comparability,
  // which is exactly the ancestor/subtree closure of the legacy trie, so
  // legacy metadata - including the shortened pre-arity-lift sequences whose
  // generic arguments were dropped - behaves unchanged.
  struct VFEPattern {
    SmallVector<GlobalVariable *, 4> Seq; // Seq[0] is the root; null = "$g".
    GlobalVariable *FT = nullptr;         // Function table of the ExtensionDef.
    bool HasCompilerInfo = false;         // ET_FLAG != 0: FT carries pair slots.
  };

  // These are used for eliminating the useless virtual func.
  GlobalDCEPass &DCE;
  // Interface root global -> patterns registered by that interface's defs.
  DenseMap<GlobalVariable *, SmallVector<VFEPattern, 2>> Patterns;

  // This is used for eliminating the useless typeinfo.
  DenseMap<Value *, SmallPtrSet<GlobalVariable *, 8>> ReverseDeps;

  CangjieDCE() = delete;
  CangjieDCE(GlobalDCEPass &DCE) : DCE(DCE) {}

  static bool isCangjieType(GlobalVariable *GV) {
    return GV->isCJTypeInfo() || GV->isCJTypeTemplate();
  }
  static bool isMetaAssociatedWithType(GlobalVariable *GV) {
    return GV->isCJTypeExt() || GV->isCJMTable();
  }

  static bool maybeFakeLiveOfTypeMeta(GlobalVariable *GVU, GlobalVariable *GV);

  int getReverseDepsIndex(GlobalVariable *GV) {
    if (GV->isCJMTable())
      return ExtensionDefFieldType::ET_TARGET_TYPE;
    if (GV->isCJTypeExt())
      return 0; // 0: dependence typeinfo
    return -1;
  }

  // Decode VFE metadata into a pattern sequence (DFS-flattened). Returns true
  // on success; returns false when the root label names no global in this
  // module, in which case the call site must be treated as fully unknown and
  // every registered pattern marked (legacy empty-path behavior).
  bool resolveVFEMetaSeq(Metadata *MD, Module &M,
                         SmallVectorImpl<GlobalVariable *> &Seq);

  // Positional wildcard match between a def pattern P and a call sequence C.
  static bool matchVFEPattern(ArrayRef<GlobalVariable *> P,
                              ArrayRef<GlobalVariable *> C);

  // Mark the function-table entry at Offset (and its compiler-info pair) of
  // the pattern's FT as a dependency of Caller.
  void markFuncTableEntry(Function *Caller, const VFEPattern &P,
                          uint64_t Offset);

  void addCangjieVirtualFunctionDependencies(Module &);

  // Obtain the function table FT that achieves P from C, return {C, P, FT}.
  std::tuple<GlobalVariable *, GlobalVariable *, GlobalVariable *>
  scanVTable(GlobalVariable &);

  void initTIE(Module &);
  void updateLiveExtensions();
  void updateNonExternalExtensionDefs(Module &M);
  void shrinkNonExternalExtensionDefs(Module &M);
  void rewriteExtensions(Module &);
  void rewriteLLVMUsed(Module &);
};

}

#endif // LLVM_TRANSFORMS_IPO_GLOBALDCE_H
