//===- CJGCLiveAnalysis.cpp - GC Live Analysis For Cangjie ------*- C++ -*-===//
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

#include "llvm/Transforms/Scalar/CJGCLiveAnalysis.h"

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/Triple.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CJIntrinsics.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/SafepointIRVerifier.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar/InsertCJTBAA.h"

#define DEBUG_TYPE "gc-live-analysis"

using namespace llvm;
using namespace cangjie;

// Print the liveset found at the insert location
static cl::opt<bool> PrintLiveSet("cj-spp-print-liveset", cl::Hidden,
                                  cl::init(false));

// Output log information and terminate the program.
void checkFailed(const Twine &Message, Instruction *I) {
  dbgs() << "Statepoint-Check: " << Message << "\n";
  dbgs() << " " << *I << "\n";
  I->getParent()->getParent()->print(dbgs());
  dbgs() << '\n';
  report_fatal_error("Broken function found, compilation aborted!");
}

#define Check(C, ...)                                                          \
  do {                                                                         \
    if (!(C)) {                                                                \
      checkFailed(__VA_ARGS__);                                                \
      return;                                                                  \
    }                                                                          \
  } while (false)

namespace llvm {
// Find all base defining values reachable from the initial.
// Return true if the terminator point is alloca, and if the struct contains
// ref of heap, save it to liveset, otherwise ignore it.
// Returns false if the base pointer is a gcptr.
bool findAllocaInsts(Value *V, SetVector<Value *> &BaseSet, bool HasArg) {
  assert(V->getType()->isPointerTy() &&
         "Illegal to ask for the base pointer of a non-pointer type");

  bool IsAlloca = false;
  bool IsGCPtr = false;
  SmallSetVector<Value *, 8> WorkList;
  WorkList.insert(V);

  // Caches all the phi and select point that have been found. This cache
  // ensures that the infinite loop of calling the phi does not occur.
  SetVector<Value *> Cache;

  while (!WorkList.empty()) {
    Value *Ptr = WorkList.pop_back_val();

    if (Cache.contains(Ptr)) {
      continue;
    }
    Cache.insert(Ptr);

    if (auto *PN = dyn_cast<PHINode>(Ptr)) {
      WorkList.insert(PN->incoming_values().begin(),
                      PN->incoming_values().end());
    } else if (auto *SI = dyn_cast<SelectInst>(Ptr)) {
      WorkList.insert(SI->getTrueValue());
      WorkList.insert(SI->getFalseValue());
    } else if (isa<GetElementPtrInst>(Ptr) || isa<CastInst>(Ptr)) {
      WorkList.insert(Ptr->stripInBoundsOffsets());
    } else if (isa<AllocaInst>(Ptr)) {
      if (isMemoryContainsGCPtrType(Ptr->getType())) {
        BaseSet.insert(Ptr);
      }
      IsAlloca = true;
    } else {
      assert((isa<Constant>(Ptr) || isa<CallInst>(Ptr) ||
              isa<InvokeInst>(Ptr) || isa<LoadInst>(Ptr) ||
              isa<Argument>(Ptr) || isa<ExtractValueInst>(Ptr)) &&
             "unexpected instructions in findStructPtrAlloca");
      if (isa<Argument>(Ptr) && HasArg) {
        BaseSet.insert(Ptr);
      }
      // If one of the base pointers is gcptr, return false.
      if (!isa<Constant>(Ptr) && isGCPointerType(Ptr->getType())) {
        IsGCPtr = true;
      }
    }
  }
  return IsGCPtr ? false : IsAlloca;
}
} // end namespace llvm

static void
prepareStructData(Function &F, SetVector<Value *> &AllStructs,
                  SmallSetVector<Value *, 8> &CallStructSet,
                  MapVector<Value *, Instruction *> &StructToMemsetMap,
                  SmallSet<Value *, 8> &InsertedMemset) {
  for (auto &I : instructions(F)) {
    if (auto MI = dyn_cast<MemSetInst>(&I)) {
      Value *Base = findMemoryBasePointer(MI->getArgOperand(0));
      if (InsertedMemset.contains(MI) && AllStructs.count(Base)) {
        StructToMemsetMap[Base] = MI;
      }
    }
    if (auto CB = dyn_cast<CallBase>(&I)) {
      if (isa<IntrinsicInst>(CB))
        continue;

      for (unsigned ArgIdx = 0; ArgIdx < CB->arg_size(); ++ArgIdx) {
        Value *Base = findMemoryBasePointer(CB->getArgOperand(ArgIdx));
        if (AllStructs.count(Base)) {
          CallStructSet.insert(Base);
        }
      }
    }
  }
}

using StructDefineTy = MapVector<Value *, SetVector<Instruction *>>;
static void computeStructDefineSet(BasicBlock &BB, StructDefineTy &StructDefs,
                                   SetVector<Value *> &AllStructs) {
  for (Instruction &I : BB) {
    Value *Base = nullptr;
    if (auto SI = dyn_cast<StoreInst>(&I)) {
      Base = findMemoryBasePointer(SI->getPointerOperand());
    } else if (auto II = dyn_cast<IntrinsicInst>(&I)) {
      Intrinsic::ID IID = II->getIntrinsicID();
      if (IID == Intrinsic::memset || IID == Intrinsic::memcpy ||
          IID == Intrinsic::memmove) {
        Base = findMemoryBasePointer(II->getArgOperand(0));
      }
    }

    if (Base != nullptr && AllStructs.count(Base)) {
      StructDefs[Base].insert(&I);
    }
  }
}

// Some redundant memsets can be deleted after liveness variable analysis.
static void eliminateDeadMemset(StructLiveData &StructLives, Function &F,
                                PostDominatorTree &PostDT,
                                AllocaAnalysisData &AllocaData,
                                SmallSet<Value *, 8> &InsertedMemset) {
  SetVector<Value *> &AllStructs = AllocaData.Structs;
  if (AllStructs.empty()) {
    return;
  }

  SmallSetVector<Value *, 8> CallStructSet;
  MapVector<Value *, Instruction *> StructToMemsetMap;
  prepareStructData(F, AllStructs, CallStructSet, StructToMemsetMap,
                    InsertedMemset);

  // record map <Struct, Set<Struct Defines>>
  MapVector<Value *, SetVector<Instruction *>> StructDefs;

  for (BasicBlock &BB : F) {
    if (!StructLives.KillSet[&BB].empty()) {
      computeStructDefineSet(BB, StructDefs, AllStructs);
    }
  }

  SmallSetVector<Instruction *, 8> DeadMemsets;
  SmallSetVector<Value *, 8> RecordAIs;

  for (auto &P : StructDefs) {
    Value *V = P.first;
    // If this AI does not have memset, or as the parameter of the call
    // instruction.do nothing.
    if (!StructToMemsetMap.count(V) || CallStructSet.count(V))
      continue;

    assert(P.second.size() > 0 && "The AI has no definition point!");
    // If the AI definition point is only memset, do nothing.
    if (P.second.size() == 1)
      continue;

    Instruction *MI = StructToMemsetMap[V];
    bool CanDominate = true;
    for (auto Def : P.second) {
      // If the define point cannot post-dominates the memset inst, we need
      // this memset inst.
      if (Def != MI && !PostDT.dominates(Def, MI)) {
        CanDominate = false;
        break;
      }
    }

    if (CanDominate) {
      DeadMemsets.insert(MI);
      RecordAIs.insert(V);
    }
  }

#ifndef NDEBUG
  if (RecordAIs.size() > 0) {
    LLVM_DEBUG(dbgs() << "Statepoint-print: Memset Opt\n");
    LLVM_DEBUG(dbgs() << "  Function: " << F.getName() << "\n");
    LLVM_DEBUG(dbgs() << "  Number of optimized memsets: " << RecordAIs.size()
                      << "\n");
    for (auto V : RecordAIs)
      LLVM_DEBUG(dbgs() << "    " << *V << "\n");
  }
#endif

  for (Instruction *MI : DeadMemsets) {
    MI->eraseFromParent();
  }
}

