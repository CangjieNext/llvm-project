//===- CJGCLiveAnalysis.h - -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Analyze and calculate live cangjie references based on cfg.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_GCLIVEANALYSIS_H
#define LLVM_TRANSFORMS_SCALAR_GCLIVEANALYSIS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Statepoint.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/ValueHandle.h"

using namespace llvm;

class FieldInfo {
public:
  Value *V;
  int Offset;

  FieldInfo(Value *V, int Offset) : V(V), Offset(Offset) {}
  ~FieldInfo() = default;
};

using StructLiveSetTy = SetVector<FieldInfo *>;

struct PtrLiveData {
  /// Values defined in this block.
  MapVector<BasicBlock *, SetVector<Value *>> KillSet;

  /// Values used in this block (and thus live); does not included values
  /// killed within this block.
  MapVector<BasicBlock *, SetVector<Value *>> LiveSet;

  /// Values live into this basic block (i.e. used by any
  /// instruction in this basic block or ones reachable from here)
  MapVector<BasicBlock *, SetVector<Value *>> LiveIn;

  /// Values live out of this basic block (i.e. live into
  /// any successor block)
  MapVector<BasicBlock *, SetVector<Value *>> LiveOut;
};

struct StructLiveData {
  /// Values defined in this block.
  MapVector<BasicBlock *, SetVector<FieldInfo *>> KillSet;

  /// Values used in this block (and thus live); does not included values
  /// killed within this block.
  MapVector<BasicBlock *, SetVector<FieldInfo *>> LiveSet;

  /// Values live into this basic block (i.e. used by any
  /// instruction in this basic block or ones reachable from here)
  MapVector<BasicBlock *, SetVector<FieldInfo *>> LiveIn;

  /// Values live out of this basic block (i.e. live into
  /// any successor block)
  MapVector<BasicBlock *, SetVector<FieldInfo *>> LiveOut;
};

using PointerToBaseTy = MapVector<Value *, Value *>;
using LiveSetTy = SetVector<Value *>;
using RematerializedValueMapTy =
    MapVector<AssertingVH<Instruction>, AssertingVH<Value>>;

struct SafepointRecord {
  /// The set of values known to be gc-live across this safepoint
  LiveSetTy LiveSet;

  /// Records the known gc fields in struct values inserted into the statepoint.
  LiveSetTy FieldSet;

  bool isCJStackCheck = false;

  /// The *new* gc.statepoint instruction itself.  This produces the token
  /// that normal path gc.relocates and the gc.result are tied to.
  GCStatepointInst *StatepointToken;

  /// Instruction to which exceptional gc relocates are attached
  /// Makes it easier to iterate through them during relocationViaAlloca.
  Instruction *UnwindToken;

  /// Record live values we are rematerialized instead of relocating.
  /// They are not included into 'LiveSet' field.
  /// Maps rematerialized copy to it's original value.
  RematerializedValueMapTy RematerializedValues;
};

using PhiToAllocasTy = MapVector<Value *, SetVector<Value *>>;
using StructLayoutGCPtrTy = MapVector<Value *, MapVector<uint64_t, bool>>;
struct AllocaAnalysisData {
  // Record the layout of each alloca is a gcptr.
  StructLayoutGCPtrTy StructLayoutGCPtrMap;

  // Records the map from the PhiNode to the Alloca instruction.
  PhiToAllocasTy PhiToAllocasMap;

  // Record the structs that contain gcptr.
  SetVector<Value *> Structs;

  // Record large structs separately, when the number of refs contained in
  // a struct exceeds the threshold.
  // This is an optimization, see LargeStructSizeThreshold.
  SetVector<Value *> LargeStructs;

  // Record the number of gcptrs contained in the struct that is in above set.
  MapVector<Value *, unsigned> StructGCFieldNum;

  void clear() {
    StructLayoutGCPtrMap.clear();
    PhiToAllocasMap.clear();
    Structs.clear();
    StructGCFieldNum.clear();
  }
};

enum MemoryAccessKind {
  MemoryNone = 0,
  DefineMemory = 0b01,
  UseMemory = 0b10,
  DefineAndUse = 0b11,
};

void checkFailed(const Twine &Message, Instruction *I);

namespace llvm {
// Check whether the base of the gcptr is from the alloca struct or argument
// instruction.
bool findAllocaInsts(Value *V, SetVector<Value *> &BaseSet,
                     bool HasArg = false);
} // end namespace llvm

/// Abstract method class for livenses variable analysis
class GCLiveAnalysis {
public:
  Function &F;
  __attribute__((unused)) DominatorTree &DT;

