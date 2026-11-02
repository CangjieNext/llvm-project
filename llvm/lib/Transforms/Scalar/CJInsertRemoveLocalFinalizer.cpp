//===- CJInsertRemoveLocalFinalizer.cpp - ---------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Pair every CJ_MCC_AddLocalFinalizer with a CJ_MCC_RemoveLocalFinalizer at the
// end of the object's live range, on every control-flow path, exactly once.
//
// "Right after the last use" is not that point: a path that never uses the
// object (the other arm of a branch, a landing pad, a loop exit) has no last
// use at all, and a use reached from several paths would be unregistered
// several times.
//
// The point we want is the dead-out frontier of the object's live range.
// Liveness is closed under predecessors -- if a value is live at p it is live
// at every point from which p is reachable -- so the live range is left exactly
// once on every path, and inserting at the frontier gives exactly one Remove
// per path:
//
//   * block-internal death: no representative is live out of B
//       -> insert after the last point in B that defines or uses one;
//   * edge death: some representative is live out of B, but none is live on the
//     edge B -> S
//       -> insert on that edge, splitting it when it is critical.
//
// A "representative" is any SSA value that keeps the object alive: the object
// itself, everything derived from it by bitcast / addrspacecast / GEP / select
// / phi, and -- to bridge the store->load round trip through a local variable
// -- the address of every stack slot it is stored into plus every value loaded
// from such a slot. Including the slot address is what keeps the live range
// connected across memory: without it the object would look dead right after
// the store.
//
// If the object reaches memory we cannot see through (a heap field, a slot
// whose address escapes), no Remove is emitted for it at all. Leaking a
// registration is far less harmful than unregistering an object that is still
// reachable.
//
// The pass keys off the CJ_MCC_AddLocalFinalizer calls rather than off the
// allocation: after CJRuntimeLowering both heap and local finalizer objects are
// produced by CJ_MCC_NewFinalizer, so the Add call is the only thing that still
// tells the two apart.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJInsertRemoveLocalFinalizer.h"

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#define DEBUG_TYPE "cj-insert-remove-local-finalizer"

using namespace llvm;