// Computes variables that are converted to type `i8 addrspace(1)*` but
// are actually pointers on the stack, and save it.
// If it contains gcptr, save it to ContainGCPtrsFakes.
void GCLiveAnalysis::computeFakeGCPtrs() {
  for (Instruction &I : instructions(F)) {
    Type *Ty = I.getType();
    if (!isGCPointerType(Ty)) {
      continue;
    }
    // BaseSet saves alloca inst that contain gcptr.
    SetVector<Value *> AllocaSet;
    if (findAllocaInsts(&I, AllocaSet)) {
      FakeGCPtrs.insert(&I);
      if (AllocaSet.size() > 0) {
        ContainGCPtrsFakes.insert(&I);
      }
    }
  }
}

#ifndef NDEBUG

/// Check that the items in 'Live' dominate 'TI'.  This is used as a basic
/// validation check for the liveness computation.
void GCLiveAnalysis::checkBasicSSA(SetVector<Value *> &Live, Instruction *TI,
                                   bool TermOkay) {
  for (Value *V : Live) {
    if (auto *I = dyn_cast<Instruction>(V)) {
      // The terminator can be a member of the LiveOut set.  LLVM's definition
      // of instruction dominance states that V does not dominate itself.  As
      // such, we need to special case this to allow it.
      if (TermOkay && TI == I)
        continue;
      assert(DT.dominates(I, TI) &&
             "basic SSA liveness expectation violated by gc-live analysis");
    }
  }
}

/// Check that all the liveness sets used during the computation of liveness
/// obey basic SSA properties.  This is useful for finding cases where we miss
/// a def.
void GCLiveAnalysis::checkBasicSSA(PtrLiveData &Data) {
  for (BasicBlock &BB : F) {
    checkBasicSSA(Data.LiveSet[&BB], BB.getTerminator());
    checkBasicSSA(Data.LiveOut[&BB], BB.getTerminator(), true);
    checkBasicSSA(Data.LiveIn[&BB], BB.getTerminator());
  }
}

#endif

// Conservatively identifies any definitions which might be live across the
// given parse point. For statepoint insertion we care about values that remain
// live *after* the current call finishes, not values used only as operands of
// the current call itself.
void GCLiveAnalysis::analyzeParsePointLiveness(CallBase *Call,
                                               SetVector<Value *> &LiveSet) {
  findLiveSetAtInst(Call, LiveSet);

#ifndef NDEBUG
  if (PrintLiveSet) {
    LLVM_DEBUG(dbgs() << "Safepoint For: "
                      << Call->getCalledOperand()->getName() << "\n");
    LLVM_DEBUG(dbgs() << "  Number live values: " << LiveSet.size() << "\n");
    LLVM_DEBUG(dbgs() << "  Live Variables:\n");
    for (Value *V : LiveSet)
      LLVM_DEBUG(dbgs() << "    " << V->getName() << " " << *V << "\n");
  }
#endif
}

// liveness computation via standard dataflow
// -------------------------------------------------------------------
// Compute the live-in set for the location rbegin starting from
// the live-out set of the basic block
void GCLiveAnalysis::computeLiveInValues(BasicBlock::reverse_iterator Begin,
                                         BasicBlock::reverse_iterator End,
                                         SetVector<Value *> &LiveTmp) {
  for (auto &I : make_range(Begin, End)) {
    // KILL/Def - Remove this definition from LiveIn for RegisterType
    LiveTmp.remove(&I);

    // Don't consider *uses* in PHI nodes, we handle their contribution to
    // predecessor blocks when we seed the LiveOut sets
    if (isa<PHINode>(I))
      continue;

    // USE - Add to the LiveIn set for this instruction
    for (Value *V : I.operands()) {
      if (isa<Constant>(V))
        continue;

      if (isHandledGCPointerType(V->getType()) && !FakeGCPtrs.count(V)) {
        // The choice to exclude all things constant here is slightly subtle.
        // There are two independent reasons:
        // - We assume that things which are constant (from LLVM's definition)
        // do not move at runtime.  For example, the address of a global
        // variable is fixed, even though it's contents may not be.
        // - Second, we can't disallow arbitrary inttoptr constants even
        // if the language frontend does.  Optimization passes are free to
        // locally exploit facts without respect to global reachability.  This
        // can create sections of code which are dynamically unreachable and
        // contain just about anything.  (see constants.ll in tests)
        LiveTmp.insert(V);
      }
    }
  }
}

void GCLiveAnalysis::computeLiveOutSeed(BasicBlock *BB,
                                        SetVector<Value *> &LiveTmp) {
  for (BasicBlock *Succ : successors(BB)) {
    for (auto &I : *Succ) {
      PHINode *PN = dyn_cast<PHINode>(&I);
      if (!PN)
        break;

      Value *V = PN->getIncomingValueForBlock(BB);
      if (isHandledGCPointerType(V->getType()) && !isa<Constant>(V) &&
          !FakeGCPtrs.count(V)) {
        LiveTmp.insert(V);
      }
    }
  }
}

void GCLiveAnalysis::computeKillSet(BasicBlock *BB,
                                    SetVector<Value *> &KillSet) {
  for (Instruction &I : *BB) {
    if (isHandledGCPointerType(I.getType())) {
      KillSet.insert(&I);
    }
  }
}

/// Compute the live-in set for every basic block in the function
void GCLiveAnalysis::computeLiveness() {
  SmallSetVector<BasicBlock *, 32> Worklist;
  InstLiveSetCache.clear();

  // Seed the liveness for each individual block
  for (BasicBlock &BB : F) {
    Data.KillSet[&BB].clear();
    computeKillSet(&BB, Data.KillSet[&BB]);
    Data.LiveSet[&BB].clear();
    computeLiveInValues(BB.rbegin(), BB.rend(), Data.LiveSet[&BB]);

    Data.LiveOut[&BB] = SetVector<Value *>();
    computeLiveOutSeed(&BB, Data.LiveOut[&BB]);
    Data.LiveIn[&BB] = Data.LiveOut[&BB];
    Data.LiveIn[&BB].set_subtract(Data.KillSet[&BB]);
    Data.LiveIn[&BB].set_union(Data.LiveSet[&BB]);
    if (!Data.LiveIn[&BB].empty())
      Worklist.insert(pred_begin(&BB), pred_end(&BB));
  }

  // Propagate that liveness until stable
  while (!Worklist.empty()) {
    BasicBlock *BB = Worklist.pop_back_val();

    // Compute our new liveout set, then exit early if it hasn't changed
    // despite the contribution of our successor.
    SetVector<Value *> LiveOut = Data.LiveOut[BB];
    const auto OldLiveOutSize = LiveOut.size();
    for (BasicBlock *Succ : successors(BB)) {
      assert(Data.LiveIn.count(Succ));
      LiveOut.set_union(Data.LiveIn[Succ]);
    }
    // assert OutLiveOut is a subset of LiveOut
    if (OldLiveOutSize == LiveOut.size()) {
      // If the sets are the same size, then we didn't actually add anything
      // when unioning our successors LiveIn.  Thus, the LiveIn of this block
      // hasn't changed.
      continue;
    }
    Data.LiveOut[BB] = LiveOut;

    // Apply the effects of this basic block
    SetVector<Value *> LiveTmp = LiveOut;
    LiveTmp.set_subtract(Data.KillSet[BB]);
    LiveTmp.set_union(Data.LiveSet[BB]);

    assert(Data.LiveIn.count(BB));
    const SetVector<Value *> &OldLiveIn = Data.LiveIn[BB];
    // assert: OldLiveIn is a subset of LiveTmp
    if (OldLiveIn.size() != LiveTmp.size()) {
      Data.LiveIn[BB] = LiveTmp;
      Worklist.insert(pred_begin(BB), pred_end(BB));
    }
  }

#ifndef NDEBUG
  // Verify our output against SSA properties.  This helps catch any
  // missing kills during the above iteration.
  checkBasicSSA(Data);
#endif
}

