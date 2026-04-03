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
  struct GenericFuncInfo {
    bool IsValid = false;
    GenericFuncInfo *Prev = nullptr;
    // Save all func tables with current type.
    SmallPtrSet<GlobalVariable *, 4> FuncTables;
    DenseMap<GlobalVariable *, bool> HasCompilerInfo;
    DenseMap<GlobalVariable *, GenericFuncInfo *> Next;

#ifndef NDEBUG
    GlobalVariable *Ty = nullptr;
#endif
  };

  // These are used for eliminating the useless virtual func.
  GlobalDCEPass &DCE;
  GenericFuncInfo *Root;

  // This is used for eliminating the useless typeinfo.
  DenseMap<Value *, SmallPtrSet<GlobalVariable *, 8>> ReverseDeps;

  CangjieDCE() = delete;
  CangjieDCE(GlobalDCEPass &DCE) : DCE(DCE) { Root = new GenericFuncInfo; }
  ~CangjieDCE();

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

  GenericFuncInfo *insertInstance(const SmallVectorImpl<GlobalVariable *> &,
                                  GlobalVariable *FT);

  GenericFuncInfo *
  findTargetInstance(const SmallVectorImpl<GlobalVariable *> &);

  // Parse metadata and return the corresponding GFI.
  GenericFuncInfo *resolveVFEMeta(Metadata *, Module &);

  void addCangjieVirtualFunctionDependencies(Module &);

  // Obtain the function table FT that achieves P from C, return {C, P, FT}.
  std::tuple<GlobalVariable *, GlobalVariable *, GlobalVariable *>
  scanVTable(GlobalVariable &);

  // Add dependencies between caller and callee.
  void updateDependencies(Function *,
                          DenseMap<GenericFuncInfo *, SmallSet<uint64_t, 4>> &);

  void initTIE(Module &);
  void updateLiveExtensions();
  void updateNonExternalExtensionDefs(Module &M);
  void rewriteExtensions(Module &);
  void rewriteLLVMUsed(Module &);
};

}

#endif // LLVM_TRANSFORMS_IPO_GLOBALDCE_H