namespace {

const char AddLocalFinalizerName[] = "CJ_MCC_AddLocalFinalizer";
const char RemoveLocalFinalizerName[] = "CJ_MCC_RemoveLocalFinalizer";

using RepSet = SmallPtrSet<Value *, 8>;
using BlockSets = DenseMap<const BasicBlock *, RepSet>;
using Edge = std::pair<BasicBlock *, BasicBlock *>;

// Walk the address chain of \p AI. Returns false if the slot is used in any way
// other than loading/storing through it, which would let its contents flow
// somewhere we cannot follow. On success \p Addrs receives the alloca and every
// cast/GEP of it, so that touching the slot counts as keeping the object alive.
bool collectSlotAddresses(AllocaInst *AI, SmallPtrSetImpl<Value *> &Addrs) {
  SmallVector<Value *, 8> Worklist{AI};
  Addrs.insert(AI);
  while (!Worklist.empty()) {
    Value *V = Worklist.pop_back_val();
    for (User *U : V->users()) {
      if (auto *LI = dyn_cast<LoadInst>(U)) {
        if (LI->getPointerOperand() != V)
          return false;
      } else if (auto *SI = dyn_cast<StoreInst>(U)) {
        // Storing through the slot is fine; storing the slot's own address
        // somewhere else is not.
        if (SI->getPointerOperand() != V)
          return false;
      } else if (isa<BitCastInst>(U) || isa<AddrSpaceCastInst>(U) ||
                 isa<GetElementPtrInst>(U)) {
        if (Addrs.insert(U).second)
          Worklist.push_back(U);
      } else if (auto *II = dyn_cast<IntrinsicInst>(U)) {
        Intrinsic::ID ID = II->getIntrinsicID();
        if (ID != Intrinsic::lifetime_start && ID != Intrinsic::lifetime_end &&
            !isa<DbgInfoIntrinsic>(II))
          return false;
      } else {
        return false;
      }
    }
  }
  return true;
}

// Collect every value that keeps \p Obj alive into \p Reps. Returns false if the
// object escapes into memory that cannot be tracked, in which case no Remove may
// be emitted for it.
bool collectRepresentatives(Value *Obj, ArrayRef<LoadInst *> AllLoads,
                            RepSet &Reps) {
  SmallVector<Value *, 8> Worklist;
  SmallPtrSet<AllocaInst *, 4> Slots;
  auto AddRep = [&](Value *V) {
    if (Reps.insert(V).second)
      Worklist.push_back(V);
  };
  AddRep(Obj);
  while (!Worklist.empty()) {
    Value *V = Worklist.pop_back_val();
    for (User *U : V->users()) {
      if (auto *SI = dyn_cast<StoreInst>(U)) {
        if (SI->getValueOperand() != V)
          continue; // V is only the destination, that is a plain use
        auto *AI =
            dyn_cast<AllocaInst>(getUnderlyingObject(SI->getPointerOperand()));
        if (AI == nullptr)
          return false; // stored into memory we do not own
        if (!Slots.insert(AI).second)
          continue;
        SmallPtrSet<Value *, 8> Addrs;
        if (!collectSlotAddresses(AI, Addrs))
          return false; // the slot's contents can escape
        for (Value *Addr : Addrs)
          Reps.insert(Addr);
        // Anything ever loaded from this slot may be the object.
        for (LoadInst *LI : AllLoads)
          if (Addrs.count(LI->getPointerOperand()))
            AddRep(LI);
        continue;
      }
      if (isa<BitCastInst>(U) || isa<AddrSpaceCastInst>(U) ||
          isa<GetElementPtrInst>(U) || isa<SelectInst>(U) || isa<PHINode>(U))
        AddRep(U);
    }
  }
  return true;
}

// Backward liveness dataflow restricted to \p Reps. A value used by a phi is
// live on the incoming edge rather than in the phi's own block, so those uses
// seed the predecessor's live-out set instead of the block's use set.
//
// A representative phi additionally seeds *every* incoming edge, not just the
// ones whose incoming value is itself a representative. Such a phi defines a
// representative in the middle of the live range, which would otherwise be a
// second entry into it: a path entering through the phi's other operand would
// leave the range twice and unregister the object twice. Extending the range
// back over all incoming edges keeps it single-entry, so it is still left
// exactly once on every path.
void computeRepLiveness(Function &F, const RepSet &Reps, BlockSets &Defs,
                        BlockSets &LiveIn, BlockSets &LiveOut) {
  BlockSets UpExposed;
  for (BasicBlock &BB : F) {
    RepSet &Def = Defs[&BB];
    RepSet &Up = UpExposed[&BB];
    for (Instruction &I : reverse(BB)) {
      if (Reps.count(&I)) {
        Def.insert(&I);
        Up.erase(&I);
      }
      if (isa<PHINode>(I))
        continue;
      for (Value *Op : I.operands())
        if (Reps.count(Op))
          Up.insert(Op);
    }
    RepSet &Out = LiveOut[&BB];
    for (BasicBlock *Succ : successors(&BB))
      for (PHINode &PN : Succ->phis()) {
        if (Reps.count(&PN))
          Out.insert(&PN);
        Value *V = PN.getIncomingValueForBlock(&BB);
        if (Reps.count(V))
          Out.insert(V);
      }
    LiveIn[&BB] = Up;
  }

  SmallSetVector<BasicBlock *, 32> Worklist;
  for (BasicBlock &BB : F)
    Worklist.insert(&BB);
  while (!Worklist.empty()) {
    BasicBlock *BB = Worklist.pop_back_val();
    RepSet &Out = LiveOut[BB];
    for (BasicBlock *Succ : successors(BB))
      Out.insert(LiveIn[Succ].begin(), LiveIn[Succ].end());
    RepSet &In = LiveIn[BB];
    const size_t OldSize = In.size();
    for (Value *V : Out)
      if (!Defs[BB].count(V))
        In.insert(V);
    if (In.size() != OldSize)
      for (BasicBlock *Pred : predecessors(BB))
        Worklist.insert(Pred);
  }
}

class LocalFinalizerRemover {
public:
  LocalFinalizerRemover(Function &F, DominatorTree &DT)
      : F(F), DT(DT), Bdr(F.getContext()) {}