/// Given results from the dataflow liveness computation, find the set of values
/// live immediately after the given instruction. The instruction result itself
/// is never considered live at that point.
void GCLiveAnalysis::findLiveSetAtInst(Instruction *Inst,
                                       SetVector<Value *> &Out) {
  BasicBlock *BB = Inst->getParent();

  // Note: The copy is intentional and required
  assert(Data.LiveOut.count(BB));
  SetVector<Value *> LiveOut = InstLiveSetCache.count(BB)
                                   ? InstLiveSetCache[BB].second
                                   : Data.LiveOut[BB];
  auto BI = InstLiveSetCache.count(BB)
                ? InstLiveSetCache[BB].first->getIterator().getReverse()
                : BB->rbegin();
  // Liveness for a parse point is defined at the program point immediately
  // after the call. This trims values that are consumed only by the current
  // call and are dead afterwards.
  computeLiveInValues(BI, Inst->getIterator().getReverse(), LiveOut);
  LiveOut.remove(Inst);
  // For FFI/runtime calls (callee does not have cangjie GC), the callee has
  // no cangjie safepoint and cannot relocate GC pointer arguments itself.
  // The caller must record these pointers in gc-live so the runtime updates
  // the caller's frame slot when GC moves the object during the call.
  // For cangjie-to-cangjie calls (callee has gc "cangjie"), the callee has
  // its own safepoints that handle relocation of its incoming arguments.
  // The caller can safely trim these call args, which keeps GCRelocates
  // empty and enables STATEPOINT_TAIL_CALL for such calls.
  if (auto *Call = dyn_cast<CallBase>(Inst)) {
    Function *Callee = Call->getCalledFunction();
    bool CalleeHasCangjieGC =
        Callee && Callee->hasGC() && Callee->getGC() == "cangjie";
    if (!CalleeHasCangjieGC) {
      for (Value *Arg : Call->args()) {
        if (isa<Constant>(Arg))
          continue;
        if (isHandledGCPointerType(Arg->getType()) && !FakeGCPtrs.count(Arg))
          LiveOut.insert(Arg);
      }
    }
  }
  Out.insert(LiveOut.begin(), LiveOut.end());
  InstLiveSetCache[BB] = {Inst, Out};
}

FieldInfo *StructLiveAnalysis::getFieldInfo(Value *Ptr, int Offset) {
  auto Key = std::make_pair(Ptr, Offset);
  if (FieldMap.count(Key))
    return FieldMap[Key];

  FieldInfo *FI = new FieldInfo(Ptr, Offset);
  AllFields.push_back(FI);
  FieldMap[Key] = FI;
  return FI;
}

FieldInfo *StructLiveAnalysis::getFieldInfoByValue(Value *Ptr) {
  APInt Offsets(DL.getIndexSizeInBits(0), 0);
  Value *PtrBase = Ptr->stripAndAccumulateConstantOffsets(DL, Offsets, false);
  // If it is a phi or select instruction, the offset is set to 0. Otherwise,
  // record the offset on the base.
  if (isa<PHINode>(PtrBase) || isa<SelectInst>(PtrBase) ||
      AllocaData.LargeStructs.contains(PtrBase)) {
    return getFieldInfo(PtrBase, -1);
  }
  return getFieldInfo(PtrBase, Offsets.getZExtValue());
}

// Checks whether a range contains gcptr based on the struct type.
bool StructLiveAnalysis::containGCPtrByOffset(Value *Ptr, uint64_t Size) {
  APInt Offsets(DL.getIndexSizeInBits(0), 0);
  Value *Base = Ptr->stripAndAccumulateConstantOffsets(DL, Offsets, false);
  if (isa<AllocaInst>(Base) && Size > 0) {
    uint64_t Begin = Offsets.getZExtValue();
    uint64_t End = Begin + Size;
    MapVector<uint64_t, bool> &OffsetMap =
        AllocaData.StructLayoutGCPtrMap[Base];
    for (auto &Pair : OffsetMap) {
      if (Pair.first >= Begin && Pair.first < End) {
        if (Pair.second) {
          return true;
        }
      }
    }
  }
  return false;
}

// The base of phi is an alloca and contains gcptr.
bool StructLiveAnalysis::isBaseContainGCPtr(Value *V) {
  Value *Base = findMemoryBasePointer(V);
  if (isMemoryContainsGCPtrType(Base->getType())) {
    return true;
  }

  if ((isa<PHINode>(V) || isa<SelectInst>(V)) &&
      AllocaData.PhiToAllocasMap.count(V)) {
    if (!AllocaData.PhiToAllocasMap[V].empty()) {
      return true;
    }
  }
  return false;
}

#ifndef NDEBUG

/// Check that the items in 'Live' dominate 'TI'.  This is used as a basic
/// validation check for the liveness computation.
void StructLiveAnalysis::checkBasicSSA(SetVector<FieldInfo *> &Live,
                                       Instruction *TI, bool TermOkay) {
  for (FieldInfo *FI : Live) {
    if (auto *I = dyn_cast<Instruction>(FI->V)) {
      // The terminator can be a member of the LiveOut set.  LLVM's definition
      // of instruction dominance states that V does not dominate itself.  As
      // such, we need to special case this to allow it.
      if (TermOkay && TI == I)
        continue;
      assert(DT.dominates(I, TI) &&
             "basic SSA liveness expectation violated by struct-live analysis");
    }
  }
}

/// Check that all the liveness sets used during the computation of liveness
/// obey basic SSA properties.  This is useful for finding cases where we miss
/// a def.
void StructLiveAnalysis::checkBasicSSA(StructLiveData &Data) {
  for (BasicBlock &BB : F) {
    checkBasicSSA(Data.LiveSet[&BB], BB.getTerminator());
    checkBasicSSA(Data.LiveOut[&BB], BB.getTerminator(), true);
    checkBasicSSA(Data.LiveIn[&BB], BB.getTerminator());
  }
}

#endif