  GCLiveAnalysis(Function &F, DominatorTree &DT) : F(F), DT(DT) {
    computeFakeGCPtrs();
  }
  virtual ~GCLiveAnalysis() = default;

  bool isFakeGCPtr(Value *V) { return FakeGCPtrs.contains(V); }

  bool isFakeContainGCPtrs(Value *V) { return ContainGCPtrsFakes.contains(V); }

  // Computes variables that are converted to type `i8 addrspace(1)*` but
  // are actually pointers on the stack, and save it.
  // If it contains gcptr, save it to ContainGCPtrsFakes.
  void computeFakeGCPtrs();

  void checkBasicSSA(SetVector<Value *> &Live, Instruction *TI,
                     bool TermOkay = false);

  void checkBasicSSA(PtrLiveData &Data);

  // Conservatively identifies any definitions which might be live at the
  // given instruction. The  analysis is performed immediately before the
  // given instruction. Values defined by that instruction are not considered
  // live.  Values used by that instruction are considered live.
  void analyzeParsePointLiveness(CallBase *Call, SetVector<Value *> &LiveSet);

  /// Compute the live-in set for the location rbegin starting from
  /// the live-out set of the basic block
  virtual void computeLiveInValues(BasicBlock::reverse_iterator Begin,
                                   BasicBlock::reverse_iterator End,
                                   SetVector<Value *> &LiveTmp);

  virtual void computeLiveOutSeed(BasicBlock *BB, SetVector<Value *> &LiveTmp);

  virtual void computeKillSet(BasicBlock *BB, SetVector<Value *> &LiveSet);

  /// Compute the live-in set for every basic block in the function
  void computeLiveness();

  /// Given results from the dataflow liveness computation, find the set of live
  /// Values at a particular instruction.
  void findLiveSetAtInst(Instruction *Inst, SetVector<Value *> &Out);

  bool insertLiveSetForCJIntrinsic(Instruction *I, SetVector<Value *> &LiveTmp);

public:
  PtrLiveData Data;

private:
  // Those are on-stack pointers, converted from addrspacecast to gc pointers.
  SetVector<Value *> FakeGCPtrs;
  // Those are FakeGCPtrs which contains gcptr, is a subset of the above.
  SetVector<Value *> ContainGCPtrsFakes;
  // Avoid duplicate liveness analysis in the BB.
  MapVector<BasicBlock *, std::pair<Instruction *, SetVector<Value *>>>
      InstLiveSetCache;
};

/// Abstract method class for livenses variable analysis
class StructLiveAnalysis : public GCLiveAnalysis {
public:
  StructLiveAnalysis(Function &F, DominatorTree &DT,
                     AllocaAnalysisData &AllocaData)
      : GCLiveAnalysis(F, DT), DL(F.getParent()->getDataLayout()),
        AllocaData(AllocaData) {}
  ~StructLiveAnalysis() { clear(); }

  FieldInfo *getFieldInfo(Value *Ptr, int Offset);

  FieldInfo *getFieldInfoByValue(Value *Ptr);

  bool containGCPtrByOffset(Value *Ptr, uint64_t Size);

  bool isBaseContainGCPtr(Value *Ptr);

  void checkBasicSSA(SetVector<FieldInfo *> &Live, Instruction *TI,
                     bool TermOkay = false);

  void checkBasicSSA(StructLiveData &Data);

  void initializeAlloca(AllocaInst *AI);

  void insertInitializerAfterLifetime(Value *V, CallInst *CI);

  void moveInitializerAfterLifetime(Value *V, Instruction *I, CallInst *CI);

  void insertMemsetForPhiStruct();

  void computeFieldLiveInValues(BasicBlock::reverse_iterator Begin,
                                BasicBlock::reverse_iterator End,
                                SetVector<FieldInfo *> &LiveTmp);

  void computeFieldLiveOutSeed(BasicBlock *BB,
                               SetVector<FieldInfo *> &LiveOuts);

  void computeFieldKillSet(BasicBlock *BB, SetVector<FieldInfo *> &KillSet);

  void computeLiveness();

  void findLiveSetAtInst(Instruction *Inst, StructLiveData &Data,
                         StructLiveSetTy &Out);

  void legalizeMemoryValueSet(SetVector<FieldInfo *> &LiveSet);

  bool isStackContainGCPtr(Value *V, uint64_t Size = 0);

  void addGCFields(Value *V, SetVector<FieldInfo *> &Set, uint64_t Begin = 0,
                   uint64_t End = 0);

  void addGCFieldsByBase(Value *V, SetVector<FieldInfo *> &Set);