  bool run();

private:
  void process(Instruction *Obj);
  void emitRemove(Value *Obj, const DebugLoc &DL);
  void emitRemoveAfter(Value *Obj, Instruction *After);
  void emitRemoveOnEdges(Value *Obj, ArrayRef<Edge> DeadEdges);

  Function &F;
  DominatorTree &DT;
  IRBuilder<> Bdr;
  Function *RemoveFunc = nullptr;
  SmallVector<LoadInst *, 32> AllLoads;
};

void LocalFinalizerRemover::emitRemove(Value *Obj, const DebugLoc &DL) {
  Value *Arg = Bdr.CreatePointerBitCastOrAddrSpaceCast(
      Obj, RemoveFunc->getFunctionType()->getParamType(0));
  Bdr.CreateCall(RemoveFunc, {Arg})->setDebugLoc(DL);
}

// Insert the Remove call directly after \p After. A run of phi nodes must stay
// contiguous at the top of its block, so a phi is not inserted after but
// skipped past.
void LocalFinalizerRemover::emitRemoveAfter(Value *Obj, Instruction *After) {
  BasicBlock *BB = After->getParent();
  BasicBlock::iterator IP = isa<PHINode>(After)
                                ? BB->getFirstInsertionPt()
                                : std::next(After->getIterator());
  Bdr.SetInsertPoint(BB, IP);
  emitRemove(Obj, After->getDebugLoc());
}

// Insert one Remove call on each of \p DeadEdges. Placing the call on the edge
// rather than at the top of the successor is what keeps a shared successor (a
// landing pad reached from several invokes, a merge block) from getting one
// call per incoming edge, and what keeps the object's definition dominating the
// call.
void LocalFinalizerRemover::emitRemoveOnEdges(Value *Obj,
                                              ArrayRef<Edge> DeadEdges) {
  const DebugLoc DL = cast<Instruction>(Obj)->getDebugLoc();

  MapVector<BasicBlock *, SmallSetVector<BasicBlock *, 2>> BySucc;
  for (const Edge &E : DeadEdges)
    BySucc[E.second].insert(E.first);

  for (auto &KV : BySucc) {
    BasicBlock *Succ = KV.first;
    // When every edge into Succ is dead the call belongs in Succ itself: no
    // path reaches Succ with the object still live, and no edge needs a split.
    // This is the common shape for a landing pad shared by invokes that all
    // consume the object.
    SmallPtrSet<BasicBlock *, 4> AllPreds(pred_begin(Succ), pred_end(Succ));
    if (KV.second.size() == AllPreds.size()) {
      Bdr.SetInsertPoint(Succ, Succ->getFirstInsertionPt());
      emitRemove(Obj, DL);
      continue;
    }
    for (BasicBlock *Pred : KV.second) {
      Instruction *TI = Pred->getTerminator();
      if (TI->getNumSuccessors() == 1) {
        // Not a critical edge, and a single-successor terminator cannot be
        // consuming the object itself.
        Bdr.SetInsertPoint(TI);
        emitRemove(Obj, DL);
        continue;
      }
      BasicBlock *NewBB = nullptr;
      if (Succ->isEHPad()) {
        // SplitEdge/ehAwareSplitEdge build funclet pads, which do not fit the
        // landing-pad EH the Cangjie runtime uses.
        if (!isa<LandingPadInst>(Succ->getFirstNonPHI())) {
          LLVM_DEBUG(dbgs() << DEBUG_TYPE ": cannot split edge into "
                            << Succ->getName() << ", no "
                            << RemoveLocalFinalizerName << " there\n");
          continue;
        }
        SmallVector<BasicBlock *, 2> NewBBs;
        SplitLandingPadPredecessors(Succ, {Pred}, ".fin", ".fin.lp", NewBBs,
                                    &DT);
        NewBB = NewBBs[0];
      } else {
        NewBB = SplitEdge(Pred, Succ, &DT);
      }
      if (NewBB == nullptr)
        continue;
      Bdr.SetInsertPoint(NewBB->getTerminator());
      emitRemove(Obj, DL);
    }
  }
}

void LocalFinalizerRemover::process(Instruction *ObjDef) {
  Value *Obj = ObjDef;
  RepSet Reps;
  if (!collectRepresentatives(Obj, AllLoads, Reps)) {
    LLVM_DEBUG(dbgs() << DEBUG_TYPE ": local finalizer escapes, no "
                      << RemoveLocalFinalizerName << " emitted for " << *Obj
                      << "\n");
    return;
  }

  BlockSets Defs, LiveIn, LiveOut;
  computeRepLiveness(F, Reps, Defs, LiveIn, LiveOut);

  // Is any representative live on the edge BB -> Succ? A phi operand is live on
  // its incoming edge even though the phi is defined in Succ, so it has to be
  // checked separately from Succ's live-in set.
  auto liveOnEdge = [&](BasicBlock *BB, BasicBlock *Succ) {
    if (!LiveIn[Succ].empty())
      return true;
    // Mirrors the live-out seeding in computeRepLiveness.
    for (PHINode &PN : Succ->phis())
      if (Reps.count(&PN) || Reps.count(PN.getIncomingValueForBlock(BB)))
        return true;
    return false;
  };

  // In valid SSA liveness implies the definition dominates, with one exception:
  // on the unwind edge of the invoke that produces the object there is no
  // object, and CJ_MCC_AddLocalFinalizer was never reached either.
  auto defAvailableOnEdge = [&](BasicBlock *BB, BasicBlock *Succ) {
    if (auto *II = dyn_cast<InvokeInst>(ObjDef))
      if (II->getParent() == BB && Succ == II->getUnwindDest())
        return false;
    return DT.dominates(ObjDef->getParent(), BB);
  };

  SmallVector<Instruction *, 4> AfterPoints;
  SmallVector<Edge, 4> DeadEdges;

  for (BasicBlock &BB : F) {
    if (LiveOut[&BB].empty()) {
      // Nothing survives BB: the object dies here, if it lived here at all.
      Instruction *Last = nullptr;
      for (Instruction &I : BB) {
        if (Reps.count(&I)) {
          Last = &I;
          continue;
        }
        if (isa<PHINode>(I))
          continue; // a phi's operands are used on the incoming edge
        for (Value *Op : I.operands())
          if (Reps.count(Op)) {
            Last = &I;
            break;
          }
      }
      if (Last == nullptr)
        continue; // no representative activity in this block
      if (!Last->isTerminator()) {
        // A representative loaded from a reused slot can appear on paths the
        // object itself never reached; require the definition to be available.
        if (Last != ObjDef && !DT.dominates(Obj, Last))
          continue;
        AfterPoints.push_back(Last);
        continue;
      }
      // The terminator itself consumes the object (it is passed to an invoke):
      // it is dead on every outgoing edge. A terminator without successors
      // (`ret obj`) hands the object to the caller, so there is nothing to
      // unregister here.
      for (BasicBlock *Succ : successors(&BB))
        if (defAvailableOnEdge(&BB, Succ))
          DeadEdges.emplace_back(&BB, Succ);
      continue;
    }
    for (BasicBlock *Succ : successors(&BB))
      if (!liveOnEdge(&BB, Succ) && defAvailableOnEdge(&BB, Succ))
        DeadEdges.emplace_back(&BB, Succ);
  }

  for (Instruction *At : AfterPoints)
    emitRemoveAfter(Obj, At);
  if (!DeadEdges.empty())
    emitRemoveOnEdges(Obj, DeadEdges);
}

bool LocalFinalizerRemover::run() {
  Module &M = *F.getParent();
  Function *AddFunc = M.getFunction(AddLocalFinalizerName);
  if (AddFunc == nullptr || AddFunc->use_empty())
    return false;

  // Collect the registered objects in program order, and every load in one
  // sweep. Inserting Remove calls adds no loads, so AllLoads stays complete.
  SmallVector<Instruction *, 8> Objs;
  SmallPtrSet<Value *, 8> Seen;
  for (Instruction &I : instructions(F)) {
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      AllLoads.push_back(LI);
      continue;
    }
    auto *CB = dyn_cast<CallBase>(&I);
    if (CB == nullptr || CB->getCalledFunction() != AddFunc ||
        CB->arg_size() < 1)
      continue;
    auto *Obj = dyn_cast<Instruction>(CB->getArgOperand(0));
    if (Obj == nullptr) {
      LLVM_DEBUG(dbgs() << DEBUG_TYPE ": registered value is not an "
                           "instruction, skipping "
                        << *CB << "\n");
      continue;
    }
    if (Seen.insert(Obj).second)
      Objs.push_back(Obj);
  }
  if (Objs.empty())
    return false;