void StructLiveAnalysis::initializeAlloca(AllocaInst *AI) {
  IRBuilder<> IRB(AI);
  Instruction *PostI = AI->getNextNode();
  IRB.SetInsertPoint(PostI);
  Type *AllocaType = AI->getAllocatedType();
  if (auto *ST = dyn_cast<StructType>(AllocaType)) {
    Value *Int8Ptr = IRB.CreateBitCast(AI, IRB.getInt8PtrTy());
    cast<Instruction>(Int8Ptr)->setDebugLoc(AI->getDebugLoc());
    uint64_t Size = DL.getStructLayout(ST)->getSizeInBytes();
    CallInst *CI = IRB.CreateMemSet(Int8Ptr, IRB.getInt8(0), Size, Align(8));
    CI->setDebugLoc(AI->getDebugLoc());
    InsertedMemset.insert(CI);
  } else {
    assert(isGCPointerType(AllocaType) && "The alloca type is not supported.");
    StoreInst *SI = IRB.CreateStore(
        ConstantPointerNull::get(cast<PointerType>(AllocaType)), AI);
    SI->setDebugLoc(AI->getDebugLoc());
  }
}

void StructLiveAnalysis::insertInitializerAfterLifetime(Value *V,
                                                        CallInst *CI) {
  IRBuilder<> IRB(CI->getNextNode());
  AllocaInst *AI = cast<AllocaInst>(V);
  Type *AllocaType = AI->getAllocatedType();
  if (AllocaType->isStructTy()) {
    CallInst *Call = IRB.CreateMemSet(CI->getArgOperand(1), IRB.getInt8(0),
                                      CI->getArgOperand(0), Align(8), false);
    Call->setDebugLoc(CI->getDebugLoc());
    InsertedMemset.insert(CI);
  } else if (AllocaType->isPointerTy()) {
    StoreInst *SI = IRB.CreateStore(
        ConstantPointerNull::get(cast<PointerType>(AllocaType)), AI);
    SI->setDebugLoc(CI->getDebugLoc());
  } else {
    checkFailed("AllocaInst Type error", AI);
  }
}

void StructLiveAnalysis::moveInitializerAfterLifetime(Value *V, Instruction *I,
                                                      CallInst *CI) {
  if (CallInst *MI = dyn_cast<CallInst>(I)) {
    MI->setArgOperand(0, CI->getArgOperand(1));
    MI->setArgOperand(2, CI->getArgOperand(0));
    MI->moveAfter(CI);
    MI->setDebugLoc(CI->getDebugLoc());
  } else if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
    SI->eraseFromParent();
    insertInitializerAfterLifetime(V, CI);
  } else {
    checkFailed("Intruction Type error", I);
  }
}

// For an alloca that contain gcptr to be used in phi, we need to insert an
// initializer in entry and lifetime.start.
void StructLiveAnalysis::insertMemsetForPhiStruct() {
  SetVector<Value *> AllocaStructs;
  SetVector<Value *> AllocaGCPtrs;
  MapVector<Value *, CallInst *> LifetimeStarts;
  MapVector<Value *, CallInst *> Memsets;
  MapVector<Value *, StoreInst *> StoreNulls;
  for (auto &I : instructions(F)) {
    if (CallInst *CI = dyn_cast<CallInst>(&I)) {
      if (CI->getIntrinsicID() == Intrinsic::lifetime_start) {
        Value *Base = CI->getArgOperand(1)->stripPointerCasts();
        if (isa<AllocaInst>(Base) &&
            isMemoryContainsGCPtrType(Base->getType())) {
          LifetimeStarts.insert({Base, CI});
        }
      } else if (CI->getIntrinsicID() == Intrinsic::memset) {
        Value *Base = CI->getArgOperand(0)->stripPointerCasts();
        if (isa<AllocaInst>(Base) &&
            isMemoryContainsGCPtrType(Base->getType()) &&
            isa<ConstantInt>(CI->getArgOperand(1))) {
          Memsets.insert({Base, CI});
        }
      }
      continue;
    }

    if (StoreInst *SI = dyn_cast<StoreInst>(&I)) {
      Constant *C = dyn_cast<Constant>(SI->getValueOperand());
      if (C && C->isNullValue() && isGCPointerType(C->getType())) {
        Value *Base = SI->getPointerOperand()->stripPointerCasts();
        if (isa<AllocaInst>(Base))
          StoreNulls.insert({Base, SI});
      }
      continue;
    }

    if (isa<PHINode>(&I) || isa<SelectInst>(&I)) {
      if (!I.getType()->isPointerTy())
        continue;

      SetVector<Value *> Allocas;
      findAllocaInsts(&I, Allocas);
      for (auto V : Allocas) {
        AllocaInst *AI = cast<AllocaInst>(V);
        Type *Ty = AI->getAllocatedType();
        if (Ty->isStructTy()) {
          AllocaStructs.insert(AI);
        } else if (Ty->isPointerTy()) {
          assert(isGCPointerType(Ty) && "Unsupport Alloca pointer type!");
          AllocaGCPtrs.insert(AI);
        } else {
          checkFailed("Unsupport Alloca type!", AI);
        }
      }
    }
    if (isa<AllocaInst>(&I) && AllocaData.LargeStructs.contains(&I))
      AllocaStructs.insert(&I);
  }

  for (auto P : LifetimeStarts) {
    Value *V = P.first;
    CallInst *Lifetime = P.second;
    if (AllocaStructs.contains(V)) {
      if (Memsets.count(V)) {
        CallInst *MI = Memsets[V];
        // Do nothing if there is a memset in lifetime.start
        if (MI->getParent() == Lifetime->getParent()) {
          continue;
        }
        if (MI->getParent()->isEntryBlock()) {
          insertInitializerAfterLifetime(V, Lifetime);
          continue;
        }
        // move it to lifetime
        moveInitializerAfterLifetime(V, MI, Lifetime);
      } else {
        // Create a memset for alloca struct.
        insertInitializerAfterLifetime(V, Lifetime);
      }
      continue;
    }
    if (AllocaGCPtrs.contains(V)) {
      if (StoreNulls.count(V)) {
        StoreInst *SI = StoreNulls[V];
        // Do nothing if there is a store null in lifetime.start
        if (SI->getParent() == Lifetime->getParent()) {
          continue;
        }
        if (SI->getParent()->isEntryBlock()) {
          insertInitializerAfterLifetime(V, Lifetime);
          continue;
        }
        // move it to lifetime
        moveInitializerAfterLifetime(V, SI, Lifetime);
      } else {
        // Craete a store null for alloca gcptr.
        insertInitializerAfterLifetime(V, Lifetime);
      }
    }
  }

  // Filter out the alloca that has been initialized in the entry.
  for (auto &I : F.getEntryBlock()) {
    if (auto *SI = dyn_cast<StoreInst>(&I)) {
      Value *Ptr = SI->getPointerOperand()->stripPointerCasts();
      if (AllocaStructs.contains(Ptr)) {
        AllocaStructs.remove(Ptr);
      } else if (AllocaGCPtrs.contains(Ptr)) {
        AllocaGCPtrs.remove(Ptr);
      }
    } else if (auto *II = dyn_cast<MemIntrinsic>(&I)) {
      Value *Ptr = II->getArgOperand(0)->stripPointerCasts();
      if (AllocaStructs.contains(Ptr)) {
        AllocaStructs.remove(Ptr);
      } else if (AllocaGCPtrs.contains(Ptr)) {
        AllocaGCPtrs.remove(Ptr);
      }
    }
  }

  for (auto V : AllocaStructs) {
    initializeAlloca(cast<AllocaInst>(V));
  }
  for (auto V : AllocaGCPtrs) {
    initializeAlloca(cast<AllocaInst>(V));
  }
}