  void addGCFieldsByValue(Value *V, SetVector<FieldInfo *> &Set);

  void addGCFieldsByMemory(Value *V, SetVector<FieldInfo *> &AllocaSet,
                           uint64_t Size = 0);

  void visitMemoryLoad(LoadInst *LI, SetVector<FieldInfo *> &AllocaUses);

  void visitMemoryStore(StoreInst *SI, SetVector<FieldInfo *> &AllocaDefs);

  void visitMemoryIntrinsic(IntrinsicInst *II,
                            SetVector<FieldInfo *> &AllocaDefs,
                            SetVector<FieldInfo *> &AllocaUses);

  void visitMemoryCallBase(CallBase *CB, SetVector<FieldInfo *> &AllocaDefs,
                           SetVector<FieldInfo *> &AllocaUses);

  void visitMemorySelect(SelectInst *SI, SetVector<FieldInfo *> &AllocaDefs,
                         SetVector<FieldInfo *> &AllocaUses);

  MemoryAccessKind hasMemoryDefineOrUseValue(Instruction *I,
                                         SetVector<FieldInfo *> &AllocaDefs,
                                         SetVector<FieldInfo *> &AllocaUses);

  // Conservatively identifies any definitions which might be live at the
  // given instruction. The  analysis is performed immediately before the
  // given instruction. Values defined by that instruction are not considered
  // live.  Values used by that instruction are considered live.
  void analyzeParsePointLiveness(CallBase *Call,
                                 SetVector<FieldInfo *> &LiveSet);

  void combineStructGCField(StructLiveSetTy &FieldInfos,
                            SetVector<Value *> &Structs, CallBase *Call);

  void insertGEPForField(CallBase *Call, StructLiveSetTy &FieldInfos,
                         LiveSetTy &FieldSet);

  void clear() {
    StackContainGCPtrMap.clear();
    for (auto *Field : AllFields) {
      delete Field;
    }
    AllFields.clear();
  }

public:
  StructLiveData Data;
  SmallSet<Value *, 8> InsertedMemset;

private:
  const DataLayout &DL;
  AllocaAnalysisData &AllocaData;
  // Cache All fieldinfo data and clear it after active variable analysis.
  SmallVector<FieldInfo *, 128> AllFields;
  MapVector<std::pair<Value *, signed>, FieldInfo *> FieldMap;
  // Mapping whether a V is a stack pointer that contains gcptr, and clear it
  // after active variable analysis.
  MapVector<Value *, bool> StackContainGCPtrMap;
  // Avoid duplicate liveness analysis in the BB.
  MapVector<BasicBlock *, std::pair<Instruction *, StructLiveSetTy>>
      InstLiveSetCache;
};

struct LivenessData {
  Function &F;
  DominatorTree &DT;
  PostDominatorTree &PostDT;
  unsigned OptLevel;
  AllocaAnalysisData &AllocaData;
  SmallVectorImpl<CallBase *> &ToUpdate;
  GCLiveAnalysis *GCLA = nullptr;

  LivenessData(Function &F, DominatorTree &DT, PostDominatorTree &PostDT,
               unsigned OptLevel, AllocaAnalysisData &AllocaData,
               SmallVectorImpl<CallBase *> &ToUpdate)
      : F(F), DT(DT), PostDT(PostDT), OptLevel(OptLevel),
        AllocaData(AllocaData), ToUpdate(ToUpdate) {
    GCLA = new GCLiveAnalysis(F, DT);
  }

  ~LivenessData() { deleteGCLA(); }

  LivenessData(LivenessData &&) = delete;
  LivenessData(const LivenessData &) = delete;
  LivenessData &operator=(LivenessData &&) = delete;
  LivenessData &operator=(const LivenessData &) = delete;

  void deleteGCLA() {
    if (GCLA) {
      delete GCLA;
      GCLA = nullptr;
    }
  }

  void findLiveReferences(MutableArrayRef<SafepointRecord> Records);

  void recomputeLiveInValues(CallBase *Call, SafepointRecord &Info,
                             PointerToBaseTy &PointerToBase);

  /// Given an updated version of the dataflow liveness results, update the
  /// liveset and base pointer maps for the call site CS.
  void recomputeLiveInValues(MutableArrayRef<SafepointRecord> Records,
                             PointerToBaseTy &PointerToBase);

  void computeGCFieldLiveness(MutableArrayRef<SafepointRecord> Records);
  void computeGCPtrLiveness(MutableArrayRef<SafepointRecord> Records);
};

#endif // LLVM_TRANSFORMS_SCALAR_GCLIVEANALYSIS_H