  // Already paired up, e.g. because the pass ran twice over this function.
  Function *Existing = M.getFunction(RemoveLocalFinalizerName);
  if (Existing != nullptr) {
    llvm::erase_if(Objs, [&](Instruction *Obj) {
      return llvm::any_of(Obj->users(), [&](User *U) {
        auto *CB = dyn_cast<CallBase>(U);
        return CB != nullptr && CB->getCalledFunction() == Existing;
      });
    });
    if (Objs.empty())
      return false;
  }

  RemoveFunc = Existing;
  if (RemoveFunc == nullptr) {
    IRBuilder<> Decl(F.getContext());
    FunctionType *FT =
        FunctionType::get(Decl.getVoidTy(), {Decl.getInt8PtrTy(1)}, false);
    RemoveFunc = M.declareCJRuntimeFunc(RemoveLocalFinalizerName, FT, false);
    RemoveFunc->addFnAttr(Attribute::get(F.getContext(), "gc-leaf-function"));
  }

  for (Instruction *Obj : Objs)
    process(Obj);
  return true;
}

class CJInsertRemoveLocalFinalizerLegacyPass : public FunctionPass {
public:
  static char ID;

  explicit CJInsertRemoveLocalFinalizerLegacyPass() : FunctionPass(ID) {
    initializeCJInsertRemoveLocalFinalizerLegacyPassPass(
        *PassRegistry::getPassRegistry());
  }
  ~CJInsertRemoveLocalFinalizerLegacyPass() = default;

  bool runOnFunction(Function &F) override {
    if (F.isDeclaration())
      return false;
    auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
    return LocalFinalizerRemover(F, DT).run();
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<DominatorTreeWrapperPass>();
  }
};

} // namespace

PreservedAnalyses
CJInsertRemoveLocalFinalizer::run(Function &F, FunctionAnalysisManager &AM) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  if (!LocalFinalizerRemover(F, DT).run())
    return PreservedAnalyses::all();
  // Splitting critical edges changes the CFG.
  return PreservedAnalyses::none();
}

char CJInsertRemoveLocalFinalizerLegacyPass::ID = 0;

FunctionPass *llvm::createCJInsertRemoveLocalFinalizerLegacyPass() {
  return new CJInsertRemoveLocalFinalizerLegacyPass();
}

INITIALIZE_PASS_BEGIN(CJInsertRemoveLocalFinalizerLegacyPass,
                      "cj-insert-remove-local-finalizer",
                      "Cangjie Insert Remove Local Finalizer", false, false)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_END(CJInsertRemoveLocalFinalizerLegacyPass,
                    "cj-insert-remove-local-finalizer",
                    "Cangjie Insert Remove Local Finalizer", false, false)