/// Compute the struct-live for each instruction.
void StructLiveAnalysis::computeFieldLiveInValues(
    BasicBlock::reverse_iterator Begin, BasicBlock::reverse_iterator End,
    SetVector<FieldInfo *> &LiveTmp) {
  for (auto &I : make_range(Begin, End)) {
    SetVector<FieldInfo *> MemDefSet;
    SetVector<FieldInfo *> MemUseSet;
    if (hasMemoryDefineOrUseValue(&I, MemDefSet, MemUseSet) != MemoryNone) {
      LiveTmp.set_subtract(MemDefSet);
      LiveTmp.set_union(MemUseSet);
    }
  }
}

void StructLiveAnalysis::computeFieldLiveOutSeed(
    BasicBlock *BB, SetVector<FieldInfo *> &LiveOuts) {
  for (BasicBlock *Succ : successors(BB)) {
    for (auto &I : *Succ) {
      PHINode *PN = dyn_cast<PHINode>(&I);
      if (!PN)
        continue;

      for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i) {
        Value *V = PN->getIncomingValue(i);
        if (!isStackContainGCPtr(V))
          continue;

        // Record incoming value if it is the phi value of the current BB.
        if (PN->getIncomingBlock(i) == BB) {
          addGCFieldsByMemory(V, LiveOuts);
        } else {
          // Otherwise, record all base values for the phi values of other BBs.
          SetVector<Value *> BaseSet;
          findAllocaInsts(V, BaseSet);
          for (auto Base : BaseSet) {
            addGCFields(Base, LiveOuts);
          }
        }
      }
    }
  }
}

void StructLiveAnalysis::computeFieldKillSet(BasicBlock *BB,
                                             SetVector<FieldInfo *> &KillSet) {
  for (Instruction &I : *BB) {
    SetVector<FieldInfo *> MemDefSet;
    SetVector<FieldInfo *> MemUseSet;

    MemoryAccess Status = hasMemoryDefineOrUseValue(&I, MemDefSet, MemUseSet);
    if (Status & DefineMemory) {
      KillSet.set_union(MemDefSet);
    }
  }
}

/// Compute the live-in set for every basic block in the function
void StructLiveAnalysis::computeLiveness() {
  InstLiveSetCache.clear();
  SmallSetVector<BasicBlock *, 32> Worklist;

  // Seed the liveness for each individual block
  for (BasicBlock &BB : F) {
    Data.KillSet[&BB].clear();
    computeFieldKillSet(&BB, Data.KillSet[&BB]);
    Data.LiveSet[&BB].clear();
    computeFieldLiveInValues(BB.rbegin(), BB.rend(), Data.LiveSet[&BB]);
    Data.LiveOut[&BB] = SetVector<FieldInfo *>();
    computeFieldLiveOutSeed(&BB, Data.LiveOut[&BB]);
    Data.LiveIn[&BB] = Data.LiveOut[&BB];
    Data.LiveIn[&BB].set_subtract(Data.KillSet[&BB]);
    Data.LiveIn[&BB].set_union(Data.LiveSet[&BB]);
    if (!Data.LiveIn[&BB].empty())
      Worklist.insert(pred_begin(&BB), pred_end(&BB));
  }

  // Propagate that liveness until stable
  while (!Worklist.empty()) {
    BasicBlock *BB = Worklist.pop_back_val();

    // Compute our new liveout set, then exit early if it hasn't changed
    // despite the contribution of our successor.
    SetVector<FieldInfo *> LiveOut = Data.LiveOut[BB];
    const auto OldLiveOutSize = LiveOut.size();
    for (BasicBlock *Succ : successors(BB)) {
      assert(Data.LiveIn.count(Succ));
      LiveOut.set_union(Data.LiveIn[Succ]);
    }
    // assert OutLiveOut is a subset of LiveOut
    if (OldLiveOutSize == LiveOut.size()) {
      // If the sets are the same size, then we didn't actually add anything
      // when unioning our successors LiveIn.  Thus, the LiveIn of this block
      // hasn't changed.
      continue;
    }
    Data.LiveOut[BB] = LiveOut;

    // Apply the effects of this basic block
    SetVector<FieldInfo *> LiveTmp = LiveOut;
    LiveTmp.set_subtract(Data.KillSet[BB]);
    LiveTmp.set_union(Data.LiveSet[BB]);

    assert(Data.LiveIn.count(BB));
    const SetVector<FieldInfo *> &OldLiveIn = Data.LiveIn[BB];
    // assert: OldLiveIn is a subset of LiveTmp
    if (OldLiveIn.size() != LiveTmp.size()) {
      Data.LiveIn[BB] = LiveTmp;
      Worklist.insert(pred_begin(BB), pred_end(BB));
    }
  }

#ifndef NDEBUG
  // Verify our output against SSA properties.  This helps catch any
  // missing kills during the above iteration.
  checkBasicSSA(Data);
#endif
}

/// Given results from the dataflow liveness computation, find the set of live
/// Values at a particular instruction.
void StructLiveAnalysis::findLiveSetAtInst(Instruction *Inst,
                                           StructLiveData &Data,
                                           StructLiveSetTy &Out) {
  BasicBlock *BB = Inst->getParent();

  // Note: The copy is intentional and required
  assert(Data.LiveOut.count(BB));
  StructLiveSetTy LiveOut = InstLiveSetCache.count(BB)
                                ? InstLiveSetCache[BB].second
                                : Data.LiveOut[BB];
  auto BI = InstLiveSetCache.count(BB)
                ? InstLiveSetCache[BB].first->getIterator().getReverse()
                : BB->rbegin();
  // We want to handle the statepoint itself oddly.  It's
  // call result is not live (normal), nor are it's arguments
  // (unless they're used again later).  This adjustment is
  // specifically what we need to relocate
  computeFieldLiveInValues(BI, ++Inst->getIterator().getReverse(), LiveOut);
  Out.insert(LiveOut.begin(), LiveOut.end());
  InstLiveSetCache[BB] = {Inst, Out};
}

void StructLiveAnalysis::analyzeParsePointLiveness(
    CallBase *Call, SetVector<FieldInfo *> &LiveSet) {
  findLiveSetAtInst(Call, Data, LiveSet);

#ifndef NDEBUG
  if (PrintLiveSet) {
    LLVM_DEBUG(dbgs() << "Safepoint For: "
                      << Call->getCalledOperand()->getName() << "\n");
    LLVM_DEBUG(dbgs() << "  Number struct-live values: " << LiveSet.size()
                      << "\n");
    LLVM_DEBUG(dbgs() << "  StructLive Variables:\n");
    for (FieldInfo *FI : LiveSet)
      LLVM_DEBUG(dbgs() << "    " << FI->V->getName() << " " << FI->Offset
                        << "\n");
  }
#endif
}

void StructLiveAnalysis::legalizeMemoryValueSet(StructLiveSetTy &LiveSet) {
  StructLiveSetTy LegalizedSet;
  for (FieldInfo *FI : LiveSet) {
    if (isa<PHINode>(FI->V) || isa<SelectInst>(FI->V))
      LegalizedSet.insert(FI);
  }

  for (FieldInfo *FI : LegalizedSet) {
    // Caches all the base pointers of phi and select.
    SetVector<Value *> AllocaSet;
    findAllocaInsts(FI->V, AllocaSet);
    LiveSet.remove(FI);
    for (auto AI : AllocaSet) {
      addGCFields(AI, LiveSet);
    }
  }

  // remove all pointers that are constants.
  LiveSet.remove_if([&](FieldInfo *LiveV) { return isa<Constant>(LiveV->V); });
}

// Identify V or the base of V is a stack pointer that contains gcptr.
bool StructLiveAnalysis::isStackContainGCPtr(Value *V, uint64_t Size) {
  PointerType *PT = dyn_cast<PointerType>(V->getType());
  if (!PT)
    return false;

  if (StackContainGCPtrMap.count(V))
    return StackContainGCPtrMap[V];

  bool Ret = false;
  if (isGCPointerType(PT)) {
    // Check whether the base of V is FakeGCptr and contains refs.
    if (isFakeContainGCPtrs(V)) {
      Ret = true;
    }
    StackContainGCPtrMap[V] = Ret;
    return Ret;
  }
  // This is actually a global value.
  if (isa<Constant>(findMemoryBasePointer(V))) {
    Ret = false;
  } else {
    Value *Def = V->stripPointerCasts();
    if (isMemoryContainsGCPtrType(Def->getType())) {
      Ret = true;
    } else if (Def->getType()->getNonOpaquePointerElementType()->isStructTy()) {
      if (containGCPtrByOffset(Def, Size)) {
        Ret = true;
      } else {
        Ret = false;
      }
    } else if (isBaseContainGCPtr(Def)) {
      Ret = true;
    }
  }

  StackContainGCPtrMap[V] = Ret;
  return Ret;
}

// V must be an alloca instruction. The gcptr in alloca is calculated and
// added to the set.
void StructLiveAnalysis::addGCFields(Value *V, SetVector<FieldInfo *> &Set,
                                     uint64_t Begin, uint64_t End) {
  AllocaInst *AI = dyn_cast<AllocaInst>(V);
  if (!AI)
    report_fatal_error("Only the alloca inst is supported in addGCFields!");

  if (AllocaData.LargeStructs.contains(V)) {
    Set.insert(getFieldInfo(V, -1));
    return;
  }

  Type *Ty = AI->getAllocatedType();
  if (isGCPointerType(Ty)) {
    Set.insert(getFieldInfo(V, 0));
    return;
  }
  assert(AllocaData.StructLayoutGCPtrMap.count(V) &&
         "StructLayoutGCPtrMap lacks V information.");

  // Others, record gc fields in struct based on StructLayoutGCPtrMap.
  bool AllGCField = End == 0;
  MapVector<uint64_t, bool> &OffsetMap = AllocaData.StructLayoutGCPtrMap[V];
  for (auto &Pair : OffsetMap) {
    // Add a range of ref fields to Set.
    if ((Pair.first >= Begin && Pair.first < End) || AllGCField) {
      if (Pair.second) {
        Set.insert(getFieldInfo(V, Pair.first));
      }
    }
  }
}

void StructLiveAnalysis::addGCFieldsByBase(Value *V,
                                           SetVector<FieldInfo *> &Set) {
  assert(isa<PointerType>(V->getType()) && "Base is not pointer type!");

  Value *Base = findMemoryBasePointer(V);
  if (isa<PHINode>(Base) || isa<SelectInst>(Base)) {
    Set.insert(getFieldInfoByValue(Base));
    return;
  }
  if (isa<AllocaInst>(Base)) {
    addGCFields(Base, Set);
    return;
  }

  Check(isa<Constant>(Base) || isa<LoadInst>(Base) || isa<Argument>(Base),
        "Unsupport stack base", cast<Instruction>(Base));
}

void StructLiveAnalysis::addGCFieldsByValue(Value *V,
                                            SetVector<FieldInfo *> &Set) {
  assert(isa<PointerType>(V->getType()) && "Base is not pointer type!");
  FieldInfo *Field = getFieldInfoByValue(V);
  Value *Base = Field->V;

  if (auto *AI = dyn_cast<AllocaInst>(Base)) {
    if (AI->getAllocatedType()->isStructTy()) {
      assert(AllocaData.StructLayoutGCPtrMap.count(Base) &&
             "StructLayoutGCPtrMap lacks AllocaInst information.");
      if (Field->Offset == -1 ||
          AllocaData.StructLayoutGCPtrMap[Base][Field->Offset]) {
        Set.insert(Field);
      }
    } else if (AI->getAllocatedType()->isPointerTy()) {
      Set.insert(Field);
    } else {
      assert(false && "Unsupported AllocaInst type");
    }
    return;
  }

  if (isa<PHINode>(Base) || isa<SelectInst>(Base)) {
    Set.insert(Field);
    return;
  }

  // The index of GEP is a variable. The offset cannot be calculated here.
  if (isa<GetElementPtrInst>(Base)) {
    addGCFieldsByBase(Base, Set);
    return;
  }

  Check(isa<Constant>(Base) || isa<Argument>(Base), "Unsupport stack value",
        cast<Instruction>(Base));
}

// Records a range of fields during memory instruction operations, if the size
// is 0, record all the gc fields in base.
void StructLiveAnalysis::addGCFieldsByMemory(Value *V,
                                             SetVector<FieldInfo *> &AllocaSet,
                                             uint64_t Size) {
  if (!isa<PointerType>(V->getType()))
    return;

  Value *Ptr = V->stripPointerCasts();
  if (!isStackContainGCPtr(Ptr, Size))
    return;

  if (Size > 0) {
    APInt Offsets(DL.getIndexSizeInBits(0), 0);
    Value *PtrBase = V->stripAndAccumulateConstantOffsets(DL, Offsets, false);
    // If it is a alloca instruction, record the range of gc fields in the base.
    if (isa<AllocaInst>(PtrBase)) {
      uint64_t Begin = Offsets.getZExtValue();
      uint64_t End = Begin + Size;
      addGCFields(PtrBase, AllocaSet, Begin, End);
      return;
    }
  }

  // Otherwise, record the all gc fields of the base.
  addGCFieldsByBase(Ptr, AllocaSet);
}

void StructLiveAnalysis::visitMemoryLoad(LoadInst *LI,
                                         SetVector<FieldInfo *> &AllocaUses) {
  Value *Ptr = LI->getPointerOperand()->stripPointerCasts();
  Type *ValType = LI->getType();

  if (!ValType->isPointerTy() || !isStackContainGCPtr(Ptr))
    return;

  Value *Base = findMemoryBasePointer(Ptr);
  if (auto *BaseLI = dyn_cast<LoadInst>(Base)) {
    assert(isa<Constant>(findMemoryBasePointer(BaseLI->getPointerOperand())) &&
           "If load inst is nested, its source must be a global variable!");
    return;
  }

  addGCFieldsByValue(Ptr, AllocaUses);
}

void StructLiveAnalysis::visitMemoryStore(StoreInst *SI,
                                          SetVector<FieldInfo *> &AllocaDefs) {
  Value *Ptr = SI->getPointerOperand();
  if (!isStackContainGCPtr(Ptr)) {
    return;
  }

  Type *ValType = SI->getValueOperand()->getType();
  // On 32-bit ARM a pointer is 4 bytes, so a `store i32` (e.g. zeroing the
  // payload field) can define a GC field. Route it through the pointer/i64
  // branch so it is recorded as a def; otherwise the else branch skips it as
  // a partial store and the GC scans stale data (SIGSEGV on a null gctib).
  const Triple TT(SI->getModule()->getTargetTriple());
  if (ValType->isPointerTy() || ValType->isIntegerTy(64) ||
      (TT.isARM() && ValType->isIntegerTy(32))) {
    FieldInfo *Field = getFieldInfoByValue(Ptr);
    Value *Base = Field->V;
    if (isa<AllocaInst>(Base) || isa<Argument>(Base))
      AllocaDefs.insert(Field);
  } else {
    Value *BasePtr = findMemoryBasePointer(Ptr);
    // Only if the PointerType and ValueType of Store Inst are equal,
    // we consider this store inst to be a define point.
    // And do nothing for partial store.
    if (ValType == cast<PointerType>(BasePtr->getType())->getElementType())
      addGCFieldsByMemory(BasePtr, AllocaDefs);
  }
}

void StructLiveAnalysis::visitMemoryIntrinsic(
    IntrinsicInst *II, SetVector<FieldInfo *> &AllocaDefs,
    SetVector<FieldInfo *> &AllocaUses) {
  switch (II->getIntrinsicID()) {
  default:
    // fall through to general call handling
    break;
  // cj.gcwrite.static.struct(AS(0) *dstPtr, AS(0) *srcPtr, size)
  case Intrinsic::cj_gcwrite_static_struct: {
    uint64_t MemSize = 0;
    if (auto Size = dyn_cast<ConstantInt>(getSize(II))) {
      MemSize = Size->getZExtValue();
    }
    addGCFieldsByMemory(getSource(II), AllocaUses, MemSize);
    break;
  }
  // cj.gcwrite.struct(AS(1) *basePtr, AS(1) *dstPtr, anyAS *srcPtr, size)
  case Intrinsic::cj_gcwrite_struct: {
    uint64_t MemSize = 0;
    if (auto Size = dyn_cast<ConstantInt>(getSize(II))) {
      MemSize = Size->getZExtValue();
    }
    addGCFieldsByMemory(getSource(II), AllocaUses, MemSize);
    break;
  }
  // cj.gcread.static.struct(AS(0) *dstPtr, AS(0) *srcPtr, size)
  case Intrinsic::cj_gcread_static_struct: {
    uint64_t MemSize = 0;
    if (auto Size = dyn_cast<ConstantInt>(getSize(II))) {
      MemSize = Size->getZExtValue();
    }
    addGCFieldsByMemory(getDest(II), AllocaDefs, MemSize);
    break;
  }
  // cj.gcread.struct(AS(0) *dstPtr, AS(1) *basePtr, AS(1) *srcPtr, size)
  case Intrinsic::cj_gcread_struct: {
    uint64_t MemSize = 0;
    if (auto Size = dyn_cast<ConstantInt>(getSize(II))) {
      MemSize = Size->getZExtValue();
    }
    addGCFieldsByMemory(getDest(II), AllocaDefs, MemSize);
    addGCFieldsByMemory(getSource(II), AllocaUses, MemSize);
    break;
  }
  case Intrinsic::memcpy:
  case Intrinsic::memmove: {
    uint64_t MemSize = 0;
    if (auto Size = dyn_cast<ConstantInt>(II->getArgOperand(2))) {
      MemSize = Size->getZExtValue();
    }
    addGCFieldsByMemory(II->getArgOperand(0), AllocaDefs, MemSize);
    addGCFieldsByMemory(II->getArgOperand(1), AllocaUses, MemSize);
    break;
  }
  case Intrinsic::memset: {
    uint64_t MemSize = 0;
    if (auto Size = dyn_cast<ConstantInt>(II->getArgOperand(2))) {
      MemSize = Size->getZExtValue();
    }
    addGCFieldsByMemory(II->getArgOperand(0), AllocaDefs, MemSize);
    break;
  }
  case Intrinsic::cj_array_copy_ref:
  case Intrinsic::cj_array_copy_struct:
  case Intrinsic::cj_array_copy_generic: {
    uint64_t MemSize = 0;
    if (auto Size = dyn_cast<ConstantInt>(getSize(II))) {
      MemSize = Size->getZExtValue();
    }
    addGCFieldsByMemory(getDest(II), AllocaDefs, MemSize);
    addGCFieldsByMemory(getSource(II), AllocaUses, MemSize);
    break;
  }
  case Intrinsic::lifetime_start:
    // lifetime_start means the begin of using a stack ptr.
    // we treat this ptr as a kill
    addGCFieldsByMemory(II->getArgOperand(1), AllocaDefs);
    break;
  case Intrinsic::lifetime_end:
    // lifetime_start means the end of using a stack ptr.
    // we do nothing here
    break;
  }
}

void StructLiveAnalysis::visitMemoryCallBase(
    CallBase *CB, SetVector<FieldInfo *> &AllocaDefs,
    SetVector<FieldInfo *> &AllocaUses) {
  if (auto *II = dyn_cast<IntrinsicInst>(CB)) {
    visitMemoryIntrinsic(II, AllocaDefs, AllocaUses);
    return;
  }

  // CallInst and InvokeInst
  // In the call instruction, the parameter are considered as use point.
  unsigned ArgNum = CB->arg_size();
  if (ArgNum == 0) {
    return;
  }

  for (unsigned ArgIdx = 0; ArgIdx < ArgNum; ++ArgIdx) {
    Function *Callee = CB->getCalledFunction();
    // If the SSA value is not used in the callee, no need to record it.
    if (Callee &&
        (Callee->hasParamAttribute(ArgIdx, Attribute::ReadNone) ||
         Callee->hasParamAttribute(ArgIdx, Attribute::WriteOnly)) &&
        Callee->hasParamAttribute(ArgIdx, Attribute::NoCapture))
      continue;

    unsigned Size = 0;
    Value *Arg = CB->getArgOperand(ArgIdx);
    Type *Ty = Arg->getType();
    if (Ty->isPointerTy() && !Ty->isOpaquePointerTy()) {
      auto *Pointee = Ty->getNonOpaquePointerElementType();
      if (isa<StructType>(Pointee) || isa<ArrayType>(Pointee))
        Size =
            static_cast<unsigned>(DL.getTypeAllocSize(Pointee).getFixedSize());
    }

    addGCFieldsByMemory(Arg, AllocaUses, Size);
  }
}

void StructLiveAnalysis::visitMemorySelect(SelectInst *SI,
                                           SetVector<FieldInfo *> &AllocaDefs,
                                           SetVector<FieldInfo *> &AllocaUses) {
  if (!SI->getType()->isPointerTy() || !isStackContainGCPtr(SI))
    return;

  AllocaDefs.insert(getFieldInfoByValue(SI));
  if (isStackContainGCPtr(SI->getTrueValue())) {
    addGCFieldsByValue(SI->getTrueValue(), AllocaUses);
  }
  if (isStackContainGCPtr(SI->getFalseValue())) {
    addGCFieldsByValue(SI->getFalseValue(), AllocaUses);
  }
}

// If it is an instruction that contains the memory parameter, then handles
// use and define values according to the instruction type.
MemoryAccess StructLiveAnalysis::hasMemoryDefineOrUseValue(
    Instruction *I, SetVector<FieldInfo *> &AllocaDefs,
    SetVector<FieldInfo *> &AllocaUses) {
  // For each memory instruction, identifies and processes use and define,
  // otherwise do nothing.
  switch (I->getOpcode()) {
  default:
    break;
  case Instruction::Load:
    visitMemoryLoad(cast<LoadInst>(I), AllocaUses);
    break;
  case Instruction::Store:
    visitMemoryStore(cast<StoreInst>(I), AllocaDefs);
    break;
  case Instruction::Call:
  case Instruction::Invoke:
    visitMemoryCallBase(cast<CallBase>(I), AllocaDefs, AllocaUses);
    break;
  case Instruction::Select:
    visitMemorySelect(cast<SelectInst>(I), AllocaDefs, AllocaUses);
    break;
  case Instruction::PHI:
    AllocaDefs.insert(getFieldInfoByValue(I));
    break;
  }

  int Status = 0;
  if (!AllocaDefs.empty())
    Status |= static_cast<int>(DefineMemory);
  if (!AllocaUses.empty())
    Status |= static_cast<int>(UseMemory);
  return static_cast<MemoryAccess>(Status);
}

void StructLiveAnalysis::combineStructGCField(StructLiveSetTy &FieldInfos,
                                              SetVector<Value *> &Structs,
                                              CallBase *Call) {
  StructLayoutGCPtrTy &StructDL = AllocaData.StructLayoutGCPtrMap;
  MapVector<Value *, unsigned> RecordGCFieldNum;
  for (FieldInfo *FI : FieldInfos) {
    Value *Base = FI->V;
    int Offset = FI->Offset;
    // If the offset is -1, record alloca SSA.
    if (Offset == -1) {
      Structs.insert(Base);
      continue;
    }
    assert(Offset >= 0 && "FieldInfos offset error.");
    if (StructDL.count(Base) && StructDL[Base][Offset]) {
      if (++RecordGCFieldNum[Base] == AllocaData.StructGCFieldNum[Base]) {
        Structs.insert(Base);
      }
    }
  }
}

void StructLiveAnalysis::insertGEPForField(CallBase *Call,
                                           StructLiveSetTy &FieldInfos,
                                           LiveSetTy &FieldSet) {
  if (FieldInfos.empty())
    return;

  SetVector<Value *> Structs;
  // If all fields of a struct are alive, record it instead of fields.
  combineStructGCField(FieldInfos, Structs, Call);

  IRBuilder<> Builder(Call);
  for (FieldInfo *FI : FieldInfos) {
    Value *Base = FI->V;
    int Offset = FI->Offset;
    assert(isMemoryContainsGCPtrType(Base->getType()) &&
           "Unexpected values are recorded in struct-live.");
    auto *AI = dyn_cast<AllocaInst>(Base);
    if (!AI) {
      continue;
    }

    if (AI->getAllocatedType()->isPointerTy()) {
      FieldSet.insert(AI);
    } else if (AI->getAllocatedType()->isStructTy()) {
      assert(AllocaData.StructLayoutGCPtrMap.count(AI) &&
             "StructLayoutGCPtrMap lacks V information.");
      // Directly record struct instead of recording its field, which avoids
      // oprands expansion.
      if (Structs.contains(Base)) {
        FieldSet.insert(AI);
        continue;
      }

      assert(Offset >= 0 && "FieldInfos offset error.");
      assert(AllocaData.StructLayoutGCPtrMap[AI][Offset] &&
             "FieldInfos contains incorrect value.");

      Value *Int8Ptr = Builder.CreateBitCast(Base, Builder.getInt8PtrTy());
      cast<Instruction>(Int8Ptr)->setMetadata(
          "noreloc", MDNode::get(Builder.getContext(), {}));
      Value *GEP = Builder.CreateGEP(Builder.getInt8Ty(), Int8Ptr,
                                     Builder.getInt64(Offset), "field", true);
      FieldSet.insert(GEP);
    } else {
      checkFailed("Unexpected type in insertGEPForField", AI);
    }
  }
}

void LivenessData::findLiveReferences(
    MutableArrayRef<SafepointRecord> Records) {
  for (size_t i = 0; i < Records.size(); i++) {
    // The stack check function does not need to compute the GC pointer.
    Function *Callee = ToUpdate[i]->getCalledFunction();
    if (Callee && Callee->isCangjieStackCheck()) {
      Records[i].isCJStackCheck = true;
      break;
    }
  }

  computeGCFieldLiveness(Records);
  computeGCPtrLiveness(Records);
}

void LivenessData::recomputeLiveInValues(CallBase *Call, SafepointRecord &Info,
                                         PointerToBaseTy &PointerToBase) {
  LiveSetTy Updated;
  GCLA->findLiveSetAtInst(Call, Updated);

  // We may have base pointers which are now live that weren't before.  We
  // need to update the PointerToBase structure to reflect this.
  for (auto V : Updated)
    PointerToBase.insert({V, V});

  Info.LiveSet = Updated;

  for (auto V : Updated) {
    // Records them whose base is alloca contained gcptr.
    SetVector<Value *> AllocaSet;
    findAllocaInsts(V, AllocaSet);
    if (!AllocaSet.empty()) {
      Info.FieldSet.insert(AllocaSet.begin(), AllocaSet.end());
    }
  }
}

/// Given an updated version of the dataflow liveness results, update the
/// liveset and base pointer maps for the call site CS.
void LivenessData::recomputeLiveInValues(
    MutableArrayRef<SafepointRecord> Records, PointerToBaseTy &PointerToBase) {
  GCLA->computeLiveness();
  for (size_t i = 0; i < Records.size(); i++) {
    // The stack check function does not need to compute the GC pointer.
    if (Records[i].isCJStackCheck)
      continue;

    SafepointRecord &info = Records[i];
    recomputeLiveInValues(ToUpdate[i], info, PointerToBase);
  }
  deleteGCLA();
}

void LivenessData::computeGCFieldLiveness(
    MutableArrayRef<SafepointRecord> Records) {
  StructLiveAnalysis StructLA(F, DT, AllocaData);

  // In the case of phi struct, we need to record all structs. To ensure
  // that the struct is initialized, we insert memset. It is optimized
  // after Livenesss analysis. Only necessary memsets are retained.
  StructLA.insertMemsetForPhiStruct();

  StructLA.computeLiveness();

  for (size_t i = 0; i < Records.size(); i++) {
    // The stack check function does not need to compute the GC pointer.
    if (Records[i].isCJStackCheck)
      continue;

    /// Records the [base+offset] format of the live gc fields.
    StructLiveSetTy FieldLives;
    StructLA.analyzeParsePointLiveness(ToUpdate[i], FieldLives);

    // If phi and select exist, delete them and record their alloca SSAs.
    StructLA.legalizeMemoryValueSet(FieldLives);

    // Generate the SSA value of `gep a + x` for each struct field.
    StructLA.insertGEPForField(ToUpdate[i], FieldLives, Records[i].FieldSet);
  }

  // Eliminate dead cjmemset. Only works at O2, because cjdb needs it at O0.
  if (OptLevel == 2) {
    eliminateDeadMemset(StructLA.Data, F, PostDT, AllocaData,
                        StructLA.InsertedMemset);
  }
}

void LivenessData::computeGCPtrLiveness(
    MutableArrayRef<SafepointRecord> Records) {
  GCLA->computeLiveness();

  for (size_t i = 0; i < Records.size(); i++) {
    // The stack check function does not need to compute the GC pointer.
    if (Records[i].isCJStackCheck)
      continue;

    GCLA->analyzeParsePointLiveness(ToUpdate[i], Records[i].LiveSet);
  }
}
