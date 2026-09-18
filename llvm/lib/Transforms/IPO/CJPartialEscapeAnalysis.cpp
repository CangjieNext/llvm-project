//===- CJPartialEscapeAnalysis.cpp - escape analysis for Cangjie ----------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file implements escape analysis, and trans non-escape object from heap
// to stack if possible.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/CJPartialEscapeAnalysis.h"

#include "queue"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/Analysis/DomTreeUpdater.h"
#include "llvm/Analysis/LazyCallGraph.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/SafepointIRVerifier.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/CJFillMetadata.h"
#include "llvm/Transforms/Utils/CallGraphUpdater.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"

#include <algorithm>

#define DEBUG_TYPE "escape-analysis"
using namespace llvm;

constexpr int alignEight = 8;

// Maximum depth of the cumulative-offset path carried by spreadMemEscape.
// Deeper propagations are collapsed to the escape-all offset (-1), which
// bounds the number of distinct (node, Offsets) states
constexpr unsigned MaxSpreadDepth = 16;

// Maximum meaningful field offset for a MemPtr. Offsets are collapsed to
// the unique escape-all value -1, keeping the number of distinct
// (node, offset) propagation states bounded.
constexpr int MaxSpreadOffset = 4096;

// Effective offset bound: once per-object sizes are computed, use the tighter
// of the object-size-aware upper bound (max object size among this analysis'
// GCNews; offsets beyond it cannot be legitimate field offsets) and the static
// MaxSpreadOffset fallback. Before that (during initialize) MaxObjSize is
// still being accumulated, so keep the bound stable at MaxSpreadOffset.
static unsigned getSpreadOffsetBound(const CJEscapeAnalysis &EA) {
  if (!EA.MaxObjSizeComputed)
    return MaxSpreadOffset;
  if (EA.MaxObjSize != 0 &&
      EA.MaxObjSize < static_cast<unsigned>(MaxSpreadOffset))
    return EA.MaxObjSize;
  return MaxSpreadOffset;
}
enum InitializeState : unsigned { NotInitialize, Initializing, Initialized };
static cl::opt<unsigned> CJMaxStackObject("cj-max-stack-obj", cl::Hidden,
                                          cl::init(1024));
static cl::opt<unsigned> CJMaxNonMoveArraySize("cj-max-nonmove-array-size",
                                               cl::Hidden, cl::init(64 * 1024));
namespace llvm {
cl::opt<bool> CJDisablePEA("cj-disable-partial-ea",
                                  cl::Hidden, cl::init(false));
cl::opt<bool> CJEASupportFinalizer("cj-ea-support-finalizer", cl::Hidden,
                                   cl::init(true));

GlobalVariable *getNewKlass(CallBase *I) {
  GlobalVariable *Klass =
      dyn_cast<GlobalVariable>(I->getArgOperand(0)->stripPointerCasts());
  if (Klass && (Klass->getMetadata("RelatedType") != 0)) {
    return Klass;
  }
  return nullptr;
}

void setEscapeMeta(Instruction *I) {
  assert(isa<AllocaInst>(I) && "Only add meta for alloca inst.");
  I->setMetadata("ea_val", MDNode::get(I->getContext(), {}));
}

Type *getAllocaType(GlobalVariable *Klass, bool &HasRef, uint32_t &AS,
                    bool &NonMovArray, Instruction *Val) {
  Module *M = Klass->getParent();
  LLVMContext &C = M->getContext();
  StructType *ST = getTypeLayoutType(Klass);
  const DataLayout &DL = M->getDataLayout();
  uint32_t ObjSize =
      static_cast<unsigned>(DL.getTypeAllocSize(ST).getFixedSize());
  // Struct Body is null
  if (ObjSize == 0) {
    AS = ObjectHeadSize;
    HasRef = false;
    return ST;
  }
  // none-array type
  StructType *ArrayBaseType = dyn_cast<StructType>(ST->getElementType(0));
  if (!ArrayBaseType || ArrayBaseType->getName() != "ArrayBase") {
    ObjSize += ObjectHeadSize;
    ObjSize = (ObjSize + 7) & (~(7)); // 8 bytes alignment
    AS = ObjSize;
    if (AS > CJMaxStackObject) {
      return nullptr; // obj size too large
    }
    HasRef = containsGCPtrType(ST);
    return ST;
  }
  Value *Size = Val->getOperand(1);
  uint64_t ArraySize = 0;
  if (auto ConstantSize = dyn_cast<ConstantInt>(Size)) {
    ArraySize = ConstantSize->getZExtValue();
  } else {
    return nullptr; // unknown array size
  }

  Type *ElementType =
      dyn_cast<ArrayType>(ST->getElementType(1))->getElementType();
  unsigned ElementSize =
      static_cast<unsigned>(DL.getTypeAllocSize(ElementType).getFixedSize());
  // ElemSize may be 0, when that is RawArray<Unit>.ti and Unit.type is {}.
  if (ElementSize && (ArraySize >= CJMaxStackObject / ElementSize)) {
    if (ArraySize > CJMaxNonMoveArraySize / ElementSize) {
      NonMovArray = true;
    }
    return nullptr; // obj size too large.
  }
  AS = ArraySize * ElementSize + ArrayHeadSize;

  if (ArraySize != 0) {
    HasRef = containsGCPtrType(ElementType);
  }
  SmallVector<Type *, 4> ElementTypes;
  ElementTypes.push_back(ArrayBaseType);
  ElementTypes.push_back(ArrayType::get(ElementType, ArraySize));
  return StructType::get(C, ElementTypes);
}

// NEW func which can rewrite.
bool isRewriteableCangjieMallocFunc(Function *F) {
  auto FuncName = F->getName();
  if (FuncName.startswith("CJ_MCC_NewObject") ||
      FuncName.startswith("CJ_MCC_NewArray") ||
      FuncName.startswith("CJ_MCC_NewFinalizer") ||
      FuncName.startswith("CJ_MCC_NewObjArray")) {
    return true;
  }
  return false;
}

bool isCangjieNewFinalizerFunc(Function *F) {
  return F->getName().startswith("CJ_MCC_NewFinalizer");
}

// Get the finalizer method (~init) for a class described by the given TypeInfo
// global variable.
Function *getFinalizerMethod(GlobalVariable *Klass, Module *M) {
  if (Klass == nullptr || !Klass->hasInitializer())
    return nullptr;

  TypeInfo TI(Klass);

  if (!TI.isGenericType()) {
    Value *Ptr = TI.get(CIT_GENERIC_FROM)->stripPointerCasts();
    if (auto *F = dyn_cast<Function>(Ptr))
      return F;
  }

  LLVM_DEBUG(dbgs() << "Cannot extract finalizer method from TypeInfo.\n");
  return nullptr;
}

// True if some path from CreateBB reaches a function exit that is not a
// normal return (unreachable / resume). Invoke itself is not an exit: both
// the normal and unwind successors are followed. A catch-all pad emitted by
// runFinalizerUnwindProtection ends in unreachable after rethrow, so a later
// candidate whose allocation can reach that pad is rejected. That is
// conservative: remaining finalizers in the same function then stay on the
// heap in EscapeAnalysisImpl (and in any later cj-pea run on the SCC).
// CatchSwitchInst / CatchReturnInst / CleanupReturnInst are not checked:
// Cangjie EH is invoke + landingpad (Itanium-style personality), not Windows
// funclet EH, so those terminators do not appear in cj-pea IR.
static bool hasNonReturnExitPath(BasicBlock *CreateBB) {
  SmallDenseSet<BasicBlock *> VisitedBB;
  SmallVector<BasicBlock *> Stack;
  Stack.push_back(CreateBB);
  while (!Stack.empty()) {
    BasicBlock *BB = Stack.pop_back_val();
    if (!VisitedBB.insert(BB).second)
      continue;
    Instruction *T = BB->getTerminator();
    if (isa<UnreachableInst>(T) || isa<ResumeInst>(T))
      return true;
    for (BasicBlock *Succ : successors(BB))
      if (VisitedBB.count(Succ) == 0)
        Stack.push_back(Succ);
  }
  return false;
}

// True if Alloc has constructed the object on every path to I and no
// destructor point in DestroyPoints dominates I. DestroyPoints are either
// the planned insert points (gate) or the inserted ~init calls (rewrite).
// DestroyRange is a range of Instruction* (insert points) or CallInst*
// (emitted dtors); both convert to Instruction* in the loop.
template <typename DestroyRange>
static bool finalizerLiveAt(const Instruction *Alloc, Instruction *I,
                            const DestroyRange &DestroyPoints,
                            DominatorTree &DT) {
  bool Constructed;
  if (auto *II = dyn_cast<InvokeInst>(Alloc))
    Constructed = DT.dominates(II->getNormalDest(), I->getParent());
  else
    Constructed = DT.dominates(Alloc, I);
  if (!Constructed)
    return false;
  for (Instruction *P : DestroyPoints)
    // dominates() is not reflexive for two instructions in one block.
    if (P == I || DT.dominates(P, I))
      return false;
  return true;
}

// Find one point that executes once per natural-loop iteration after every
// supported use. Values that cross a PHI, memory, aggregate, or loop boundary
// are deliberately rejected: PEA's escape result alone cannot prove where the
// finalizer must run for those flows.
static bool
collectLoopFinalizerInsertPoint(Instruction *NewFinalizer, BasicBlock *CreateBB,
                                LoopInfo &LI, DominatorTree &DT,
                                SmallVectorImpl<Instruction *> &Points) {
  Loop *L = LI.getLoopFor(CreateBB);
  if (!L)
    return false;

  BasicBlock *Latch = L->getLoopLatch();
  if (!Latch)
    return false;
  SmallVector<BasicBlock *, 4> ExitingBlocks;
  L->getExitingBlocks(ExitingBlocks);
  if (ExitingBlocks.size() != 1)
    return false;

  // Return has no CFG successor, so it is not reported by getExitingBlocks().
  // It would bypass the latch insertion point and skip destruction.
  for (BasicBlock *BB : L->blocks())
    if (isa<ReturnInst>(BB->getTerminator()))
      return false;

  // Restrict promotion to the canonical shape emitted for a for-loop: the
  // allocation and all of its modeled uses are in the latch. Inserting at the
  // latch terminator then preserves the order of the complete iteration.
  if (CreateBB != Latch)
    return false;

  SmallVector<Value *, 16> Worklist;
  SmallPtrSet<Value *, 16> VisitedValues;
  Worklist.push_back(NewFinalizer);
  while (!Worklist.empty()) {
    Value *V = Worklist.pop_back_val();
    if (!VisitedValues.insert(V).second)
      continue;
    for (User *U : V->users()) {
      Instruction *I = dyn_cast<Instruction>(U);
      if (!I || I->getParent() != Latch)
        return false;

      if (isa<BitCastInst>(I) || isa<AddrSpaceCastInst>(I) ||
          isa<GetElementPtrInst>(I)) {
        Worklist.push_back(I);
        continue;
      }

      if (auto *SI = dyn_cast<StoreInst>(I)) {
        if (SI->getPointerOperand() == V) {
          continue;
        }
        return false;
      }

      if (auto *Load = dyn_cast<LoadInst>(I)) {
        if (!isGCPointerType(Load->getType())) {
          continue;
        }
        return false;
      }

      if (auto *CB = dyn_cast<CallBase>(I)) {
        const Function *Callee = CB->getCalledFunction();
        if (Callee && Callee->getIntrinsicID() == Intrinsic::cj_gc_result &&
            CB->arg_size() == 1 && CB->getArgOperand(0) == V) {
          Worklist.push_back(CB);
          continue;
        }
        if (Callee && Callee->getIntrinsicID() == Intrinsic::cj_gcwrite_ref &&
            findMemoryBasePointer(CB->getArgOperand(1)) == NewFinalizer) {
          continue;
        }
        return false;
      }

      return false;
    }
  }

  Instruction *InsertPoint = Latch->getTerminator();
  if (!DT.dominates(NewFinalizer, InsertPoint))
    return false;
  Points.push_back(InsertPoint);
  return true;
}

static void collectFinalizerInsertPointsImpl(
    Instruction *NewFinalizer, BasicBlock *CreateBB, LoopInfo *LI,
    DominatorTree *DT, SmallVectorImpl<Instruction *> &Points) {
  Instruction *Term = CreateBB->getTerminator();
  if (isa<ReturnInst>(Term)) {
    // musttail is glued to ret, so ~init cannot be inserted there. Leave
    // Points empty (Cangjie codegen does not emit musttail).
    if (DT != nullptr && DT->dominates(NewFinalizer, Term) &&
        Term->getParent()->getTerminatingMustTailCall() == nullptr)
      Points.push_back(Term);
    return;
  }

  if (LI && LI->getLoopFor(CreateBB)) {
    if (DT != nullptr)
      collectLoopFinalizerInsertPoint(NewFinalizer, CreateBB, *LI, *DT, Points);
    return;
  }

  SmallSet<BasicBlock *, 16> VisitedBB;
  SmallVector<BasicBlock *, 8> Stack;
  Stack.push_back(CreateBB);
  // A return that cannot receive ~init (not dominated, or terminating
  // musttail) makes the set incomplete; drop every point.
  bool HasUnusableExit = false;
  while (!Stack.empty()) {
    BasicBlock *BB = Stack.pop_back_val();
    if (!VisitedBB.insert(BB).second)
      continue;
    Instruction *T = BB->getTerminator();
    if (isa<ReturnInst>(T)) {
      if (DT == nullptr || !DT->dominates(NewFinalizer, T) ||
          T->getParent()->getTerminatingMustTailCall()) {
        HasUnusableExit = true;
      } else {
        Points.push_back(T);
      }
      continue;
    }
    for (BasicBlock *Succ : successors(BB))
      if (VisitedBB.count(Succ) == 0)
        Stack.push_back(Succ);
  }
  if (HasUnusableExit)
    Points.clear();
}

// Check whether at least one valid insertion point exists for the explicit
// ~init call of a stack-allocated finalizer.
static bool hasFinalizerInsertPoints(Instruction *NewFinalizer,
                                     BasicBlock *CreateBB, LoopInfo *LI,
                                     DominatorTree *DT) {
  SmallVector<Instruction *, 4> Points;
  collectFinalizerInsertPointsImpl(NewFinalizer, CreateBB, LI, DT, Points);
  return !Points.empty();
}

static bool canStackAllocateFinalizer(CallBase *CB, Module *M, LoopInfo *LI) {
  if (!CJEASupportFinalizer)
    return false;
  // landingpad (and therefore invoke unwind) requires a personality fn.
  // Cangjie functions with bodies always have one: BuildCJFunc sets
  // __cj_personality_v0$. Reject CFFI wrappers / declarations here rather
  // than scanning live may-throw calls that cannot be converted.
  if (!CB->getFunction()->hasPersonalityFn())
    return false;
  GlobalVariable *Klass = getNewKlass(CB);
  if (Klass == nullptr)
    return false;
  Function *FinalizerMethod = getFinalizerMethod(Klass, M);
  if (FinalizerMethod == nullptr)
    return false;
  // ~init is not required to be nounwind. The language spec ("class 终结器")
  // makes an uncaught exception escaping a finalizer undefined behavior, so
  // surfacing it at the insert point is a legal, deterministic realization
  // of that UB. A may-throw plain call between the allocation and ~init
  // unwinds on an edge the CFG does not model (issue #204);
  // runFinalizerUnwindProtection converts those calls to invokes.
  BasicBlock *BB = CB->getParent();
  DominatorTree DT(*BB->getParent());
  if (hasNonReturnExitPath(BB))
    return false;
  if (!hasFinalizerInsertPoints(CB, BB, LI, &DT))
    return false;
  return true;
}

class NonEscapeRewriter : public InstVisitor<NonEscapeRewriter> {
  friend class InstVisitor<NonEscapeRewriter>;

public:
  explicit NonEscapeRewriter(CallGraphUpdater *CGUpdater, Module *M,
                             LoopInfo *LI = nullptr)
      : CGUpdater(CGUpdater), M(M), LI(LI) {
    CJMemset = Intrinsic::getDeclaration(
        M, Intrinsic::cj_memset,
        {IntegerType::getInt8PtrTy(M->getContext(), 0)});
  }

  void setLoopInfo(LoopInfo *LoopInfo) { LI = LoopInfo; }

  bool prepareFinalizer(SmallVectorImpl<Instruction *> &InsertPoints) {
    CallBase *CB = dyn_cast<CallBase>(OldInst);
    if (!CB || !CB->getCalledFunction() ||
        !isCangjieNewFinalizerFunc(CB->getCalledFunction()))
      return true;
    if (!CJEASupportFinalizer)
      return false;
    if (!CB->getFunction()->hasPersonalityFn())
      return false;
    IsFinalizer = true;
    FinalizerKlass = getNewKlass(CB);
    FinalizerMethod = getFinalizerMethod(FinalizerKlass, M);
    FinalizerInsertBB = CB->getParent();
    if (FinalizerMethod == nullptr)
      return false;
    if (hasNonReturnExitPath(FinalizerInsertBB))
      return false;
    collectFinalizerInsertPoints(InsertPoints);
    if (InsertPoints.empty())
      return false;
    return true;
  }

  bool rewrite(Instruction *Old, BasicBlock *NewInstInsertBB) {
    OldInst = Old;
    Visited.clear();
    IsFinalizer = false;
    FinalizerMethod = nullptr;
    FinalizerInsertBB = nullptr;
    FinalizerKlass = nullptr;
    SmallVector<Instruction *, 4> InsertPoints;
    if (!prepareFinalizer(InsertPoints)) {
      return false;
    }
    if (!createNewAllocaInst(NewInstInsertBB)) {
      return false;
    }
    rewriteNewInst();
    if (IsFinalizer && FinalizerInsertBB != nullptr) {
      insertFinalizerCall(InsertPoints);
    }
    return true;
  }

  void collectFinalizerInsertPoints(SmallVectorImpl<Instruction *> &Points) {
    DominatorTree DT(*FinalizerInsertBB->getParent());
    collectFinalizerInsertPointsImpl(OldInst, FinalizerInsertBB, LI, &DT,
                                     Points);
  }

  // A finalizer object stack-promoted in this run, awaiting function-level
  // unwind protection (runFinalizerUnwindProtection).
  struct PromotedFinalizer {
    AllocaInst *Alloca;
    GlobalVariable *Klass;
    Function *Method;
    CallBase *AllocCall;
    // Used only while collectFinalizerHazardSites runs. A listed CallInst may
    // itself be converted to invoke (changeToInvokeAndSplitBasicBlock deletes
    // it); do not dereference DtorCalls after convertFinalizerHazards.
    SmallVector<CallInst *> DtorCalls;
  };

  CallInst *emitFinalizerCall(IRBuilder<> &Builder, Instruction *Alloca,
                              GlobalVariable *FinalizerKlass,
                              Function *FinalizerMethod) {
    Type *PI8AS1Ty = Type::getInt8PtrTy(M->getContext(), 1);
    Value *ObjPtr = Builder.CreateAddrSpaceCast(Alloca, PI8AS1Ty);
    SmallVector<Value *> Args;
    Args.push_back(ObjPtr);
    // ~init is either (this) or (this, TypeInfo*). Pass this class's TypeInfo
    // when the lowered function expects it.
    if (FinalizerMethod->getFunctionType()->getNumParams() > 1) {
      Value *OuterTI = Builder.CreateBitCast(
          FinalizerKlass, FinalizerMethod->getFunctionType()->getParamType(1));
      Args.push_back(OuterTI);
    }
    return Builder.CreateCall(FinalizerMethod, Args);
  }

  void insertFinalizerCall(SmallVectorImpl<Instruction *> &InsertPoints) {
    PromotedFinalizer PF;
    PF.Alloca = NewInst;
    PF.Klass = FinalizerKlass;
    PF.Method = FinalizerMethod;
    PF.AllocCall = cast<CallBase>(OldInst);
    for (Instruction *InsertPoint : InsertPoints) {
      // Several objects may share one insert point: destruct in reverse
      // rewrite order of this rewriter (not program allocation order),
      // matching the unwind pads.
      CallInst *&FirstDtor = FirstDtorCallAtPoint[InsertPoint];
      IRBuilder<> Builder(FirstDtor != nullptr ? FirstDtor : InsertPoint);
      Builder.SetCurrentDebugLocation(InsertPoint->getDebugLoc());
      CallInst *Dtor =
          emitFinalizerCall(Builder, NewInst, FinalizerKlass, FinalizerMethod);
      PF.DtorCalls.push_back(Dtor);
      FirstDtor = Dtor;
    }
    PromotedFinalizers.push_back(std::move(PF));
    if (CGUpdater && !InsertPoints.empty())
      CGUpdater->reanalyzeFunction(*FinalizerInsertBB->getParent());
  }

  using HazardSite = std::pair<CallInst *, SmallVector<PromotedFinalizer *>>;

  void collectFinalizerHazardSites(
      Function *F, const SmallVectorImpl<PromotedFinalizer *> &Promoted,
      DominatorTree &DT, SmallVectorImpl<HazardSite> &Sites) {
    SmallDenseSet<Instruction *> DeadAllocs;
    for (PromotedFinalizer *PF : Promoted)
      DeadAllocs.insert(PF->AllocCall);
    for (BasicBlock &BB : *F) {
      for (Instruction &I : BB) {
        auto *CI = dyn_cast<CallInst>(&I);
        if (CI == nullptr || !CI->mayThrow() || DeadAllocs.count(CI))
          continue;
        SmallVector<PromotedFinalizer *> Live;
        for (PromotedFinalizer *PF : Promoted) {
          if (finalizerLiveAt(PF->AllocCall, CI, PF->DtorCalls, DT))
            Live.push_back(PF);
        }
        if (Live.empty())
          continue;
        Sites.push_back({CI, std::move(Live)});
      }
    }
  }

  static bool sameLiveSet(const SmallVectorImpl<PromotedFinalizer *> &A,
                          const SmallVectorImpl<PromotedFinalizer *> &B) {
    return A.size() == B.size() && std::equal(A.begin(), A.end(), B.begin());
  }

  BasicBlock *
  createFinalizerUnwindPad(Function *F,
                           const SmallVectorImpl<PromotedFinalizer *> &Live,
                           DebugLoc DL, Function *GetWrapper,
                           Function *PostThrow, Function *ThrowEx) {
    LLVMContext &Ctx = M->getContext();
    BasicBlock *Pad = BasicBlock::Create(Ctx, "cj.finalizer.unwind", F);
    IRBuilder<> Builder(Pad);
    // 1 = catch-all clause.
    LandingPadInst *LP =
        Builder.CreateLandingPad(Type::getTokenTy(Ctx), 1, "finalizer.lp");
    LP->addClause(
        ConstantPointerNull::get(cast<PointerType>(Type::getInt8PtrTy(Ctx))));
    Builder.SetCurrentDebugLocation(DL);
    Value *Wrapper = Builder.CreateCall(GetWrapper);
    Value *Ex = Builder.CreateCall(PostThrow, {Wrapper});
    for (PromotedFinalizer *PF : reverse(Live))
      emitFinalizerCall(Builder, PF->Alloca, PF->Klass, PF->Method);
    Builder.CreateCall(ThrowEx, {Ex});
    Builder.CreateUnreachable();
    return Pad;
  }

  void convertFinalizerHazards(Function *F,
                               SmallVectorImpl<HazardSite> &Sites) {
    LLVMContext &Ctx = M->getContext();
    Type *PI8Ty = Type::getInt8PtrTy(Ctx);
    Type *PI8AS1Ty = Type::getInt8PtrTy(Ctx, 1);
    Function *GetWrapper = M->declareCJRuntimeFunc(
        "CJ_MCC_GetExceptionWrapper", FunctionType::get(PI8Ty, {}), true);
    GetWrapper->addFnAttr(Attribute::NoUnwind);
    Function *PostThrow = M->declareCJRuntimeFunc(
        "CJ_MCC_PostThrowException",
        FunctionType::get(PI8AS1Ty, {PI8Ty}, false), true);
    PostThrow->addFnAttr(Attribute::NoUnwind);
    Function *ThrowEx = M->declareCJRuntimeFunc(
        "CJ_MCC_ThrowException",
        FunctionType::get(Type::getVoidTy(Ctx), {PI8AS1Ty}, false), true);
    ThrowEx->setDoesNotReturn();

    SmallVector<std::pair<BasicBlock *, SmallVector<PromotedFinalizer *>>> Pads;
    auto FindPad = [&](const SmallVectorImpl<PromotedFinalizer *> &Live) {
      for (auto &Existing : Pads)
        if (sameLiveSet(Existing.second, Live))
          return Existing.first;
      return static_cast<BasicBlock *>(nullptr);
    };
    for (auto &Site : Sites) {
      BasicBlock *Pad = FindPad(Site.second);
      if (Pad == nullptr) {
        Pad =
            createFinalizerUnwindPad(F, Site.second, Site.first->getDebugLoc(),
                                     GetWrapper, PostThrow, ThrowEx);
        Pads.push_back({Pad, Site.second});
      }
      CallInst *CI = Site.first;
      CI->setTailCallKind(CallInst::TCK_None);
      // Deletes CI (may be a ~init listed in PromotedFinalizer::DtorCalls).
      changeToInvokeAndSplitBasicBlock(CI, Pad);
    }
  }

  // Convert every may-throw plain call that can execute while a promoted
  // finalizer is live into an invoke unwinding to a catch-all pad that runs
  // pending ~init calls and rethrows (issue #204).
  bool runFinalizerUnwindProtection() {
    if (PromotedFinalizers.empty())
      return false;
    MapVector<Function *, SmallVector<PromotedFinalizer *>> ByFunc;
    for (PromotedFinalizer &PF : PromotedFinalizers)
      ByFunc[PF.AllocCall->getFunction()].push_back(&PF);

    bool Changed = false;
    for (auto &Entry : ByFunc) {
      Function *F = Entry.first;
      DominatorTree DT(*F);
      SmallVector<HazardSite> Sites;
      collectFinalizerHazardSites(F, Entry.second, DT, Sites);
      if (Sites.empty())
        continue;
      convertFinalizerHazards(F, Sites);
      Changed = true;
      if (CGUpdater)
        CGUpdater->reanalyzeFunction(*F);
    }
    for (Instruction *I : DeferredFinalizerAllocs) {
      if (auto *II = dyn_cast<InvokeInst>(I)) {
        IRBuilder<> BrIRB(II);
        BrIRB.CreateBr(II->getNormalDest());
        II->getUnwindDest()->removePredecessor(II->getParent());
      }
      I->eraseFromParent();
    }
    DeferredFinalizerAllocs.clear();
    return Changed;
  }

  bool handleStructAS1Cast(AllocaInst *Old) {
    NewInst = Old;
    rewriteNewInst();
    return true;
  }

  void setStructAS1Flag() {
    StructAS1Replace = true;
  }

  static void clearRewritePhiIncoming() {
    RewritePhiIncoming.clear();
  }

private:
  void eraseInstr(Instruction *I) {
    Visited.remove(I);
    I->eraseFromParent();
  }
  void enqueueUsers(Instruction &I) {
    for (Use &U : I.uses()) {
      if (Visited.insert(U.getUser())) {
        Queue.push_back(dyn_cast<Instruction>(U.getUser()));
      }
    }
  }

  void rewriteNewInst() {
    enqueueUsers(*NewInst);
    LLVM_DEBUG(dbgs() << "begin rewrite Inst:" << *NewInst << "\n");
    while (!Queue.empty()) {
      Instruction *I = Queue.pop_back_val();
      visit(I);
    }
  }

  void visitInstruction(Instruction &I) {
    LLVM_DEBUG(dbgs() << "rewrite path:" << I << "\n");
  }

  void visitPHINode(PHINode &PN) {
    if (RewritePhiIncoming[&PN].insert(NewInst)) {
      Visited.remove(&PN);
    }
    if (RewritePhiIncoming[&PN].size() == PN.getNumIncomingValues()) {
      enqueueUsers(PN);
    } else {
      removeInvariantLoad(&PN);
    }
  }

  void visitSelectInst(SelectInst &SI) {
    RewriteSIIncoming[&SI].insert(NewInst);
    // 2: select inst incoming size.
    if (RewriteSIIncoming[&SI].size() == 2) {
      enqueueUsers(SI);
    } else {
      removeInvariantLoad(&SI);
    }
  }

  void visitAddrSpaceCastInst(AddrSpaceCastInst &ASC) { enqueueUsers(ASC); }

  void visitBitCastInst(BitCastInst &BC) { enqueueUsers(BC); }

  void visitGetElementPtrInst(GetElementPtrInst &GEPI) { enqueueUsers(GEPI); }

  void visitLoadInst(LoadInst &LI) {
    removeInvariantLoad(&LI);
    if (!isGCPointerType(LI.getOperand(0)->getType())) {
      return;
    }
    Type *StackObjPtr = LI.getType()->getPointerTo(0);
    IRBuilder<> Builder(&LI);
    Value *AS1ToAS0 =
        Builder.CreateAddrSpaceCast(LI.getOperand(0), StackObjPtr);
    Instruction *Load =
        new LoadInst(LI.getType(), AS1ToAS0, "", false, LI.getAlign(), &LI);
    LI.replaceAllUsesWith(Load);
    Load->setDebugLoc(LI.getDebugLoc());
    eraseInstr(&LI);
  }

  // Specifically for typeinfo load, since store typeinfo and new object are no
  // longer "atomic", it is possible for load to occur before store.
  void removeInvariantLoad(Instruction *I) {
    if (!(isa<PHINode>(I) || isa<LoadInst>(I) || isa<SelectInst>(I)))
      report_fatal_error("error instruction");
    if (isa<LoadInst>(I)) {
      I->setMetadata(LLVMContext::MD_invariant_load, nullptr);
      return;
    }
    SmallVector<User *, 4> Worklist = {I->user_begin(), I->user_end()};
    SetVector<User *> Visited;
    while (!Worklist.empty()) {
      User *U = Worklist.pop_back_val();
      if (!Visited.insert(U))
        continue;
      Instruction *UI = cast<Instruction>(U);
      switch (UI->getOpcode()) {
      default:
        break;
      case Instruction::AddrSpaceCast:
      case Instruction::BitCast:
      case Instruction::PHI:
      case Instruction::Select:
      case Instruction::IntToPtr:
      case Instruction::PtrToInt:
      case Instruction::GetElementPtr:
        Worklist.append(U->user_begin(), U->user_end());
        break;
      case Instruction::Load:
        if (UI->hasMetadata(LLVMContext::MD_invariant_load))
          UI->setMetadata(LLVMContext::MD_invariant_load, nullptr);
        break;
      }
    }
  }

  bool canReplaceBarrierCall(CallInst &CI, unsigned Arg) {
    if (StructAS1Replace)
      return true;
    Value *Val = findMemoryBasePointer(CI.getArgOperand(Arg));
    if (PHINode *PN = dyn_cast<PHINode>(Val)) {
      return RewritePhiIncoming.count(PN) &&
             (RewritePhiIncoming[PN].size() == PN->getNumIncomingValues());
    } else if (SelectInst *SI = dyn_cast<SelectInst>(Val)) {
      // 2: select inst incoming size
      return RewriteSIIncoming.count(SI) && (RewriteSIIncoming[SI].size() == 2);
    }
    return Val == NewInst;
  }

  bool isArrayCopyAllAlloca(CallInst &CI) {
    if (RewriteArrayCopy[&CI].insert(NewInst)) {
      Visited.remove(&CI);
    }
    // 2: all the ptr operand of array copy is not escape
    if (RewriteArrayCopy[&CI].size() == 2) {
      return false;
    }
    return true;
  }

  void visitCallInst(CallInst &CI) {
    if (CI.isTailCall())
      CI.setTailCall(false);

    if (const Function *F = CI.getCalledFunction()) {
      Instruction *NewInst = nullptr;
      Value *AS1ToAS0 = nullptr;
      Type *StackObjPtr = nullptr;
      IRBuilder<> Builder(&CI);
      switch (F->getIntrinsicID()) {
      default:
        return;
      case Intrinsic::cj_gcwrite_ref:
        if (!canReplaceBarrierCall(CI, 1)) {
          return;
        }
        StackObjPtr = CI.getArgOperand(0)->getType()->getPointerTo(0);
        // 2: value operands number
        AS1ToAS0 =
            Builder.CreateAddrSpaceCast(CI.getArgOperand(2), StackObjPtr);
        NewInst =
            new StoreInst(CI.getArgOperand(0),
                          AS1ToAS0, false, Align(alignEight), &CI);
        break;
      case Intrinsic::cj_gcread_ref:
        if (!canReplaceBarrierCall(CI, 0)) {
          return;
        }
        StackObjPtr = CI.getType()->getPointerTo(0);
        // 2: value operands number
        AS1ToAS0 =
            Builder.CreateAddrSpaceCast(CI.getArgOperand(1), StackObjPtr);
        NewInst = new LoadInst(CI.getType(), AS1ToAS0, "", &CI);
        break;
      case Intrinsic::cj_gcwrite_struct:
        if (!canReplaceBarrierCall(CI, 0)) {
          return;
        }
        // 1: pointer operands number
        // 2: value operands number
        // 3: size info
        NewInst = Builder.CreateMemCpy(CI.getArgOperand(1), Align(alignEight),
                                       CI.getArgOperand(2), Align(alignEight),
                                       CI.getArgOperand(3));
        break;
      case Intrinsic::cj_gcread_struct:
        if (!canReplaceBarrierCall(CI, 1)) {
          return;
        }
        // 0: pointer operands number
        // 2: value operands number
        // 3: size info
        NewInst = Builder.CreateMemCpy(CI.getArgOperand(0), Align(alignEight),
                                       CI.getArgOperand(2), Align(alignEight),
                                       CI.getArgOperand(3));
        break;
      case Intrinsic::cj_array_copy_ref:
      case Intrinsic::cj_array_copy_struct:
        Type *BaseType = findMemoryBasePointer(CI.getArgOperand(0))->getType();
        if (isArrayCopyAllAlloca(CI) ||
            (StructAS1Replace && isGCPointerType(BaseType)) ||
            !canReplaceBarrierCall(CI, 0)) {
          return;
        }
        NewInst = Builder.CreateMemMove(CI.getArgOperand(1), Align(alignEight),
                                        CI.getArgOperand(3), Align(alignEight),
                                        CI.getArgOperand(4));
        break;
      }
      CI.replaceAllUsesWith(NewInst);
      NewInst->setDebugLoc(CI.getDebugLoc());
      eraseInstr(&CI);
    }
  }

  Type *getObjAllocaType(Type *LayoutType) {
    SmallVector<Type *> Elems;
    auto *KlassType = StructType::getTypeByName(M->getContext(), "TypeInfo");
    Elems.push_back(KlassType->getPointerTo());
    Elems.push_back(LayoutType);
    return StructType::get(M->getContext(), Elems);
  }

  void createAllocaInst(IRBuilder<> &IRB, bool IsArray, CallBase *I) {
    BasicBlock *EntryBB = &I->getParent()->getParent()->getEntryBlock();
    Instruction *InsertPos = &*EntryBB->getFirstInsertionPt();
    const DebugLoc &CurDbg = OldInst->getDebugLoc();
    NewInst = new AllocaInst(getObjAllocaType(AllocaType), 0, "", InsertPos);
    uint32_t MemSetSize = AllocaSize - ObjectHeadSize;
    if (IsArray) {
      SmallVector<Value *, 2> GEPIndices;
      GEPIndices.push_back(IRB.getInt32(0));
      GEPIndices.push_back(IRB.getInt32(1));
      auto SizeP = IRB.CreateInBoundsGEP(
          NewInst->getType()->getNonOpaquePointerElementType(), NewInst,
          GEPIndices);
      SmallVector<Value *, 2> GEPSizeIndices;
      GEPSizeIndices.push_back(IRB.getInt32(0));
      GEPSizeIndices.push_back(IRB.getInt32(0));
      GEPSizeIndices.push_back(IRB.getInt32(0));
      auto SizeP64 = IRB.CreateInBoundsGEP(
          SizeP->getType()->getNonOpaquePointerElementType(), SizeP,
          GEPSizeIndices);
      IRB.CreateStore(I->getOperand(1), SizeP64); // store array size
      MemSetSize = AllocaSize - ArrayHeadSize;
    }
    NewInst->setDebugLoc(CurDbg);
    setEscapeMeta(NewInst);
    Type *PI8Ty = Type::getInt8PtrTy(I->getContext());
    Value *PI = IRB.CreateBitCast(NewInst, PI8Ty->getPointerTo(0));
    IRB.CreateStore(I->getOperand(0), PI);
    if (MemSetSize == 0) {
      return;
    }
    SmallVector<Value *, 1> GEPValues;
    // 8: PI Type Size
    GEPValues.push_back(IRB.getInt32((AllocaSize - MemSetSize) / 8));
    auto Values = IRB.CreateGEP(PI8Ty, PI, GEPValues);
    CallInst *CI = IRB.CreateMemSet(
        Values, IRB.getInt8(0), IRB.getInt64(MemSetSize), MaybeAlign(0), false);
    if (UseCJMemset) {
      CI->setCalledFunction(CJMemset);
    }
    addLoopIterationMemsetForHoistedArray(I, CI);
  }

  void addLoopIterationMemsetForHoistedArray(CallBase *I, CallInst *MemsetCI) {
    IRBuilder<> IRB(I);
    if (I && I->getCalledFunction()) {
      Function *F = I->getCalledFunction();
      auto FuncName = F->getName();
      if (FuncName.startswith("CJ_MCC_NewArray")) {
        CallInst *NewMemset = cast<CallInst>(MemsetCI->clone());
        NewMemset->insertBefore(I);
      }
    }
  }

  void replaceOldInst(IRBuilder<> &IRB, CallBase *I) {
    // replace old inst
    // collect origin used point, replace them with addrspace 0
    // maybe change record type.
    Type *PI8AS1Ty = Type::getInt8PtrTy(I->getContext(), 1);
    Value *BI = IRB.CreateAddrSpaceCast(NewInst, PI8AS1Ty);
    OldInst->replaceAllUsesWith(BI);
    if (IsFinalizer) {
      // Keep the dead allocation as the dominance anchor for the
      // unwind-protection live-set scan; erase after that scan.
      DeferredFinalizerAllocs.push_back(OldInst);
      return;
    }
    if (auto II = dyn_cast<InvokeInst>(OldInst)) {
      IRBuilder BrIRB(OldInst);
      BrIRB.CreateBr(II->getNormalDest());
      BasicBlock *UnwindBB = II->getUnwindDest();
      UnwindBB->removePredecessor(OldInst->getParent());
    }
    if (OldInst->use_empty()) {
      eraseInstr(OldInst);
    }
  }

  bool createNewAllocaInst(BasicBlock *NewInstInsertBB) {
    CallBase *I = dyn_cast<CallBase>(OldInst);
    Function *Callee = I->getCalledFunction();
    bool IsArray = !Callee->getName().isCangjieNewObjFunction();
    GlobalVariable *Klass = getNewKlass(I);
    bool NonMoveArray = false;
    // cache, now duplicate.
    AllocaType = getAllocaType(Klass, UseCJMemset, AllocaSize, NonMoveArray, I);
    if (NonMoveArray) {
      return false;
    }
    Instruction *InsertInst = OldInst;
    if (NewInstInsertBB) {
      InsertInst = &*NewInstInsertBB->getTerminator();
    }
    IRBuilder<> IRB(InsertInst);
    // do replace so update CallGraph
    if (CGUpdater)
      CGUpdater->removeCallSite(*I);
    createAllocaInst(IRB, IsArray, I);
    replaceOldInst(IRB, I);
    return true;
  }

  Type *AllocaType;
  Instruction *OldInst;
  AllocaInst *NewInst;
  bool UseCJMemset = false;
  uint32_t AllocaSize = 0;
  CallGraphUpdater *CGUpdater;
  Module *M;
  Function *CJMemset;
  SmallVector<Instruction *, 8> Queue;
  SmallSetVector<User *, 8> Visited;
  bool IsFinalizer = false;
  Function *FinalizerMethod = nullptr;
  BasicBlock *FinalizerInsertBB = nullptr;
  GlobalVariable *FinalizerKlass = nullptr;
  SmallVector<PromotedFinalizer> PromotedFinalizers;
  SmallVector<Instruction *> DeferredFinalizerAllocs;
  DenseMap<Instruction *, CallInst *> FirstDtorCallAtPoint;
  bool StructAS1Replace = false;
  LoopInfo *LI = nullptr;
  static DenseMap<PHINode *, SmallSetVector<Instruction *, 8>> RewritePhiIncoming;
  DenseMap<SelectInst *, SmallSetVector<Instruction *, 8>> RewriteSIIncoming;
  DenseMap<CallInst *, SmallSetVector<Instruction *, 8>> RewriteArrayCopy;
};
} // end namespace llvm

DenseMap<PHINode *, SmallSetVector<Instruction *, 8>>
    NonEscapeRewriter::RewritePhiIncoming;

namespace {
enum EscapeType : unsigned {
  EscapeOrigin = 0x1 << 0,
  InfoEscapedParam = 0x1 << 1,
  InfoEscapedDirect = 0x1 << 2,
  EscapeParam = 0x1 << 3,
  EscapeGlobal = 0x1 << 4,
  EscapeRet = 0x1 << 5,
  EscapeLoop = 0x1 << 6
};

enum LocationType : unsigned { NewLoc = 0x1, ParamLoc = 0x2, TempLoc = 0x3 };

// each as1 ptr has a location, which contains escape state
struct ObjectLocation {
  LocationType LocType;
  Value *Val;
  Function *Parent;
  unsigned LoopDepth;
  int EscapeDepth = INT32_MAX;
  SetVector<ObjectLocation *> EdgesSet;
  SmallDenseMap<ObjectLocation *, int> EdgesMap;
  bool Escaped = false;
  bool Visited = false;
  uint32_t AllocaSize = 0;
  Type *AllocaType = nullptr;
  Value *EscapedInst = nullptr;
  unsigned EscapeState = 0;
  // escape param locs
  SetVector<ObjectLocation *> EscapeLoc;
  SetVector<ObjectLocation *> ZeroPathEscapeLoc;
  // info may escape param locs
  SetVector<ObjectLocation *> InfoEscapeLoc;
  ObjectLocation() = default;
  virtual ~ObjectLocation() = default;
  ObjectLocation(Value *V, Function *F, unsigned LoopDepth)
      : ObjectLocation(TempLoc, V, F, LoopDepth) {}

public:
  // compute escape info between this and Src locations.
  // for Path Value
  // 0 : info copy -1: Src load from this 1: this load from Src
  // for Return value
  // 0 : nothing changed 1: Src's state changed -1: this's State Changed
  virtual int EscapeEdge(ObjectLocation &Src, int Path) {
    assert(this->Escaped);
    if (Src.Escaped) {
      return 0;
    }
    return Src.setInfoEscaped(Path);
  }

  void insertEdge(ObjectLocation *Loc, int Path) {
    if (Loc == this) {
      return;
    }

    if (EdgesSet.contains(Loc)) {
      assert(EdgesMap.count(Loc) && "The EdgesMap and EdgesSet do not match.");
      if (EdgesMap[Loc] > Path) {
        EdgesMap[Loc] = Path;
      }
    } else {
      if (Path <= 0 && LoopDepth > Loc->LoopDepth) {
        Escaped = true;
      }
      EdgesSet.insert(Loc);
      EdgesMap[Loc] = Path;
    }
  }

  void removeEdge(ObjectLocation *Loc) {
    if (Loc == this) {
      return;
    }
    if (EdgesSet.contains(Loc)) {
      EdgesSet.remove(Loc);
      EdgesMap.erase(Loc);
    }
  }

  void clearEdges() {
    EdgesSet.clear();
    EdgesMap.clear();
  }

  bool isInfoEscaped() const {
    return (EscapeState & InfoEscapedDirect) != 0 ||
           (EscapeState & InfoEscapedParam) != 0;
  }

  // Info escaped location has an EscapeDepth, 0 means value load from this
  // location's value has been escaped. EscapedDepth is 0 when value load from
  // the value's EscapeDepth is 1.
  int setInfoEscaped(int i) {
    if (i < 0) {
      if (!Escaped) {
        Escaped = true;
        return 1;
      }
      return 0;
    }
    if (EscapeDepth < i || (EscapeDepth == i &&
                            (EscapeState & InfoEscapedDirect))) {
      return 0;
    }
    EscapeState |= InfoEscapedDirect;
    InfoEscapeLoc.clear();
    EscapeDepth = i;
    return 1;
  }

  virtual bool isParam() { return false; }

  virtual bool isNew() const { return false; }

  bool isTemp() { return !isParam() && !isNew(); }

  bool isEscaped() const {
    return Escaped || (EscapeState & (EscapeParam | EscapeGlobal)) != 0;
  }

  bool isEscapeParam() const { return EscapeParam & EscapeState; }

  bool setEscapeParam(const ObjectLocation &Loc) {
    EscapeState |= EscapeParam;
    unsigned OldSize = EscapeLoc.size();
    for (ObjectLocation *Param : Loc.EscapeLoc) {
      EscapeLoc.insert(Param);
    }
    return OldSize != EscapeLoc.size();
  }

  bool setInfoEscapeParam(SetVector<ObjectLocation *> &LocVec,
                          int InfoEscapeDepth) {
    if (InfoEscapeDepth < 0) {
      if (!Escaped) {
        if (isParam() && LocVec.size() == 1 && LocVec[0]->Val == Val) {
          return false;
        }
        Escaped = true;
        InfoEscapeLoc.clear();
        for (auto Param : LocVec) {
          InfoEscapeLoc.insert(Param);
        }
        return true;
      } else if (InfoEscapeLoc.size() != 0) {
        for (auto Param : LocVec) {
          InfoEscapeLoc.insert(Param);
        }
      }
      return false;
    }
    if (EscapeDepth < InfoEscapeDepth) {
      return 0;
    }
    EscapeState |= InfoEscapedParam;
    if (EscapeDepth > InfoEscapeDepth) {
      EscapeState &= ~InfoEscapedDirect;
      EscapeDepth = InfoEscapeDepth;
      InfoEscapeLoc.clear();
      for (auto Param : LocVec) {
        InfoEscapeLoc.insert(Param);
      }
      return true;
    }
    unsigned OldSize = InfoEscapeLoc.size();
    for (auto Param : LocVec) {
      InfoEscapeLoc.insert(Param);
    }
    return OldSize != InfoEscapeLoc.size();
  }

  void passEdges(bool ShouldClear) {
    struct EdgeInfo {
      ObjectLocation *Loc;
      int Path;
      int PathFromThis;
      int PathMax;
      int PathMin;
    };
    const size_t EdgeCount = EdgesSet.size();
    SmallVector<EdgeInfo, 8> EdgeInfos;
    EdgeInfos.reserve(EdgeCount);
    for (unsigned i = 0; i < EdgeCount; i++) {
      ObjectLocation *IL = EdgesSet[i];
      assert(EdgesMap.count(IL) && "The EdgesMap and EdgesSet do not match.");
      if (EscapeDepth != INT32_MAX)
        IL->setInfoEscaped(EscapeDepth + EdgesMap[IL]);
      int ILPath = EdgesMap[IL];
      int ILPathFromThis = IL->EdgesMap[this];
      int ILPathMax = std::max(ILPath, -ILPathFromThis);
      int ILPathMin = std::min(ILPath, -ILPathFromThis);
      EdgeInfos.push_back({IL, ILPath, ILPathFromThis, ILPathMax, ILPathMin});
    }

    for (unsigned i = 0; i < EdgeCount; i++) {
      EdgeInfo &ILInfo = EdgeInfos[i];
      ObjectLocation *IL = ILInfo.Loc;
      for (unsigned j = i + 1; j < EdgeCount; j++) {
        EdgeInfo &JLInfo = EdgeInfos[j];
        ObjectLocation *JL = JLInfo.Loc;
        int I2J = JLInfo.PathMin - ILInfo.PathMax;
        int J2I = ILInfo.PathMin - JLInfo.PathMax;
        IL->insertEdge(JL, I2J);
        JL->insertEdge(IL, J2I);
      }
      if (ShouldClear)
        IL->removeEdge(this);
    }
    if (ShouldClear)
      clearEdges();
  }

protected:
  ObjectLocation(LocationType LT, Value *V, Function *F, unsigned LoopDepth = 0)
      : LocType(LT), Val(V), Parent(F), LoopDepth(LoopDepth) {}
};

struct NewLocation : ObjectLocation {
public:
  bool HasRefs = false;
  bool NonMoveArray = false;
  BasicBlock *NewInstInsertBB = nullptr;

  NewLocation(Value *V, Function *F, unsigned LoopDepth, BasicBlock *PreHeader)
      : ObjectLocation(NewLoc, V, F, LoopDepth) {
    CallBase *I = dyn_cast<CallBase>(V);
    GlobalVariable *Klass = getNewKlass(I);
    if (Klass == nullptr) {
      Escaped = true;
      return;
    }
    AllocaType = getAllocaType(Klass, HasRefs, AllocaSize, NonMoveArray,
                               dyn_cast<Instruction>(Val));
    if (AllocaType == nullptr && !NonMoveArray) { // fail to create alloca type
      Escaped = true;
    } else {
      NewInstInsertBB = PreHeader;
    }
  }

  bool isNew() const override { return true; }

  int EscapeEdge(ObjectLocation &Src, int Path) override {
    if (isEscaped() && Src.isEscaped()) {
      return 0;
    }
    if (isEscaped()) {
      return Src.setInfoEscapeParam(InfoEscapeLoc, Path - 1);
    }
    if (Src.isEscaped()) {
      return -setInfoEscapeParam(Src.InfoEscapeLoc, -1 - Path);
    }
    if (Src.isParam()) {
      SetVector<ObjectLocation *> TmpLocs;
      TmpLocs.insert(&Src);
      int IsRet = (Src.Val == nullptr) ? 1 : 0;
      // if a new obj is path 0 to args, it not escape. if path 0 to
      // ret value, it escaped.
      if (setInfoEscapeParam(TmpLocs, -Path - IsRet))
        return -1;
    }
    if (EscapeDepth != INT32_MAX) {
      if (Src.EscapeDepth > EscapeDepth + Path) {
        return Src.setInfoEscapeParam(InfoEscapeLoc, EscapeDepth + Path);
      }
    }
    if (Src.EscapeDepth != INT32_MAX) {
      return -setInfoEscapeParam(Src.InfoEscapeLoc, Src.EscapeDepth - Path);
    }
    return 0;
  }

  static bool classof(const ObjectLocation *Location) {
    return Location->isNew();
  }
};

struct ParamLocation : ObjectLocation {
  ParamLocation(Value *V, Function *F) : ObjectLocation(ParamLoc, V, F, 0) {
    EscapeLoc.insert(this);
  }

  int EscapeEdge(ObjectLocation &Src, int Path) override {
    if (Src.isNew()) {
      return -Src.EscapeEdge(*this, -Path);
    }

    if (Path == 0) {
      if (Parent == Src.Parent) {
        ZeroPathEscapeLoc.insert(&Src);
      }
      return 0;
    }
    if (Path > 0) {
      return -setEscapeParam(Src);
    }
    return Src.setEscapeParam(*this);
  }

  bool isParam() override { return true; }
};

Value *getObjectVal(Instruction &I) {
  auto CI = dyn_cast<CallBase>(&I);
  if (CI && CI->getCalledFunction()) {
    if (isRewriteableCangjieMallocFunc(CI->getCalledFunction()))
      return &I;
  }
  return nullptr;
}

#ifndef NDEBUG
void doPrintFuncObjects(SmallVectorImpl<ObjectLocation *> &AllLocations) {
  for (unsigned i = 0; i < AllLocations.size(); i++) {
    if (AllLocations[i]->Val) {
      LLVM_DEBUG(dbgs() << AllLocations[i]->Parent->getName() << " : "
                        << *AllLocations[i]->Val << "\n");
    } else {
      LLVM_DEBUG(dbgs() << AllLocations[i]->Parent->getName() << " :retval.\n");
    }
  }
}
#endif

// simple version. escape global, param's object are all escaped. We don't
// consider param and object passed by function calls.
// 0. escape from return inst
// 1. escape when store into/load from escaped object.
// 2. object size bigger than threshold.
// 3. escape as function's params.

// Values that can never legally be stored into a GC reference field.
static bool isValueInvalid(Value *V) {
  if (V->getValueID() == llvm::Value::UndefValueVal) {
    report_fatal_error("store/gcwrite an undef value!");
  }
  return V->getValueID() == llvm::Value::ConstantPointerNullVal;
}

class EADepthImpl : public InstVisitor<EADepthImpl> {
  // Befriend the base class so it can delegate to private visit methods.
  friend class InstVisitor<EADepthImpl>;

  // 27: 0x7ffffff
  static constexpr int MaxSupportArgSize = 27;

public:
  EADepthImpl(CallGraphUpdater *CGUpdater, Module *M,
      function_ref<LoopInfo &(Function &)> LookLoop)
      : CGUpdater(CGUpdater), M(M), LookupLoopInfo(LookLoop) {}

  void insertSCCFunctions(Function *Func) {
    SCCFunctions.insert(Func);
  }

  void initialize() {
    ArgsObjectNum = 0;
    MayReplaceObjectNum = 0;

    // init object locations
    for (Function *Func : SCCFunctions) {
      for (unsigned i = 0; i < Func->arg_size(); i++) {
        auto arg = Func->getArg(i);
        if (arg->getType()->isPointerTy()) {
          enqueueUsers(arg);
          ObjectLocation *Loc = new ParamLocation(arg, Func);
          AllLocations.push_back(Loc);
          ValueToLocMap[arg] = Loc;
        }
      }
      if (auto PT = dyn_cast<PointerType>(Func->getReturnType())) {
        // multi return.
        if (PT->getAddressSpace() == 1) {
          ObjectLocation *Loc = new ParamLocation(nullptr, Func);
          AllLocations.push_back(Loc);
          ValueToLocMap[Func] = Loc;
        }
      }
    }
    ArgsObjectNum = AllLocations.size();
    for (Function *Func : SCCFunctions) {
      LoopInfo &LI = LookupLoopInfo(*Func);
      for (BasicBlock &BB : *Func) {
        for (Instruction &I : BB) {
          collectStructCastToAS1Inst(I);
          Value *ObjectVal = getObjectVal(I);
          if (!ObjectVal) {
            if (auto PT = dyn_cast<PointerType>(I.getType())) {
              if (PT->getAddressSpace() == 1) {
                Queue.push_back(&I);
              }
            }
            continue;
          }
          unsigned LoopDepth = LI.getLoopDepth(&BB);
          BasicBlock *PreHeader = nullptr;
          if (LoopDepth > 0) {
            PreHeader =
                LI.getLoopFor(&BB)->getOutermostLoop()->getLoopPreheader();
          }
          ObjectLocation *Loc =
              new NewLocation(ObjectVal, Func, LoopDepth, PreHeader);
          // For NewFinalizer, pre-flight the rewrite conditions during the
          // first (EADepthImpl) analysis phase.  If the object cannot be
          // stack-allocated , mark it escaped now so that walkEdges propagates
          // the escape to every object referenced by this finalizer (e.g.
          // closures captured by the finalizer's free lambda). This must happen
          // here, before walkEdges runs, because the second phase
          // (EscapeAnalysisImpl) runs after ObjectLocation graph is destroyed.
          if (!Loc->Escaped) {
            if (auto *CB = dyn_cast<CallBase>(ObjectVal)) {
              Function *Callee = CB->getCalledFunction();
              if (Callee && isCangjieNewFinalizerFunc(Callee) &&
                  !canStackAllocateFinalizer(CB, M, &LI)) {
                Loc->Escaped = true;
              }
            }
          }
          AllLocations.push_back(Loc);
          ValueToLocMap[ObjectVal] = Loc;
          enqueueUsers(ObjectVal);
        }
      }
    }
    MayReplaceObjectNum = AllLocations.size() - ArgsObjectNum;

#ifndef NDEBUG
    doPrintFuncObjects(AllLocations);
#endif
  }

  void collectStructCastToAS1Inst(Instruction &I) {
    if (isa<AddrSpaceCastInst>(&I)) {
      Value *Base = findMemoryBasePointer(&I);
      if (AllocaInst *AI = dyn_cast<AllocaInst>(Base)) {
        StructCastToAS1.push_back(AI);
      }
    }
  }

  void calculateEscapeInfo(uint32_t &EscapeInfo, ObjectLocation *CurLoc,
                           ObjectLocation *L) const {
    if (L == CurLoc || CurLoc->Parent != L->Parent) {
      return;
    }
    if (!L->Val) {
      // 30: the second-highest bit present the arg escape to return value
      EscapeInfo |= 1 << 30;
      return;
    }
    Argument *Arg = dyn_cast<Argument>(L->Val);
    if (Arg == nullptr) {
      // 30: the second-highest bit present the arg escape to return value
      EscapeInfo |= 1 << 30;
    } else {
      EscapeInfo |= 1 << Arg->getArgNo();
    }
  }

  void recordParamEscapeInfos() const {
    for (unsigned Index = 0; Index < ArgsObjectNum; Index++) {
      auto CurLoc = AllLocations[Index];
      uint32_t HighEscapeInfo = 0;
      uint32_t LowEscapeInfo = 0;
      if (CurLoc->EscapeDepth != INT32_MAX) {
        HighEscapeInfo |= 1 << 29; // 29: InfoEscaped Info
        uint32_t RecordEscapeDepth =
            CurLoc->EscapeDepth > 3 ? 3 : CurLoc->EscapeDepth;
        HighEscapeInfo |= RecordEscapeDepth << 27; // 27, 28 : escaped depth
      }
      if (CurLoc->Escaped) {
        HighEscapeInfo |= 1 << 31; // 31: highest bit present global escape
      } else {
        if (CurLoc->EscapeLoc.size() > 1) {
          for (auto L : CurLoc->EscapeLoc) {
            calculateEscapeInfo(HighEscapeInfo, CurLoc, L);
          }
        }
        for (auto L : CurLoc->ZeroPathEscapeLoc) {
          calculateEscapeInfo(LowEscapeInfo, CurLoc, L);
        }
      }
      uint64_t EscapeInfo = HighEscapeInfo;
      // 32: HighEscapeInfo
      EscapeInfo = (EscapeInfo << 32) | LowEscapeInfo;
      if (EscapeInfo != 0) {
        if (CurLoc->Val == nullptr) {
          // return val
          CurLoc->Parent->setEscapeInfo(EscapeInfo);
          LLVM_DEBUG(dbgs() << CurLoc->Parent->getName() << " : "
                            << Twine::utohexstr(EscapeInfo) << "\n");
        } else {
          Argument *CurArg = dyn_cast<Argument>(CurLoc->Val);
          CurArg->setEscapeInfo(EscapeInfo);
          LLVM_DEBUG(dbgs() << CurArg->getParent()->getName() << " : "
                            << CurArg->getName() << ", ArgEscapeInfo: "
                            << Twine::utohexstr(EscapeInfo) << "\n");
        }
      }
    }
  }

  bool finishAll(CallGraphUpdater *CGUpdater) {
    recordParamEscapeInfos();
    bool Changed = false;
    NonEscapeRewriter Rewriter(CGUpdater, M);
    for (unsigned i = ArgsObjectNum; i < MayReplaceObjectNum + ArgsObjectNum;
         i++) {
      // New Obj in loop can't handle now.
      if (!AllLocations[i]->isEscaped()) {
        NewLocation *New = dyn_cast<NewLocation>(AllLocations[i]);
        Function *F = dyn_cast<Instruction>(New->Val)->getParent()->getParent();
        Rewriter.setLoopInfo(&LookupLoopInfo(*F));
        Changed |= Rewriter.rewrite(dyn_cast<Instruction>(New->Val),
                                    New->NewInstInsertBB);
      }
    }

    Rewriter.setStructAS1Flag();
    for (unsigned i = 0; i < StructCastToAS1.size(); i++) {
      Changed |= Rewriter.handleStructAS1Cast(StructCastToAS1[i]);
    }
    Changed |= Rewriter.runFinalizerUnwindProtection();

    for (unsigned i = 0; i < AllLocations.size(); i++) {
      delete AllLocations[i];
    }
    AllLocations.clear();
    StructCastToAS1.clear();
    if (HeapLoc != nullptr) {
      delete HeapLoc;
      HeapLoc = nullptr;
    }
    for (Function *F : SCCFunctions) {
      F->setEscapeAnalysis();
    }
    return Changed;
  }

  bool run() {
    if (AllLocations.size() == 0 && StructCastToAS1.size() == 0 &&
        Queue.empty()) {
      return false;
    }
    HeapLoc = new ObjectLocation(nullptr, nullptr, 0);
    HeapLoc->Escaped = true;

    // visit all relative instruction and append their relationship by edges.
    // now the origin escape loc is definite.
    while (!Queue.empty()) {
      visit(Queue.pop_back_val());
    }

    LLVM_DEBUG(dbgs() << "locations's escape info before walk edges.\n");
    for (unsigned i = 0; i < AllLocations.size(); i++) {
      if (AllLocations[i]->isEscaped())
        LLVM_DEBUG(dbgs() << *AllLocations[i]->Val << " : escape state "
                          << AllLocations[i]->EscapeState << "\n");
    }

    walkEdges();

    for (unsigned i = ArgsObjectNum; i < MayReplaceObjectNum + ArgsObjectNum;
         i++) {
      if (!AllLocations[i]->isEscaped()) {
        LLVM_DEBUG(dbgs() << " not escape: " << *AllLocations[i]->Val << "\n");
      } else {
        LLVM_DEBUG(dbgs() << " escape to heap: "
                          << *AllLocations[i]->Val << "\n");
        if (AllLocations[i]->EscapedInst != nullptr) {
          LLVM_DEBUG(dbgs() << "escape instruction:"
                            << *AllLocations[i]->EscapedInst << "\n");
        }
      }
    }

    return finishAll(CGUpdater);
  }

  bool isGCReadIntrinsic(Value *Base) {
    if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(Base)) {
      switch (II->getIntrinsicID()) {
        case Intrinsic::cj_gcread_ref:
        case Intrinsic::cj_gcread_static_ref:
          return true;
        default:
          return false;
      }
    }
    return false;
  }

  ObjectLocation *getOrCreateLocation(Value *V) {
    if (!dyn_cast<Argument>(V) && !dyn_cast<Instruction>(V)) {
      if (isa<GlobalVariable>(V) || isa<ConstantExpr>(V)) {
        Value *Base = findMemoryBasePointer(V);
        PointerType *PT = dyn_cast<PointerType>(Base->getType());
        if (PT == nullptr) {
          return nullptr;
        }
        Type *PointeeType = PT->getNonOpaquePointerElementType();
        if (containsGCPtrType(PointeeType)) {
          return HeapLoc;
        }
      }
      return nullptr;
    }
    auto Loc = ValueToLocMap[V];
    if (!Loc) {
      Value *Base = findMemoryBasePointer(V);
      Loc = ValueToLocMap[Base];
      if (Loc) {
        ValueToLocMap[V] = Loc;
        return Loc;
      }
      unsigned LoopDepth = 0;

      if (LoadInst *Load = dyn_cast<LoadInst>(Base)) {
        ObjectLocation* SrcLoc = getOrCreateLocation(Load->getPointerOperand());
        // If SrcLoc is nullptr, load value escaped.
        if (SrcLoc != nullptr) {
          LoopDepth = SrcLoc->LoopDepth;
        }
      } else if (Instruction *I = dyn_cast<Instruction>(Base)) {
        LoopInfo &LI = LookupLoopInfo(*I->getParent()->getParent());
        LoopDepth = LI.getLoopDepth(I->getParent());
      }
      ObjectLocation *NewLoc = new ObjectLocation(
          Base, dyn_cast<Instruction>(V)->getParent()->getParent(), LoopDepth);
      AllLocations.push_back(NewLoc);
      ValueToLocMap[Base] = NewLoc;
      ValueToLocMap[V] = NewLoc;
      Loc = NewLoc;
      // we will do enqueueUsers in visitLoadInst later.
      if (isa<LoadInst>(Base) || isa<PHINode>(Base) ||
          isa<ExtractValueInst>(Base) || isGCReadIntrinsic(Base)) {
        if (Visited.insert(cast<Instruction>(Base)).second) {
          Queue.push_back(cast<Instruction>(Base));
       }
      } else if (isa<Argument>(Base) || isa<Instruction>(Base)) {
        enqueueUsers(Base);
      }
    }
    return Loc;
  }

  void escapeHeap(ObjectLocation &Loc, int Path = -1) const {
    if (Loc.Val == nullptr) {
      return;
    }
    HeapLoc->insertEdge(&Loc, Path);
    Loc.insertEdge(HeapLoc, -Path);
  }

  // Mark a location as escaped to the heap at the given instruction. Shared by
  // the load / store / gcwrite / extractvalue handlers.
  void markEscaped(ObjectLocation *Loc, Instruction *I) {
    Loc->Escaped = true;
    Loc->EscapedInst = I;
    Loc->EscapeState |= EscapeOrigin | EscapeGlobal;
    escapeHeap(*Loc);
  }

  void handleSCCFunctionCall(CallBase &I, Function *CalledFunction) {
    for (unsigned Index = 0; Index < CalledFunction->arg_size(); Index++) {
      auto CallArg = I.getOperand(Index);
      if (CallArg->getType()->isPointerTy()) {
        auto VLoc = getOrCreateLocation(CallArg);
        if (VLoc == nullptr) {
          continue;
        }
        auto PLoc = getOrCreateLocation(CalledFunction->getArg(Index));
        VLoc->insertEdge(PLoc, 0);
        PLoc->insertEdge(VLoc, 0);
      }
    }
    if (auto PT = dyn_cast<PointerType>(I.getType())) {
      if (PT->getAddressSpace() == 1) {
        ObjectLocation *Ret = ValueToLocMap[CalledFunction];
        ObjectLocation *Loc = getOrCreateLocation(&I);
        Ret->insertEdge(Loc, 0);
        Loc->insertEdge(Ret, 0);
      }
    }
  }

  void handleEscapeInfo(ObjectLocation *VLoc, CallBase &I,
                        uint64_t EscapeInfo) {
    uint32_t HighEscapeInfo = EscapeInfo >> 32;
    uint32_t LowEscapeInfo = EscapeInfo;
    if (HighEscapeInfo & 0x20000000) {
      // 27 28 : escape depth
      VLoc->setInfoEscaped((HighEscapeInfo & 0x18000000) >> 27);
    }
    if (HighEscapeInfo & 0x80000000) {
      VLoc->Escaped = true;
      VLoc->EscapedInst = &I;
      escapeHeap(*VLoc);
      return;
    }
    // escape to Return Val
    if (HighEscapeInfo & 0x40000000) {
      auto PLoc = getOrCreateLocation(&I);
      if (PLoc != nullptr) {
        PLoc->insertEdge(VLoc, -1);
        VLoc->insertEdge(PLoc, 1);
      }
    }
    HighEscapeInfo &= 0x7ffffff;
    unsigned CurArg = 0;
    while (HighEscapeInfo != 0) {
      if (HighEscapeInfo & 0x1) {
        auto PLoc = getOrCreateLocation(I.getOperand(CurArg));
        if (PLoc != nullptr) {
          PLoc->insertEdge(VLoc, -1);
          VLoc->insertEdge(PLoc, 1);
        }
      }
      HighEscapeInfo = HighEscapeInfo >> 1;
      CurArg++;
    }
    CurArg = 0;
    if (LowEscapeInfo & 0x40000000) {
      auto PLoc = getOrCreateLocation(&I);
      PLoc->insertEdge(VLoc, 0);
      VLoc->insertEdge(PLoc, 0);
    }
    LowEscapeInfo &= 0x1fffffff;
    while (LowEscapeInfo != 0) {
      if (LowEscapeInfo & 0x1) {
        auto PLoc = getOrCreateLocation(I.getOperand(CurArg));
        if (PLoc != nullptr) { // recursive call
          PLoc->insertEdge(VLoc, 0);
          VLoc->insertEdge(PLoc, 0);
        }
      }
      LowEscapeInfo = LowEscapeInfo >> 1;
      CurArg++;
    }
  }

  void handleNormalCall(CallBase &I, Function *CalledFunction) {
    for (unsigned Index = 0; Index < CalledFunction->arg_size(); Index++) {
      auto CallArg = I.getOperand(Index);
      if (!CallArg->getType()->isPointerTy()) {
        continue;
      }
      auto VLoc = getOrCreateLocation(CallArg);
      if (VLoc == nullptr) {
        continue;
      }
      auto Argument = CalledFunction->getArg(Index);
      uint64_t EscapeInfo = Argument->getEscapeInfo();
      handleEscapeInfo(VLoc, I, EscapeInfo);
    }
    if (CalledFunction->getEscapeInfo() != 0) {
      auto VLoc = getOrCreateLocation(&I);
      handleEscapeInfo(VLoc, I, CalledFunction->getEscapeInfo());
    }
  }

  void handleCallArgs(CallBase &I) {
    Function *CalledFunction = I.getCalledFunction();
    if (!CalledFunction || CalledFunction->isVarArg() ||
        CalledFunction->isDeclaration() ||
        !CalledFunction->hasEscapeAnalysis() ||
        CalledFunction->arg_size() > MaxSupportArgSize) {
      escapeAllOperand(&I);
      return;
    }
    if (SCCFunctions.count(CalledFunction)) {
      handleSCCFunctionCall(I, CalledFunction);
    } else {
      handleNormalCall(I, CalledFunction);
    }
  }

  void handleLoad(Value *P, Instruction *I) {
    ObjectLocation *SrcLoc = getOrCreateLocation(P);
    ObjectLocation *Loc = getOrCreateLocation(I);
    if (SrcLoc) {
      if (SrcLoc->isParam()) {
        Loc->EscapedInst = I;
        Loc->EscapeLoc.insert(SrcLoc);
        Loc->EscapeState |= EscapeOrigin | EscapeParam;
      }
      Loc->insertEdge(SrcLoc, 1);
      SrcLoc->insertEdge(Loc, -1);
    } else {
      markEscaped(Loc, I);
    }
    enqueueUsers(I);
  }

  void visitLoadInst(LoadInst &I) {
    if (!containsGCPtrType(I.getType())) {
      return;
    }

    auto Src = I.getPointerOperand();
    if (isa<StructType>(I.getType())) {
      handleMemcpyOrWriteAgg(&I, &I, Src);
      return;
    }

    handleLoad(Src, &I);
  }

  void visitStoreInst(StoreInst &I) {
    Value *V = I.getValueOperand();
    if (!containsGCPtrType(V->getType()) || isValueInvalid(V)) {
      return;
    }
    Value *P = I.getPointerOperand();
    if (isa<StructType>(V->getType())) {
      handleMemcpyOrWriteAgg(&I, P, V);
      return;
    }
    auto Loc = getOrCreateLocation(P);
    auto SrcLoc = getOrCreateLocation(V);
    if (!Loc || !SrcLoc) {
      return;
    }
    if (SrcLoc->LoopDepth > Loc->LoopDepth) {
      SrcLoc->Escaped = true;
      SrcLoc->EscapedInst = &I;
      SrcLoc->EscapeState = EscapeLoop;
      escapeHeap(*SrcLoc);
    }
    Loc->insertEdge(SrcLoc, -1);
    SrcLoc->insertEdge(Loc, 1);
  }

  void visitGetElementPtrInst(GetElementPtrInst &GEP) { enqueueUsers(&GEP); }

  void visitBitCastInst(BitCastInst &BC) { enqueueUsers(&BC); }

  void visitAddrSpaceCastInst(AddrSpaceCastInst &AI) { enqueueUsers(&AI); }

  void visitInvokeInst(InvokeInst &II) {
    handleCallArgs(II);
  }

  void visitCallInst(CallInst &I) {
    if (I.getCalledFunction()) {
      if (handleCangjieSL(&I)) {
        return;
      }
    }
    if (const Function *F = I.getCalledFunction()) {
      switch (F->getIntrinsicID()) {
      default:
        break;
      case Intrinsic::memcpy:
      case Intrinsic::memmove:
        visitMemCpyOrMove(I);
        return;
      case Intrinsic::memset:
      case Intrinsic::lifetime_start:
      case Intrinsic::lifetime_end:
        return;
      }
    }
    handleCallArgs(I);
  }

  void visitMemCpyOrMove(Instruction &I) {
    Value *P = I.getOperand(0);
    Value *V = I.getOperand(1);
    handleMemcpyOrWriteAgg(&I, P, V);
  }

  void visitReturnInst(ReturnInst &I) {
    Value *Ret = I.getOperand(0);
    if (!isGCPointerType(Ret->getType())) {
      return;
    }
    auto Loc = getOrCreateLocation(Ret);
    if (Loc) {
      Loc->EscapedInst = &I;
      Loc->EscapeLoc.insert(Loc);
      Loc->EscapeState |= EscapeOrigin | EscapeRet | EscapeParam;
      auto RetLoc = ValueToLocMap[I.getParent()->getParent()];
      if (RetLoc != nullptr) {
        Loc->insertEdge(RetLoc, 0);
        RetLoc->insertEdge(Loc, 0);
      }
    }
  }

  void updateLoc(ObjectLocation *Loc, Value *Op) {
    if (auto OpLoc = getOrCreateLocation(Op)) {
      Loc->insertEdge(OpLoc, 0);
      OpLoc->insertEdge(Loc, 0);
    }
  }

  void visitPHINode(PHINode &PI) {
    auto Loc = getOrCreateLocation(&PI);
    for (unsigned Idx = 0; Idx < PI.getNumIncomingValues(); Idx++) {
      updateLoc(Loc, PI.getIncomingValue(Idx));
    }
    enqueueUsers(&PI);
  }

  void visitSelectInst(SelectInst &SI) {
    auto Loc = getOrCreateLocation(&SI);
    updateLoc(Loc, SI.getTrueValue());
    updateLoc(Loc, SI.getFalseValue());
  }

  void visitExtractValueInst(ExtractValueInst &EVT) {
    if (!containsGCPtrType(EVT.getType())) {
      return;
    }
    Value *Src = EVT.getAggregateOperand();
    if (isa<StructType>(EVT.getType())) {
      handleMemcpyOrWriteAgg(&EVT, &EVT, Src);
      return;
    }
    ObjectLocation *SrcLoc = getOrCreateLocation(Src);
    ObjectLocation *Loc = getOrCreateLocation(&EVT);
    if (SrcLoc) {
      if (SrcLoc->isParam()) {
        Loc->EscapedInst = &EVT;
        Loc->EscapeLoc.insert(SrcLoc);
        Loc->EscapeState |= EscapeOrigin | EscapeParam;
      }
      Loc->insertEdge(SrcLoc, 1);
      SrcLoc->insertEdge(Loc, -1);
    } else {
      markEscaped(Loc, &EVT);
    }
    enqueueUsers(&EVT);
  }

  void visitInstruction(Instruction &I) { escapeAllOperand(&I); }

private:
  CallGraphUpdater *CGUpdater;

  Module *M;

  function_ref<LoopInfo &(Function &)> LookupLoopInfo;

  SmallVector<ObjectLocation *, 8> AllLocations;

  unsigned ArgsObjectNum = 0;

  unsigned MayReplaceObjectNum = 0;

  SmallVector<Instruction *, 8> Queue;

  SmallPtrSet<User *, 8> Visited;

  SmallSetVector<Function *, 8> SCCFunctions;

  ObjectLocation *HeapLoc = nullptr;

  SmallVector<AllocaInst *, 8> StructCastToAS1;

  DenseMap<Value *, ObjectLocation *> ValueToLocMap;
  void enqueueUsers(Value *V) {
    Value *Base = findMemoryBasePointer(V);
    if (Base->getValueID() == llvm::Value::ConstantPointerNullVal) {
      return;
    }
    if (Base != V)
      ValueToLocMap[V] = ValueToLocMap[Base];

    for (Use &U : V->uses())
      if (Visited.insert(U.getUser()).second) {
        Queue.push_back(dyn_cast<Instruction>(U.getUser()));
      }
  }

  void escapeAllOperand(Instruction *I) {
    unsigned IsClosureCall = 0;
    if (auto CI = dyn_cast<CallInst>(I)) {
      if (CI->getMetadata("IsClosureCall")) {
        IsClosureCall = 1;
      }
    }

    if (isGCPointerType(I->getType()) ||
        isMemoryContainsGCPtrType(I->getType())) {
      if (auto Loc = getOrCreateLocation(I)) {
        markEscaped(Loc, I);
      }
    }
    if (IsClosureCall) {
        auto V = I->getOperand(0);
        if (auto Loc = getOrCreateLocation(V)) {
          Loc->setInfoEscaped(0);
        }
    }
    for (unsigned i = IsClosureCall; i < I->getNumOperands() - IsClosureCall;
         i++) {
      auto V = I->getOperand(i);
      if (!(isGCPointerType(V->getType()) ||
          isMemoryContainsGCPtrType(V->getType())))
        continue;
      if (auto Loc = getOrCreateLocation(V)) {
        markEscaped(Loc, I);
      }
    }
  }

  void handleMemcpyOrWriteAgg(Instruction *I, Value *P, Value *V) {
    auto VLoc = getOrCreateLocation(V);
    auto PLoc = getOrCreateLocation(P);
    if (!VLoc || !PLoc) {
      return;
    }

    if (VLoc->LoopDepth > PLoc->LoopDepth) {
      VLoc->setInfoEscaped(0);
    }
    PLoc->insertEdge(VLoc, 0);
    VLoc->insertEdge(PLoc, 0);
  }

  void handleGCWrite(IntrinsicInst *II) {
    Value *V = II->getOperand(0);
    Type *T = V->getType();
    if (!T->isPointerTy() || T->getPointerAddressSpace() != 1 ||
        isValueInvalid(V)) {
      return;
    }

    Value *P = II->getOperand(2); // 2: direct store pointer
    auto VLoc = getOrCreateLocation(V);
    auto PLoc = getOrCreateLocation(P);
    if (VLoc == nullptr)
      return;
    assert(PLoc != nullptr && "PLoc is null when handle GCWrite!");
    if (VLoc->LoopDepth > PLoc->LoopDepth) {
      VLoc->Escaped = true;
      VLoc->EscapedInst = II;
      VLoc->EscapeState = EscapeLoop;
      escapeHeap(*VLoc);
    }
    PLoc->insertEdge(VLoc, -1);
    VLoc->insertEdge(PLoc, 1);
  }

  void handleGCWriteStatic(IntrinsicInst *II) {
    Value *V = II->getOperand(0);
    Type *T = V->getType();
    if (!T->isPointerTy() || T->getPointerAddressSpace() != 1 ||
        isValueInvalid(V)) {
      return;
    }

    auto Loc = getOrCreateLocation(V);
    if (!Loc)
      return;
    markEscaped(Loc, II);
  }

  void handleGCRead(IntrinsicInst *II, bool IsStatic) {
    Value *P = II->getArgOperand(IsStatic ? 0 : 1);
    handleLoad(P, II);
  }

  void handleGCWriteAgg(Instruction *I, bool IsStatic) {
    Value *P = I->getOperand(IsStatic ? 0 : 1);
    Value *V = I->getOperand(IsStatic ? 1 : 2);
    handleMemcpyOrWriteAgg(I, P, V);
  }

  void handleGCReadAgg(Instruction *I, bool IsStatic) {
    Value *P = I->getOperand(0);
    Value *V = I->getOperand(IsStatic ? 1 : 2);
    handleMemcpyOrWriteAgg(I, P, V);
  }

  void handleArrayCopyto(Instruction *I) {
    Value *P = I->getOperand(0);
    Value *V = I->getOperand(2);
    handleMemcpyOrWriteAgg(I, P, V);
  }

  bool handleCangjieSL(Instruction *I) {
    IntrinsicInst *II = dyn_cast<IntrinsicInst>(I);
    if (II) {
      switch (II->getIntrinsicID()) {
      case Intrinsic::cj_gcwrite_ref:
        handleGCWrite(II);
        return true;
      case Intrinsic::cj_gcwrite_static_ref:
        handleGCWriteStatic(II);
        return true;
      case Intrinsic::cj_gcwrite_struct:
        handleGCWriteAgg(II, false);
        return true;
      case Intrinsic::cj_gcwrite_static_struct:
      case Intrinsic::cj_assign_generic:
      case Intrinsic::cj_copy_struct_field:
        handleGCWriteAgg(II, true);
        return true;
      case Intrinsic::cj_array_copy_ref:
      case Intrinsic::cj_array_copy_struct:
        handleArrayCopyto(I);
        return true;
      case Intrinsic::cj_gcread_ref:
        handleGCRead(II, false);
        return true;
      case Intrinsic::cj_gcread_static_ref:
        handleGCRead(II, true);
        return true;
      case Intrinsic::cj_gcread_struct:
        handleGCReadAgg(II, false);
        return true;
      case Intrinsic::cj_gcread_static_struct:
        handleGCReadAgg(II, true);
        return true;
      default:
        break;
      }
    }
    return false;
  }

  void walkEdge(ObjectLocation &Loc) const {
    if (Loc.Visited) {
      return;
    }

    SmallVector<ObjectLocation *, 8> VisitLoc;
    VisitLoc.push_back(&Loc);
    Loc.Visited = true;
    while (!VisitLoc.empty()) {
      auto CurLoc = VisitLoc.pop_back_val();
      bool PushCur = false;
      for (auto *EdgeLoc : CurLoc->EdgesSet) {
        int EdgeState = CurLoc->EscapeEdge(*EdgeLoc, CurLoc->EdgesMap[EdgeLoc]);
        if (EdgeState == 1) {
          VisitLoc.push_back(EdgeLoc);
          EdgeLoc->Visited = true;
        }
        if (EdgeState == -1 && PushCur == false) {
          VisitLoc.push_back(CurLoc);
          PushCur = true;
        }
      }
    }
  }

  void walkEdges() {
    deleteTempLocs();

    SmallVector<ObjectLocation *, 8> EscapedLoc;
    for (unsigned i = 0; i < ArgsObjectNum + MayReplaceObjectNum; i++) {
      EscapedLoc.push_back(AllLocations[i]);
    }
    if (HeapLoc != nullptr) {
      EscapedLoc.push_back(HeapLoc);
    }
    while (!EscapedLoc.empty()) {
      auto CurLoc = EscapedLoc.pop_back_val();
      walkEdge(*CurLoc);
    }
  }

  void deleteTempLocs() {
    if (SCCFunctions.size() != 1) {
      for (unsigned Index = 0; Index < ArgsObjectNum; Index++) {
        AllLocations[Index]->passEdges(false);
      }
    }
    // These locations may have interdependent relationships. To minimize
    // compilation time as much as possible, nodes with fewer reachable edges
    // need to be processed first
    llvm::sort(AllLocations.begin() + ArgsObjectNum + MayReplaceObjectNum,
               AllLocations.end(), [](ObjectLocation *L, ObjectLocation *R) {
                 return L->EdgesSet.size() < R->EdgesSet.size();
               });
    for (unsigned Index = ArgsObjectNum + MayReplaceObjectNum;
         Index < AllLocations.size(); Index++) {
      ObjectLocation *CurLoc = AllLocations[Index];
      CurLoc->passEdges(true);
      AllLocations[Index] = nullptr;
      delete CurLoc;
    }
  }
};
}
class GCPtr;
static void memPtrspreadEscape(BasicBlock *BB, unsigned ES,
                               MapVector<GCPtr *, unsigned> &SuccBBInfo,
                               GCPtr *P, SmallVectorImpl<int> &Offset,
                               bool Direct);

static void countRefOffsets(StructType *ST, SmallVectorImpl<uint64_t> &Offsets,
                            uint64_t BaseOff, const DataLayout &DL,
                            unsigned Left, unsigned Right);

static void countArrayOffsets(ArrayType *AT, SmallVectorImpl<uint64_t> &Offsets,
                              uint64_t BaseOff, const DataLayout &DL,
                              unsigned Left, unsigned Right) {
  uint32_t Size = AT->getNumElements();
  Type *ElementType = AT->getElementType();
  if (Size == 0) { // Array type
    uint32_t ElementSize = DL.getTypeAllocSize(ElementType);
    Size = (Right + ElementSize - 1) / ElementSize;
  }
  if (isa<PointerType>(ElementType)) {
    for (unsigned Idx = 0; Idx < Size; Idx++) {
      uint64_t Off = BaseOff + Idx * 8;
      if (Off >= Left && Off < Right) {
        Offsets.push_back(Off);
      }
    }
  } else if (StructType *ArrayStruct = dyn_cast<StructType>(ElementType)) {
    uint32_t DerivedOffset = DL.getStructLayout(ArrayStruct)->getSizeInBytes();
    for (unsigned Idx = 0; Idx < Size; Idx++) {
      countRefOffsets(ArrayStruct, Offsets, BaseOff + Idx * DerivedOffset, DL,
                      Left, Right);
    }
  } else if (ArrayType *AET = dyn_cast<ArrayType>(ElementType)) {
    uint32_t DerivedOffset = DL.getTypeSizeInBits(ElementType);
    for (unsigned Idx = 0; Idx < Size; Idx++) {
      countArrayOffsets(AET, Offsets, BaseOff + Idx * DerivedOffset, DL, Left,
                        Right);
    }
  }
}

static void countRefOffsets(StructType *ST, SmallVectorImpl<uint64_t> &Offsets,
                            uint64_t BaseOff, const DataLayout &DL,
                            unsigned Left, unsigned Right) {
  for (uint32_t Idx = 0; Idx < ST->getNumElements(); Idx++) {
    uint64_t Off = DL.getStructLayout(ST)->getElementOffset(Idx) + BaseOff;
    Type *ET = ST->getElementType(Idx);
    if (Off > Right) {
      return;
    }
    if (PointerType *EPT = dyn_cast<PointerType>(ET)) {
      if (isGCPointerType(EPT) && Off >= Left) {
        Offsets.push_back(Off);
      }
    } else if (StructType *EST = dyn_cast<StructType>(ET)) {
      countRefOffsets(EST, Offsets, Off, DL, Left, Right);
    } else if (ArrayType *AT = dyn_cast<ArrayType>(ET)) {
      countArrayOffsets(AT, Offsets, Off, DL, Left, Right);
    }
  }
}

class GCPtr {
public:
  explicit GCPtr(Value *Val, unsigned Depth, CJEscapeAnalysis &EAImpl)
      : P(Val), IsLoad(isa<LoadInst>(Val) || isa<ExtractValueInst>(Val)),
        IsPhiOrSelect(isa<PHINode>(Val) || isa<SelectInst>(Val)),
        LoopDepth(Depth), EA(EAImpl), Id(EAImpl.getNextId()) {
    if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(Val))
      IsLoad |= II->getIntrinsicID() == Intrinsic::cj_gcread_ref;
  }

  virtual ~GCPtr() = default;

  virtual bool equal(GCPtr *V) { return V->isPtr() && (P == V->P); }

  // Size in bytes of the object this pointer refers to, lazily derived from
  // the defining instruction (GCNew / load / phi / select); 0 = unknown.
  virtual unsigned getObjSize();

  static GCPtr *get(Value *Val, CJEscapeAnalysis &EAImpl) {
    if (EAImpl.AllPtrLocInfo.count(Val)) {
      return EAImpl.AllPtrLocInfo[Val];
    }
    return nullptr;
  }

  static GCPtr *create(Value *Val, CJEscapeAnalysis &EAImpl,
                       LoopInfo *LI = nullptr) {
    if (Val->getValueID() == llvm::Value::ConstantPointerNullVal) {
      return nullptr;
    }
    int Off;
    Val = EAImpl.getBaseValue(Val, Off);
    if (EAImpl.AllPtrLocInfo.count(Val)) {
      return EAImpl.AllPtrLocInfo[Val];
    }
    unsigned LD = 0;
    if (LoadInst *Load = dyn_cast<LoadInst>(Val)) {
      GCPtr *Src = GCPtr::create(Load->getPointerOperand(), EAImpl, LI);
      // if Src is nullptr, Load inst escaped.
      if (Src != nullptr) {
        LD = Src->LoopDepth;
      }
    } else if (Instruction *I = dyn_cast<Instruction>(Val)) {
      if (LI != nullptr) {
        LD = LI->getLoopDepth(I->getParent());
      }
    }
    GCPtr *New = new GCPtr(Val, LD, EAImpl);
    if (EAImpl.isEscapedValue(Val)) {
      Function *CurFunc = EAImpl.ProcessedFunc;
      EAImpl.setEscaped(New, &CurFunc->getEntryBlock());
    }
    EAImpl.AllPtrLocInfo[Val] = New;
    return New;
  }

  virtual bool isPtr() { return true; }

  void setInfoEscaped() { InfoEscapeInfo = true; }

  bool isInitializing() { return InitializeFlag == Initializing; }

  void setInitializing() { InitializeFlag = Initializing; }

  bool isInitialized() { return InitializeFlag == Initialized; }

  void setInitialized() { InitializeFlag = Initialized; }

  bool isLoad() { return IsLoad; }

  bool isPhiOrSelect() { return IsPhiOrSelect; }

  virtual bool isDerivedPtr() { return IsLoad || IsPhiOrSelect; }

  unsigned getDirectSpread() { return DirectSpread; }

  bool canSpread(GCPtr *V, unsigned ES) {
    return V->isDerivedPtr() && ES > V->getDirectSpread();
  }

  bool isInfoEscaped() { return InfoEscapeInfo; }

  virtual void spreadInfoEscape(BasicBlock *BB, unsigned ES,
                                MapVector<GCPtr *, unsigned> &SuccBBInfo,
                                bool Direct = true) {
    if (isDerivedPtr() && Direct && DirectSpread < InfoEscaped) {
      DirectSpread = InfoEscaped;
    } else if (SuccBBInfo.count(this) && SuccBBInfo[this] >= InfoEscaped) {
      return;
    }
    if (SuccBBInfo[this] < InfoEscaped) {
      SuccBBInfo[this] = InfoEscaped;
    }
    for (unsigned It = 0; It < MPs.size(); ++It) {
      if (!SuccBBInfo.count(MPs[It]) || SuccBBInfo[MPs[It]] < ES) {
        SuccBBInfo[MPs[It]] = ES;
        MPs[It]->spreadEscape(BB, ES, SuccBBInfo, true);
      }
    }
    if (Direct) {
      for (auto &KV : BaseValue) {
        GCPtr *V = KV.first;
        V->spreadInfoEscape(BB, ES, SuccBBInfo, true);
      }
    }
    if (Alias.size() != 0) {
      for (auto &KV : Alias) {
        GCPtr *V = KV.first;
        V->spreadInfoEscape(BB, ES, SuccBBInfo, false);
      }
    }
    for (GCPtr *V : InfoEscapedVec) {
      V->spreadInfoEscape(BB, ES, SuccBBInfo, Direct);
    }
  }

  virtual void spreadMemEscape(BasicBlock *BB, unsigned ES,
                               MapVector<GCPtr *, unsigned> &SuccBBInfo,
                               SmallVectorImpl<int> &Offsets,
                               bool Direct = true) {
    if (!EA.tryMarkSpreadVisited(this, ES, Direct, Offsets)) {
      return;
    }
    int CurOffset = Offsets.pop_back_val();
    if (CurOffset >= 0) {
      Offsets.push_back(CurOffset);
      memPtrspreadEscape(BB, ES, SuccBBInfo, this, Offsets, Direct);
      return;
    }
    if (isPhiOrSelect()) {
      if (Offsets.size() > 0) {
        for (unsigned I = 0; I < MPs.size(); ++I)
          MPs[I]->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, Direct);
        for (GCPtr *V : InfoEscapedVec) {
          for (unsigned I = 0; I < V->MPs.size(); ++I)
            V->MPs[I]->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, Direct);
        }
      } else {
        for (unsigned I = 0; I < MPs.size(); ++I)
          MPs[I]->spreadEscape(BB, ES, SuccBBInfo, true);
        for (GCPtr *V : InfoEscapedVec) {
          for (unsigned I = 0; I < V->MPs.size(); ++I)
            V->MPs[I]->spreadEscape(BB, ES, SuccBBInfo, Direct);
        }
      }
      Offsets.push_back(CurOffset);
      if (Direct) {
        for (auto &KV : BaseValue) {
          GCPtr *V = KV.first;
          V->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, Direct);
        }
      }
      for (auto &KV : Alias) {
        GCPtr *V = KV.first;
        V->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, Direct);
      }
      return;
    }
    if (Offsets.size() > 0) {
      for (unsigned I = 0; I < MPs.size(); ++I)
        MPs[I]->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, Direct);
      if (Direct) {
        for (auto &KV : BaseValue) {
          GCPtr *V = KV.first;
          for (unsigned I = 0; I < V->MPs.size(); ++I)
            V->MPs[I]->spreadMemEscape(BB, ES, SuccBBInfo, Offsets,
                                       Direct);
        }
      }
      for (auto &KV : Alias) {
        GCPtr *V = KV.first;
        for (unsigned I = 0; I < V->MPs.size(); ++I)
          V->MPs[I]->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, Direct);
      }
      for (GCPtr *V : InfoEscapedVec) {
        for (unsigned I = 0; I < V->MPs.size(); ++I)
          V->MPs[I]->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, Direct);
      }
      Offsets.push_back(CurOffset);
    } else {
      for (unsigned I = 0; I < MPs.size(); ++I)
        MPs[I]->spreadEscape(BB, ES, SuccBBInfo, true);
      if (Direct) {
        for (auto &KV : BaseValue) {
          GCPtr *V = KV.first;
          for (unsigned I = 0; I < V->MPs.size(); ++I)
            V->MPs[I]->spreadEscape(BB, ES, SuccBBInfo, true);
        }
      }
      for (auto &KV : Alias) {
        GCPtr *V = KV.first;
        for (unsigned I = 0; I < V->MPs.size(); ++I)
          V->MPs[I]->spreadEscape(BB, ES, SuccBBInfo, Direct);
      }
      for (GCPtr *V : InfoEscapedVec) {
        for (unsigned I = 0; I < V->MPs.size(); ++I)
          V->MPs[I]->spreadEscape(BB, ES, SuccBBInfo, Direct);
      }
      Offsets.push_back(CurOffset);
    }
  }

  virtual void spreadEscape(BasicBlock *BB, unsigned ES,
                            MapVector<GCPtr *, unsigned> &SuccBBInfo,
                            bool Direct) {
    if (isDerivedPtr() && Direct && DirectSpread < ES) {
      DirectSpread = ES;
    }

    for (unsigned It = 0; It < MPs.size(); ++It) {
      if (!SuccBBInfo.count(MPs[It]) || SuccBBInfo[MPs[It]] < ES) {
        SuccBBInfo[MPs[It]] = ES;
        MPs[It]->spreadEscape(BB, ES, SuccBBInfo, true);
      }
    }
    // is direct escape
    if (isDerivedPtr() && Direct) {
      for (auto &KV : BaseValue) {
        GCPtr *V = KV.first;
        if (!SuccBBInfo.count(V) || SuccBBInfo[V] < ES ||
            canSpread(V, ES)) {
          SuccBBInfo[V] = ES;
          V->spreadEscape(BB, ES, SuccBBInfo, true);
        }
      }
    }
    if (Alias.size() != 0) {
      for (auto &KV : Alias) {
        GCPtr *V = KV.first;
        if (!SuccBBInfo.count(V) || SuccBBInfo[V] < ES) {
          SuccBBInfo[V] = ES;
          V->spreadEscape(BB, ES, SuccBBInfo, false);
        }
      }
    }
    for (GCPtr *V : InfoEscapedVec) {
      V->spreadInfoEscape(BB, ES, SuccBBInfo, Direct);
    }
  }

  void insertMem(GCPtr *MP) { MPs.push_back(MP); }

  // Base Value Ptr
  Value *P;
  bool IsLoad;
  bool IsPhiOrSelect;
  unsigned DirectSpread = NotEscape;
  bool InfoEscapeInfo = false;
  int MemOff = 0;
  InitializeState InitializeFlag = NotInitialize;
  unsigned LoopDepth;
  CJEscapeAnalysis &EA;
  // Stable unique id (assigned in IR-visit order)
  unsigned Id;
  // Size in bytes of the object this pointer refers to (0 = unknown). An offset
  // larger than the object itself cannot be a legitimate field offset, so it
  // will collapse to the escape-all value.
  unsigned ObjSize = 0;
  // for load
  MapVector<GCPtr *, SmallSet<uint64_t, 2>> BaseValue;
  // Ptr's memPtr info
  SmallVector<GCPtr *, 8> MPs;
  // pass to other instruction, such as
  // %3 = phi [%1, %bb1], [%2, %bb2]
  // which %1， %2's obj Alias stored %3.
  // Value is the offset of %1's base and %3
  MapVector<GCPtr *, SmallSet<int, 2>> Alias;
  SmallSetVector<GCPtr *, 8> InfoEscapedVec;
};

// The object size is computed iteratively by EscapeAnalysisImpl::
// computeAllObjSizes() before propagation.
unsigned GCPtr::getObjSize() { return ObjSize; }

class MemPtr final : public GCPtr {
public:
  explicit MemPtr(Value *Val, uint32_t Off, unsigned Depth,
                  CJEscapeAnalysis &EAImpl, GCPtr *P)
      : GCPtr(Val, Depth, EAImpl) {
    Offset = Off;
    Base = P;
  }

  ~MemPtr() override = default;

  static MemPtr *create(Value *Val, int Off, CJEscapeAnalysis &EAImpl,
                        LoopInfo *LI = nullptr, bool needInsert = true) {
    // Normalize extreme offsets to the unique escape-all value (-1):
    // negative offsets and offsets beyond the effective bound are not
    // meaningful for individual fields.
    if (Off < 0) {
      Off = -1;
    }
    int RawOff = Off;
    GCPtr *P = GCPtr::create(Val, EAImpl, LI);
    if (P == nullptr)
      return nullptr;
    if (Off > 0) {
      unsigned ObjSize = P->getObjSize();
      unsigned Bound = ObjSize ? ObjSize : getSpreadOffsetBound(EAImpl);
      if (Off > static_cast<int>(Bound))
        Off = -1;
    }
    // If the raw offset exceeds the current bound but a MemPtr was already
    // created at that exact offset (e.g., during initialize under the looser
    // pre-sizing bound), reuse it. Collapsing to -1 here would orphan the
    // existing MemPtr and its DefineValues from the offset-precise channel.
    if (Off == -1 && RawOff > 0) {
      auto RawIt = EAImpl.AllMemLocInfo.find(std::make_pair(Val, RawOff));
      if (RawIt != EAImpl.AllMemLocInfo.end()) {
        MemPtr *M = RawIt->second;
        if (needInsert && !M->InsertedIntoMPs) {
          P->insertMem(M);
          M->InsertedIntoMPs = true;
        }
        return M;
      }
    }
    auto CacheIt = EAImpl.AllMemLocInfo.find(std::make_pair(Val, Off));
    if (CacheIt != EAImpl.AllMemLocInfo.end()) {
      MemPtr *M = CacheIt->second;
      // A MemPtr may first be created by an alias/weak path with
      // needInsert=false; backfill MPs when a later direct/strong path needs
      // the slot to be visible to base-escape enumeration.
      if (needInsert && !M->InsertedIntoMPs) {
        P->insertMem(M);
        M->InsertedIntoMPs = true;
      }
      return M;
    }
    MemPtr *New = new MemPtr(Val, Off, P->LoopDepth, EAImpl, P);
    EAImpl.AllMemLocInfo[std::make_pair(Val, Off)] = New;
    if (needInsert) {
      P->insertMem(New);
      New->InsertedIntoMPs = true;
    }
    return New;
  }

  bool equal(GCPtr *V) override {
    if (V->isPtr()) {
      return false;
    }
    MemPtr *M = reinterpret_cast<MemPtr *>(V);
    return (M->P == P) && (M->Offset == Offset);
  }

  bool isPtr() override { return false; }

  // A memory location lives inside its base object, so it shares its size.
  unsigned getObjSize() override { return Base->getObjSize(); }

  void insertDefine(BasicBlock *BB, GCPtr *Val) {
    DefineValues[BB].push_back(Val);
  }

  // Propagate escape through the Base->BaseValue edges (strong propagation,
  // only meaningful when Direct). Shared by the three MemPtr spread methods.
  void spreadBaseValues(BasicBlock *BB, unsigned ES,
                        MapVector<GCPtr *, unsigned> &SuccBBInfo,
                        SmallVectorImpl<int> &Offsets, bool Direct) {
    if (!Direct)
      return;
    for (auto &KV : Base->BaseValue) {
      GCPtr *V = KV.first;
      for (auto BaseValueOffset : Base->BaseValue[V]) {
        Offsets.push_back(BaseValueOffset + Offset);
        V->spreadMemEscape(BB, ES, SuccBBInfo, Offsets);
        Offsets.pop_back();
      }
    }
  }

  // Propagate escape through the Base->Alias edges (weak propagation).
  // Shared by the three MemPtr spread methods.
  void spreadAliases(BasicBlock *BB, unsigned ES,
                     MapVector<GCPtr *, unsigned> &SuccBBInfo,
                     SmallVectorImpl<int> &Offsets) {
    if (Base->Alias.size() == 0)
      return;
    for (auto &KV : Base->Alias) {
      GCPtr *V = KV.first;
      for (auto AliasOffset : Base->Alias[V]) {
        if (Offset < AliasOffset)
          continue;
        if (AliasOffset != -1)
          Offsets.push_back(Offset - AliasOffset);
        else
          Offsets.push_back(AliasOffset); // == -1, escape-all offset
        V->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, false);
        Offsets.pop_back();
      }
    }
  }

  void spreadInfoEscape(BasicBlock *BB, unsigned ES,
                        MapVector<GCPtr *, unsigned> &SuccBBInfo,
                        bool Direct = true) override {
    if (isDerivedPtr() && Direct && DirectSpread < InfoEscaped) {
      DirectSpread = InfoEscaped;
    } else if (SuccBBInfo.count(this) && SuccBBInfo[this] >= InfoEscaped) {
      return;
    }
    if (SuccBBInfo[this] < InfoEscaped) {
      SuccBBInfo[this] = InfoEscaped;
    }
    for (auto &DefineMap : DefineValues) {
      for (GCPtr *Define : DefineMap.second) {
        if (!SuccBBInfo.count(Define) || SuccBBInfo[Define] < InfoEscaped) {
          Define->spreadInfoEscape(BB, Escaped, SuccBBInfo, Direct);
        }
      }
    }
    SmallVector<int, 8> Offsets;
    Offsets.push_back(-1); // -1 Escaped All
    Offsets.push_back(Offset);
    Base->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, Direct);
    Offsets.pop_back();
    if (Offset < 0) {
      return;
    }
    spreadBaseValues(BB, ES, SuccBBInfo, Offsets, Direct);
    spreadAliases(BB, ES, SuccBBInfo, Offsets);
  }

  void spreadMemEscape(
      BasicBlock *BB, unsigned ES, MapVector<GCPtr *, unsigned> &SuccBBInfo,
      SmallVectorImpl<int> &Offsets, bool Direct) override {
    if (!EA.tryMarkSpreadVisited(this, ES, Direct, Offsets)) {
      return;
    }
    int CurOffset = Offsets.back();
    for (auto &DefineMap : DefineValues) {
      for (GCPtr *Define : DefineMap.second) {
        if ((Offsets.size() == 1) && Define->isPtr()) {
          MemPtr *MPtr = MemPtr::create(Define->P, CurOffset, EA);
          if ((MPtr != nullptr) &&
              (!SuccBBInfo.count(MPtr) || SuccBBInfo[MPtr] < ES)) {
            SuccBBInfo[MPtr] = ES;
            MPtr->spreadEscape(BB, ES, SuccBBInfo, true);
          }
          continue;
        }
        Define->spreadMemEscape(BB, ES, SuccBBInfo, Offsets);
      }
    }
    // Depth-limit the cumulative offset path: beyond MaxSpreadDepth, collapse
    // to the escape-all offset (-1) so the number of distinct (node, Offsets)
    // states stays bounded.
    int LimitedOffset = (Offsets.size() < MaxSpreadDepth)? Offset : -1;
    Offsets.push_back(LimitedOffset);
    Base->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, Direct);
    Offsets.pop_back();
    if (Offset < 0) {
      return;
    }
    spreadBaseValues(BB, ES, SuccBBInfo, Offsets, Direct);
    spreadAliases(BB, ES, SuccBBInfo, Offsets);
  }

  void spreadEscape(BasicBlock *BB, unsigned ES,
                    MapVector<GCPtr *, unsigned> &SuccBBInfo,
                    bool Direct) override {
    if (isDerivedPtr() && Direct && DirectSpread < ES) {
      DirectSpread = ES;
    }
    for (auto &DefineMap : DefineValues) {
      for (GCPtr *Define : DefineMap.second) {
        if (!SuccBBInfo.count(Define) || SuccBBInfo[Define] < ES ||
            canSpread(Define, ES)) {
          SuccBBInfo[Define] = ES;
          Define->spreadEscape(BB, ES, SuccBBInfo, true);
        }
      }
    }
    if (Offset < 0 || Base->InfoEscapedVec.size() != 0) {
      Base->spreadInfoEscape(BB, Escaped, SuccBBInfo, Direct);
      return;
    }
    SmallVector<int, 8> Offsets;
    spreadBaseValues(BB, ES, SuccBBInfo, Offsets, Direct);

    spreadAliases(BB, ES, SuccBBInfo, Offsets);
  }

  bool isDerivedPtr() override { return Base->isDerivedPtr(); }
  GCPtr *Base;
  int Offset;
  bool InsertedIntoMPs = false;
  MapVector<BasicBlock *, SmallVector<GCPtr *>> DefineValues;
};

static void memPtrspreadEscape(
    BasicBlock *BB, unsigned ES, MapVector<GCPtr *, unsigned> &SuccBBInfo,
    GCPtr *P, SmallVectorImpl<int> &Offsets, bool Direct) {
  int CurOffset = Offsets.back();
  MemPtr *MPtr = MemPtr::create(P->P, CurOffset, P->EA, nullptr, Direct);
  if (MPtr == nullptr) {
    return;
  }
  if (Offsets.size() == 1) {
    if (!SuccBBInfo.count(MPtr) || SuccBBInfo[MPtr] < ES) {
      SuccBBInfo[MPtr] = ES;
      MPtr->spreadEscape(BB, ES, SuccBBInfo, Direct);
    }
  } else {
    Offsets.pop_back();
    MPtr->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, Direct);
    Offsets.push_back(CurOffset);
  }
}

// Return true if a concrete Back above the per-object bound should be kept in
// the visited key instead of being collapsed to -1. This is needed when the
// current state can materialize a MemPtr whose raw offset was already created
// (e.g., during initialize under the looser pre-sizing bound). Collapsing such
// a Back would make two different raw backs share the same memo key and skip
// the second one.
//
// GCPtr::spreadMemEscape materializes MemPtr(P->P, Back) through
// memPtrspreadEscape. MemPtr::spreadMemEscape does not create (P->P, Back),
// but its define branch (Offsets.size() == 1) and the recursive spread through
// pointer Defines can materialize MemPtr(Define->P, Back). Check both so
// MemPtr-layer memoization does not collapse distinct raw targets that are
// already registered.
static bool shouldKeepConcreteBack(CJEscapeAnalysis &EA, GCPtr *P, int Back) {
  if (P->isPtr())
    return EA.AllMemLocInfo.count(std::make_pair(P->P, Back));

  MemPtr *M = static_cast<MemPtr *>(P);
  for (auto &DefineMap : M->DefineValues)
    for (GCPtr *Define : DefineMap.second)
      if (Define->isPtr() &&
          EA.AllMemLocInfo.count(std::make_pair(Define->P, Back)))
        return true;
  return false;
}

void CJEscapeAnalysis::setInfoEscaped(GCPtr *GP, BasicBlock *BB) {
  assert(GP && "CJEscapeAnalysis::setInfoEscaped GCPtr should not be nullptr!");
  GP->setInfoEscaped();
  if (!EscapeBBInfo[BB].count(GP)) {
    EscapeBBInfo[BB][GP] = NotEscape;
  }
}

bool CJEscapeAnalysis::tryMarkSpreadVisited(GCPtr *P, unsigned ES, bool Direct,
                                            SmallVectorImpl<int> &Offsets) {
  int Back = Offsets.empty() ? 0 : Offsets.back();
  if (Back < 0) {
    Back = -1; // normalize escape-all offsets
  } else if (Back > 0) {
    // Per-object bound: an offset beyond the current node's own object size
    // is not a legitimate field offset. Falls back to the global bound when
    // the object size is unknown.
    unsigned ObjSize = P->getObjSize();
    unsigned Bound = ObjSize ? ObjSize : getSpreadOffsetBound(*this);
    if (Back > static_cast<int>(Bound) &&
        !shouldKeepConcreteBack(*this, P, Back))
      Back = -1;
  }
  unsigned DepthBucket = Offsets.size() > MaxSpreadDepth ? MaxSpreadDepth
                                                         : Offsets.size();
  // Pack the state into a compact integer key:
  //   Id (32 bits) | Reserved (11 bits) | Direct (1 bit) | ES (2 bits)
  //   | Back+1 (13 bits) | DepthBucket (5 bits).
  // Id occupies the high 32 bits. The Reserved field spans the 22nd through
  // 32nd bits (0-based bit 21..31) and must remain zero for future expansion.
  // Id is the stable unique id assigned at GCPtr construction; Back+1 keeps
  // the escape-all (-1) offset addressable.
  uint64_t BaseKey = ((uint64_t)P->Id << 32) | // Id: bits 32..63
                     ((uint64_t)(ES & 0x3) << 18) |
                     ((uint64_t)(Back + 1) << 5) |
                     (DepthBucket & 0x1F);
  uint64_t TrueKey = BaseKey | (1ULL << 20); // Direct bit: bit 20

  // Direct==true is strictly stronger than Direct==false for the same state:
  // it performs all Direct==false propagation plus the BaseValue/needInsert
  // work. Therefore a Direct==false visit is redundant once a Direct==true
  // visit of the same state has happened, while a Direct==true visit is never
  // covered by an earlier Direct==false visit.
  if (Direct) {
    return SpreadVisited.insert(TrueKey).second;
  }
  if (SpreadVisited.count(TrueKey))
    return false;
  return SpreadVisited.insert(BaseKey).second;
}

Value *CJEscapeAnalysis::getBaseValue(Value *CV, int &Offset) {
  Offset = 0;
  bool InfoEscapeInfo = false;
  Value *V = CV;
  while (true) {
    if (auto *CI = dyn_cast<CastInst>(V)) {
      if (!CI->getType()->isPointerTy()) { // ptr2int
        V = CI->getOperand(0);
        continue;
      }
      V = CI->stripPointerCasts();
      // If we find a cast instruction here, it means we've found a cast which
      // is not simply a pointer cast (i.e. an inttoptr).
      if (isa<IntToPtrInst>(V)) {
        V = CI->getOperand(0);
      }
    } else if (auto *GEPI = dyn_cast<GetElementPtrInst>(V)) {
      APInt GEPOffset(GEPI->getModule()->getDataLayout().getIndexSizeInBits(0),
                      0);
      if (GEPI->accumulateConstantOffset(GEPI->getModule()->getDataLayout(),
                                         GEPOffset)) {
        Offset += GEPOffset.getZExtValue();
      } else {
        InfoEscapeInfo = true;
      }
      V = GEPI->getPointerOperand();
    } else if (auto *CE = dyn_cast<ConstantExpr>(V)) {
      V = CE->getOperand(0);
    } else {
      break;
    }
  }
  if (InfoEscapeInfo) {
    Offset = -1;
  }
  return V;
}

bool CJEscapeAnalysis::isEscapedValue(Value *V) {
  int Offset = 0;
  Value *Base = getBaseValue(V, Offset);
  if (isa<Argument>(Base)) {
    return true;
  }
  if (isa<GlobalVariable>(Base) || isa<ConstantExpr>(Base)) {
    if (auto PT = dyn_cast<PointerType>(Base->getType())) {
      if (!containsGCPtrType(PT->getNonOpaquePointerElementType())) {
        return false;
      }
    }
    return true;
  }
  return false;
}

class EscapeAnalysisImpl : public InstVisitor<EscapeAnalysisImpl>,
                           public CJEscapeAnalysis {
  friend class InstVisitor<EscapeAnalysisImpl>;

public:
  EscapeAnalysisImpl(CallGraphUpdater &CGUpdater, Module *M,
                     function_ref<LoopInfo &(Function &)> LookLoop)
      : CGUpdater(CGUpdater), M(M), LookupLoopInfo(LookLoop) {}

  void insertSCCFunction(Function *Func) { SCCFunctions.insert(Func); }
#ifndef NDEBUG
  void dumpBBInfo(BasicBlock &BB, bool BeforeRun) {
    LLVM_DEBUG(dbgs() << "  bb." << BB.getName() << "\n");
    if (EscapeBBInfo.count(&BB)) {
      LLVM_DEBUG(dbgs() << "    bb value escape state:\n");
      for (auto EscapeInfo : EscapeBBInfo[&BB]) {
        LLVM_DEBUG(dbgs() << "    " << *(EscapeInfo.first->P) << " : ");
        switch (EscapeInfo.second) {
        case NotEscape:
          LLVM_DEBUG(dbgs() << "NotEscape\n");
          break;
        case Escaped:
          LLVM_DEBUG(dbgs() << "Escaped");
          if (!EscapeInfo.first->isPtr()) {
            LLVM_DEBUG(dbgs() << " offset: "
                              << ((MemPtr *)EscapeInfo.first)->Offset);
          }
          LLVM_DEBUG(dbgs() << "\n");
          break;
        default:
          LLVM_DEBUG(dbgs() << "info escape state.\n");
          break;
        }
      }
    }
    if (StoreRelations.count(&BB) && BeforeRun) {
      LLVM_DEBUG(dbgs() << "    store relations:\n");
      for (auto StoreRelation : StoreRelations[&BB]) {
        LLVM_DEBUG(dbgs() << "      " << *StoreRelation.first
                          << " be stored:\n");
        for (auto Stored : StoreRelation.second) {
          LLVM_DEBUG(dbgs() << "        " << *Stored << "\n");
        }
      }
    }
    if (BeforeRun) {
      LLVM_DEBUG(dbgs() << "    defs:\n");
      for (const auto &DefInfo : Def[&BB]) {
        for (const auto &ValueVec : DefInfo.second) {
          LLVM_DEBUG(dbgs() << "      " << *DefInfo.first
                            << " offset :" << ValueVec.first << "\n");
          for (auto Value : ValueVec.second) {
            LLVM_DEBUG(dbgs() << "        " << *(Value->P) << "\n");
          }
        }
      }
    } else {
      LLVM_DEBUG(dbgs() << "    ins:\n");
      for (const auto &InInfo : In[&BB]) {
        for (const auto &ValueVec : InInfo.second) {
          LLVM_DEBUG(dbgs() << "      " << *InInfo.first
                            << " offset :" << ValueVec.first << "\n");
          for (auto Value : ValueVec.second) {
            LLVM_DEBUG(dbgs() << "        " << *(Value->P) << "\n");
          }
        }
      }
    }
  }

  void dumpAnalysisInfo(bool BeforeRun) {
    if (BeforeRun) {
      LLVM_DEBUG(dbgs() << "PartialEscapeAnalysis : after initialized.\n");
    } else {
      LLVM_DEBUG(dbgs() << "PartialEscapeAnalysis : after run.\n");
    }

    for (Function *Func : SCCFunctions) {
      LLVM_DEBUG(dbgs() << Func->getName() << "\n");
      for (BasicBlock &BB : *Func) {
        dumpBBInfo(BB, BeforeRun);
      }
    }
  }
#endif

  void initialize() {
    MaxObjSize = 0;
    MaxObjSizeComputed = false;
    for (Function *Func : SCCFunctions) {
      ProcessedFunc = Func;
      LI = &LookupLoopInfo(*Func);
      for (BasicBlock &BB : *Func) {
        for (Instruction &I : BB) {
          visit(I);
        }
      }
    }
#ifndef NDEBUG
    dumpAnalysisInfo(true);
#endif
  }
  void processLoadNew(GCPtr *LoadPtr) {
    Value *V = LoadPtr->P;
    BasicBlock *VBB = dyn_cast<Instruction>(V)->getParent();
    SmallSetVector<GCPtr *, 8> Visited;
    SmallVector<MemPtr *, 8> BaseMemValues;
    SmallVector<GCPtr *, 8> BaseLoadValues;
    if (LoadPtr->isInitializing() || LoadPtr->isInitialized()) {
      return;
    }
    LoadPtr->setInitializing();
    for (auto &BaseV : LoadPtr->BaseValue) {
      GCPtr *LoadBase = BaseV.first;
      Visited.insert(LoadBase);
      if (!LoadBase->isPtr()) {
        BaseMemValues.push_back((MemPtr *)LoadBase);
      } else if (LoadBase->isLoad()) {
        BaseLoadValues.push_back(LoadBase);
      } else {
        continue;
      }
    }
    for (unsigned Index = 0; Index < BaseMemValues.size(); Index++) {
      MemPtr *MemBasePtr = BaseMemValues[Index];
      if (In[VBB][MemBasePtr->P][MemBasePtr->Offset].size() == 0) {
        LoadPtr->BaseValue[MemBasePtr].insert(0);
      } else {
        LoadPtr->BaseValue.erase(MemBasePtr);
      }
      auto findMemBasePtr = [&] (int CurOff) {
        for (GCPtr *MemVal : In[VBB][MemBasePtr->P][CurOff]) {
          if (LoadPtr->BaseValue.count(MemVal) || !Visited.insert(MemVal)) {
            continue;
          }
          if (!MemVal->isPtr()) {
            BaseMemValues.push_back((MemPtr *)MemVal);
          } else if (MemVal->isLoad()) {
            BaseLoadValues.push_back(MemVal);
            LoadPtr->BaseValue[MemVal].insert(0);
          } else {
            LoadPtr->BaseValue[MemVal].insert(0);
          }
        }
      };
      findMemBasePtr(MemBasePtr->Offset);
      findMemBasePtr(-1);
    }
    for (unsigned Index = 0; Index < BaseLoadValues.size(); Index++) {
      GCPtr *LoadBasePtr = BaseLoadValues[Index];
      processLoadNew(LoadBasePtr);
      // LoadBasePtr State??
      // when initialized.
      if (LoadBasePtr->isInitialized()) {
        for (auto &PtrPtr : LoadBasePtr->BaseValue) {
          if (PtrPtr.first->isPtr() && PtrPtr.first->isLoad()) {
            continue;
          }
          for (auto BaseOffset : PtrPtr.second) {
            LoadPtr->BaseValue[PtrPtr.first].insert(BaseOffset);
          }
          if (PtrPtr.first->isPtr()) {
            for (auto BaseOffset : PtrPtr.second) {
              PtrPtr.first->Alias[LoadPtr].insert(BaseOffset);
            }
          } else {
            ((MemPtr *)PtrPtr.first)->DefineValues[
                ((Instruction *)LoadPtr->P)->getParent()].push_back(LoadPtr);
          }
        }
      } else {
        LLVM_DEBUG(dbgs() << "error cycle occurs" << *LoadBasePtr->P << "\n");
      }
    }
    for (auto &Ptr : LoadPtr->BaseValue) {
      if (Ptr.first->isPtr()) {
        for (auto BaseOffset : Ptr.second) {
          Ptr.first->Alias[LoadPtr].insert(BaseOffset);
        }
      } else {
        ((MemPtr *)Ptr.first)->DefineValues[
            ((Instruction *)LoadPtr->P)->getParent()].push_back(LoadPtr);
      }
    }
    LoadPtr->setInitialized();
  }

  void spreadFuncEscape(Function *Func, bool &Changed) {
    for (auto &BB : *Func) {
      auto &SuccBBInfo = EscapeBBInfo[&BB];
      for (auto *Pred : predecessors(&BB)) {
        if (!EscapeBBInfo.count(Pred)) {
          continue;
        }
        // SpreadEscape can update SuccBBInfo, which is the same MapVector as
        // EscapeBBInfo[Pred] for a self-loop BB; snapshot the predecessor state
        // before iterating to avoid mutating a container while iterating it.
        SmallVector<std::pair<GCPtr *, unsigned>> PredSnapshot;
        for (auto &PredInfo : EscapeBBInfo[Pred])
          PredSnapshot.push_back(PredInfo);
        for (auto BBInfo : PredSnapshot) {
          if (BBInfo.second == Escaped &&
              (!SuccBBInfo.count(BBInfo.first) ||
               SuccBBInfo[BBInfo.first] < Escaped)) {
            Changed = true;
            SuccBBInfo[BBInfo.first] = Escaped;
            // Any object stored into an Escaped memory location is to be
            // treated as Escaped.
            BBInfo.first->spreadEscape(&BB, Escaped, SuccBBInfo, true);
          } else if (BBInfo.second == InfoEscaped &&
                     (!SuccBBInfo.count(BBInfo.first) ||
                      SuccBBInfo[BBInfo.first] < InfoEscaped)) {
            Changed = true;
            BBInfo.first->spreadInfoEscape(&BB, Escaped, SuccBBInfo);
          }
        }
      }
    }
  }

  void processFunc(Function *Func) {
    // Start a fresh spreadMemEscape deduplication set for this function.
    resetSpreadVisited();
    bool Changed = true;
    for (auto &BB : *Func) {
      auto &BBInfo = EscapeBBInfo[&BB];
      SmallVector<GCPtr *, 8> EscapedValues;
      SmallVector<GCPtr *, 8> InfoEscapedValues;
      for (auto EscapeInfo : BBInfo) {
        if (EscapeInfo.second == Escaped) {
          EscapedValues.push_back(EscapeInfo.first);
        } else if (EscapeInfo.first->isInfoEscaped()) {
          InfoEscapedValues.push_back(EscapeInfo.first);
        }
      }
      for (unsigned Index = 0; Index < EscapedValues.size(); Index++) {
        EscapedValues[Index]->spreadEscape(&BB, Escaped, BBInfo, true);
      }
      for (unsigned Index = 0; Index < InfoEscapedValues.size(); Index++) {
        InfoEscapedValues[Index]->spreadInfoEscape(&BB, Escaped, BBInfo);
      }
    }
    while (Changed) {
      Changed = false;
      spreadFuncEscape(Func, Changed);
    }
  }

  void handleLiveness(Function *F) {
    bool Changed = true;
    bool NotInitialize = true;
    while (Changed) {
      Changed = false;
      for (auto &BB : *F) {
        auto &SuccBBInfo = Def[&BB];
        auto &SuccOut = Out[&BB];
        auto &InInfo = In[&BB];
        if (NotInitialize) { // first time to initialize.
          for (const auto &SuccDefs : Def[&BB]) {
            for (const auto &SuccDef : SuccDefs.second) {
              SuccOut[SuccDefs.first][SuccDef.first].insert(
                  SuccDef.second.begin(), SuccDef.second.end());
            }
          }
        }
        for (auto *Pred : predecessors(&BB)) {
          for (const auto &LivVal : Out[Pred]) {
            for (const auto &ValVec : LivVal.second) {
              for (auto Val : ValVec.second) {
                if (!InInfo[LivVal.first][ValVec.first].count(Val)) {
                  InInfo[LivVal.first][ValVec.first].insert(Val);
                  Changed = true;
                }
              }
            }
          }
          if (Pred == &BB) {
            continue;
          }
          for (const auto &LivVal : Out[Pred]) {
            for (const auto &ValVec : LivVal.second) {
              for (auto Val : ValVec.second) {
                if (SuccBBInfo[LivVal.first].count(ValVec.first) &&
                    SuccBBInfo[LivVal.first][ValVec.first].size() != 0) {
                  continue;
                }
                if ((!SuccOut[LivVal.first].count(ValVec.first)) ||
                    !SuccOut[LivVal.first][ValVec.first].count(Val)) {
                  Changed = true;
                  SuccOut[LivVal.first][ValVec.first].insert(Val);
                }
              }
            }
          }
        }
      }
      NotInitialize = false;
    }
  }

  // Aggregate the object size of G from its defining instruction without
  // recursing: GCNew calls carry their Klass size, loads/phis/selects read the
  // (already computed) size of their source/operands via GCPtr::get.
  unsigned deriveObjSize(GCPtr *G) {
    Value *V = G->P;
    if (auto *CI = dyn_cast<CallBase>(V)) {
      if (GlobalVariable *Klass = getNewKlass(CI)) {
        bool HasRefs = false;
        uint32_t AS = 0;
        bool NonMoveArray = false;
        Type *T = getAllocaType(Klass, HasRefs, AS, NonMoveArray,
                                dyn_cast<Instruction>(V));
        if (T != nullptr || NonMoveArray)
          return AS;
      }
      return 0;
    }
    if (auto *LI = dyn_cast<LoadInst>(V)) {
      if (GCPtr *Src = GCPtr::get(LI->getPointerOperand(), *this))
        return Src->ObjSize;
      return 0;
    }
    unsigned Max = 0;
    if (auto *PN = dyn_cast<PHINode>(V)) {
      for (unsigned I = 0; I < PN->getNumIncomingValues(); ++I)
        if (GCPtr *Inv = GCPtr::get(PN->getIncomingValue(I), *this))
          Max = std::max(Max, Inv->ObjSize);
      return Max;
    }
    if (auto *SI = dyn_cast<SelectInst>(V)) {
      for (Value *Op : {SI->getTrueValue(), SI->getFalseValue()})
        if (GCPtr *OpPtr = GCPtr::get(Op, *this))
          Max = std::max(Max, OpPtr->ObjSize);
      return Max;
    }
    return 0;
  }

  void computeAllObjSizes() {
    constexpr unsigned UseVectorSize = 4;
    DenseMap<GCPtr *, SmallVector<GCPtr *, UseVectorSize>> Uses;
    SmallVector<GCPtr *> Worklist;
    for (auto &KV : AllPtrLocInfo) {
      GCPtr *G = KV.second;
      Value *V = G->P;
      if (auto *CI = dyn_cast<CallBase>(V)) {
        // Seed: a GCNew carries its Klass size.
        if (GlobalVariable *Klass = getNewKlass(CI)) {
          bool HasRefs = false;
          uint32_t AS = 0;
          bool NonMoveArray = false;
          Type *T = getAllocaType(Klass, HasRefs, AS, NonMoveArray,
                                  dyn_cast<Instruction>(V));
          if (T != nullptr || NonMoveArray) {
            G->ObjSize = AS;
            Worklist.push_back(G);
          }
        }
      }
      // Build the reverse use index: def -> users.
      if (auto *LI = dyn_cast<LoadInst>(V)) {
        if (GCPtr *Src = GCPtr::get(LI->getPointerOperand(), *this))
          Uses[Src].push_back(G);
      } else if (auto *PN = dyn_cast<PHINode>(V)) {
        for (unsigned I = 0; I < PN->getNumIncomingValues(); ++I)
          if (GCPtr *Inc = GCPtr::get(PN->getIncomingValue(I), *this))
            Uses[Inc].push_back(G);
      } else if (auto *SI = dyn_cast<SelectInst>(V)) {
        for (Value *Op : {SI->getTrueValue(), SI->getFalseValue()})
          if (GCPtr *OpPtr = GCPtr::get(Op, *this))
            Uses[OpPtr].push_back(G);
      }
    }
    // Forward propagation: when a def's size grows, recompute its users.
    while (!Worklist.empty()) {
      GCPtr *G = Worklist.pop_back_val();
      for (GCPtr *Use : Uses[G]) {
        unsigned S = deriveObjSize(Use);
        if (S > Use->ObjSize) {
          Use->ObjSize = S;
          Worklist.push_back(Use);
        }
      }
    }
    MaxObjSizeComputed = true;
  }

  bool run() {
    bool Changed = false;
    if (GCNew.empty()) {
      cleanupAnalysisState();
      return Changed;
    }
    for (Function *Func : SCCFunctions) {
      ProcessedFunc = Func;
      handleLiveness(Func);
    }
    for (auto LoadPtr : LoadValues) {
      processLoadNew(LoadPtr);
    }
    // Compute per-object sizes before propagation so MemPtr offset saturation
    // uses each object's own bound. MemPtrs created during initialize /
    // processLoadNew come from direct IR accesses and are intentionally left
    // as-is; only MemPtrs created during propagation need to obey the
    // per-object bound to keep translation-derived offsets bounded.
    computeAllObjSizes();
    for (Function *Func : SCCFunctions) {
      processFunc(Func);
    }
#ifndef NDEBUG
    dumpAnalysisInfo(false);
#endif
    finishAll();
    if (OneWayNotEscaped.size() != 0) {
      LLVM_DEBUG(dbgs() << "Partial analysis object:\n");
    }
#ifndef NDEBUG
    for (auto Partial : OneWayNotEscaped) {
      LLVM_DEBUG(dbgs() << "  " << *Partial << "\n");
    }
#endif
    NonEscapeRewriter Rewriter(&CGUpdater, M);
    for (auto &Partial : GCNew) {
      if (!OnceEscapedValue.count(Partial.first)) {
        LLVM_DEBUG(dbgs() << "not escaped obj:");
        LLVM_DEBUG(dbgs() << "  " << *Partial.first << "\n");
        Function *F = Partial.first->getParent()->getParent();
        Rewriter.setLoopInfo(&LookupLoopInfo(*F));
        Changed |= Rewriter.rewrite(Partial.first, Partial.second);
      } else {
        LLVM_DEBUG(dbgs() << "partial escaped obj:");
        LLVM_DEBUG(dbgs() << "  " << *Partial.first << "\n");
      }
    }
    Changed |= Rewriter.runFinalizerUnwindProtection();

    cleanupAnalysisState();
    return Changed;
  }

  void cleanupAnalysisState() {
    for (auto PInfo : AllPtrLocInfo) {
      delete PInfo.second;
    }
    AllPtrLocInfo.clear();
    for (auto MemInfo : AllMemLocInfo) {
      delete MemInfo.second;
    }
    AllMemLocInfo.clear();
  }

  void walkBBs(BasicBlock *CurBB) {
    for (auto BBInfo : EscapeBBInfo[CurBB]) {
      if (BBInfo.second == Escaped && BBInfo.first->isPtr()) {
        OnceEscapedValue.insert(BBInfo.first->P);
      }
    }
  }

  void finishAll() {
    for (Function *Func : SCCFunctions) {
      ProcessedFunc = Func;
      for (BasicBlock &BB : *Func) {
        walkBBs(&BB);
      }
    }
  }

  void bindInfoEscape(GCPtr *P, GCPtr *V, BasicBlock *BB) {
    assert((P && V) && "CJEscapeAnalysis::bindInfoEscape GCPtr should not be nullptr!");
    P->InfoEscapedVec.insert(V);
    V->InfoEscapedVec.insert(P);
    unsigned PES = isEscapedValue(P->P) ? Escaped : NotEscape;
    unsigned VES = isEscapedValue(V->P) ? Escaped : NotEscape;
    if (!EscapeBBInfo[BB].count(P) || EscapeBBInfo[BB][P] < PES) {
      EscapeBBInfo[BB][P] = PES;
    }
    if (!EscapeBBInfo[BB].count(V) || EscapeBBInfo[BB][V] < VES) {
      EscapeBBInfo[BB][V] = VES;
    }
  }

  // Shared handling for "write GC reference V into pointer location P": record
  // the escape relationship (via MemPtr) and the Def entry. Used by both
  // cj_gcwrite_ref (handleGCWrite) and plain store (visitStoreInst).
  void handleWrite(Value *P, Value *V, BasicBlock *BB) {
    if (P->getValueID() == llvm::Value::ConstantPointerNullVal ||
        V->getValueID() == llvm::Value::ConstantPointerNullVal)
      return;
    GCPtr *GP = GCPtr::create(P, *this, LI);
    GCPtr *GV = GCPtr::create(V, *this, LI);
    if (!GP || !GV)
      return;
    if (isEscapedValue(P) || GP->LoopDepth < GV->LoopDepth) {
      if (auto VI = dyn_cast<Instruction>(V)) {
        setEscaped(GV, BB);
      }
    }
    int Offset = 0;
    Value *BaseP = getBaseValue(P, Offset);
    MemPtr *MP = MemPtr::create(BaseP, Offset, *this, LI);
    MP->insertDefine(BB, GV);

    if (isEscapedValue(V)) {
      setEscaped(MP, BB);
    }

    StoreRelations[BB][P].insert(V);
    if (Offset >= 0 && !Def[BB][BaseP][Offset].empty()) {
      Def[BB][BaseP][Offset].clear();
    }
    Def[BB][BaseP][Offset].insert(GV);
  }

  void handleGCWrite(IntrinsicInst *II) {
    Value *V = II->getOperand(0);
    Type *T = V->getType();
    if (!T->isPointerTy() || T->getPointerAddressSpace() != 1 ||
        isValueInvalid(V)) {
      return;
    }
    Value *P = II->getOperand(2);
    handleWrite(P, V, II->getParent());
  }

  void handleGCRead(IntrinsicInst *II, bool IsStatic) {
    Value *P = II->getArgOperand(IsStatic ? 0 : 1);
    handleLoad(P, II);
  }

  void handleGCWriteStatic(IntrinsicInst *II) {
    Value *V = II->getOperand(0);
    Type *T = V->getType();
    if (!T->isPointerTy() || T->getPointerAddressSpace() != 1 ||
        isValueInvalid(V)) {
      return;
    }
    setEscaped(GCPtr::create(V, *this, LI), II->getParent());
    StoreRelations[II->getParent()][II->getOperand(1)].insert(V);
  }

  void handleGCWriteAgg(Instruction *I, bool IsStatic) {
    Value *P = I->getOperand(IsStatic ? 0 : 1);
    Value *V = I->getOperand(IsStatic ? 1 : 2);
    ConstantInt *CSI = dyn_cast<ConstantInt>(I->getOperand(IsStatic ? 2 : 3));
    handleMemcpyOrWriteAgg(I, P, V, CSI);
  }

  void handleGCReadAgg(Instruction *I, bool IsStatic) {
    Value *P = I->getOperand(0);
    Value *V = I->getOperand(IsStatic ? 1 : 2);
    ConstantInt *CSI = dyn_cast<ConstantInt>(I->getOperand(IsStatic ? 2 : 3));
    handleMemcpyOrWriteAgg(I, P, V, CSI);
  }

  void handleArrayCopyto(Instruction *I) {
    Value *P = I->getOperand(1);
    Value *V = I->getOperand(3);
    ConstantInt *CSI = dyn_cast<ConstantInt>(I->getOperand(4));
    handleMemcpyOrWriteAgg(I, P, V, CSI);
  }

  void handleMemcpyOrWriteAgg(Instruction *I, Value *P, Value *V,
                              ConstantInt *CSI) {
    int POffset = 0;
    int Off;
    GCPtr *GP = GCPtr::create(getBaseValue(P, POffset), *this, LI);
    GCPtr *GV = GCPtr::create(getBaseValue(V, Off), *this, LI);
    if (GP == nullptr || GV == nullptr)
      return;
    unsigned Size = 0;
    const DataLayout &DL = M->getDataLayout();
    StructType *ST = nullptr;
    if (isa<LoadInst>(I)) {
      ST = dyn_cast<StructType>(P->getType());
      Size = DL.getStructLayout(ST)->getSizeInBytes();
    }
    if (isa<StoreInst>(I)) {
      ST = dyn_cast<StructType>(V->getType());
      Size = DL.getStructLayout(ST)->getSizeInBytes();
    }
    if ((!Size && CSI == nullptr) || GP->isInfoEscaped() ||
        GV->isInfoEscaped() || POffset < 0 || Off < 0) {
      bindInfoEscape(GP, GV, I->getParent());
      return;
    } else if (Size == 0) {
      Size = CSI->getZExtValue();
    }
    Type *CopyType = nullptr;

    SmallVector<uint64_t, 8> RefOffsets;
    Value *BaseV = getBaseValue(V, Off);
    int BeginOff = Off;
    if (ST != nullptr) {
      CopyType = ST;
      BeginOff = 0;
    } else {
      if (!isa<PointerType>(BaseV->getType())) {
        return;
      }
      CopyType =
          cast<PointerType>(BaseV->getType())->getNonOpaquePointerElementType();
    }
    if (auto *CopyST = dyn_cast<StructType>(CopyType)) {
      if (!containsGCPtrType(CopyST)) {
        return;
      }
      uint64_t Left = static_cast<uint64_t>(BeginOff);
      uint64_t Right = Left + Size;
      const auto &Offsets = getStructRefOffsets(CopyST, Right, DL);
      auto It = std::lower_bound(Offsets.begin(), Offsets.end(), Left);
      for (; It != Offsets.end() && *It < Right; ++It) {
        RefOffsets.push_back(*It);
      }
    } else {
      // 64: large size threshold
      if (Size > 64) {
        setInfoEscaped(GP, I->getParent());
        setInfoEscaped(GV, I->getParent());
        return;
      }
      for (uint64_t PtrOff = BeginOff; PtrOff < BeginOff + Size;
           PtrOff += alignEight) {
        RefOffsets.push_back(PtrOff);
      }
    }
    Value *Base = getBaseValue(P, POffset);

    if (isEscapedValue(P) || GP->LoopDepth < GV->LoopDepth) {
      for (auto Offset : RefOffsets) {
        // Info Escaped
        EscapeBBInfo[I->getParent()][MemPtr::create(
            BaseV, Offset - BeginOff + Off, *this, LI)] = Escaped;
      }
      return;
    } else if (isEscapedValue(V)) {
      for (auto Offset : RefOffsets) {
        uint64_t PVOffset = POffset + Offset - BeginOff;
        EscapeBBInfo[I->getParent()]
                    [MemPtr::create(Base, PVOffset, *this, LI)] = Escaped;
      }
      return;
    }
    SmallDenseMap<std::pair<Value *, uint64_t>, GCPtr *, 8> SrcMPCache;
    for (auto Offset : RefOffsets) {
      uint64_t PVOffset = POffset + Offset - BeginOff;
      uint64_t VPOffset = Off + Offset - BeginOff;
      auto CacheKey = std::make_pair(BaseV, VPOffset);
      GCPtr *SrcMP = nullptr;
      auto CacheIt = SrcMPCache.find(CacheKey);
      if (CacheIt != SrcMPCache.end()) {
        SrcMP = CacheIt->second;
      } else {
        SrcMP = findSrcMP(BaseV, VPOffset, I->getParent());
        SrcMPCache.insert({CacheKey, SrcMP});
      }
      // The definition of Base is itself, skip it.
      if (SrcMP->P == Base && VPOffset == PVOffset)
        continue;
      MemPtr *MP = MemPtr::create(Base, PVOffset, *this, LI);
      MP->insertDefine(I->getParent(), SrcMP);
      if (!Def[I->getParent()][Base][PVOffset].empty()) {
        Def[I->getParent()][Base][PVOffset].clear();
      }
      Def[I->getParent()][Base][PVOffset].insert(SrcMP);
    }
  }

  const SmallVector<uint64_t, 8> &
  getStructRefOffsets(StructType *ST, uint64_t RangeEnd,
                      const DataLayout &DL) {
    auto &BySize = StructRefOffsetsCache[ST];
    auto It = BySize.find(RangeEnd);
    if (It != BySize.end()) {
      return It->second;
    }
    SmallVector<uint64_t, 8> Offsets;
    if (RangeEnd != 0) {
      countRefOffsets(ST, Offsets, 0, DL, 0, RangeEnd);
      std::sort(Offsets.begin(), Offsets.end());
      Offsets.erase(std::unique(Offsets.begin(), Offsets.end()),
                    Offsets.end());
    }
    auto Inserted = BySize.insert({RangeEnd, std::move(Offsets)});
    return Inserted.first->second;
  }

  GCPtr *findSrcMP(Value *BaseV, uint64_t VPOffset, BasicBlock *CurBB) {
    auto BBIt = Def.find(CurBB);
    if (BBIt == Def.end())
      return MemPtr::create(BaseV, VPOffset, *this, LI);
    auto &BBDefs = BBIt->second;

    auto FindInMap = [&BBDefs, this](Value *BaseV,
                                     uint64_t VPOffset) -> GCPtr * {
      auto BaseIt = BBDefs.find(BaseV);
      if (BaseIt == BBDefs.end())
        return nullptr;
      auto OffIt = BaseIt->second.find(VPOffset);
      if (OffIt == BaseIt->second.end() || OffIt->second.empty())
        return nullptr;
      return *OffIt->second.begin();
    };

    GCPtr *Temp = FindInMap(BaseV, VPOffset);
    if (!Temp)
      return MemPtr::create(BaseV, VPOffset, *this, LI);
    GCPtr *CurP = nullptr;
    do {
      CurP = Temp;
      if (CurP->isPtr())
        return CurP;
      MemPtr *MP = reinterpret_cast<MemPtr *>(CurP);
      VPOffset = MP->Offset;
      BaseV = MP->P;
    } while ((Temp = FindInMap(BaseV, VPOffset)));
    return CurP;
  }

  bool handleCangjieSL(Instruction *I) {
    IntrinsicInst *II = dyn_cast<IntrinsicInst>(I);
    if (II) {
      switch (II->getIntrinsicID()) {
      case Intrinsic::cj_gcwrite_ref:
        handleGCWrite(II);
        return true;
      case Intrinsic::cj_gcwrite_static_ref:
        handleGCWriteStatic(II);
        return true;
      case Intrinsic::cj_gcwrite_struct:
        handleGCWriteAgg(II, false);
        return true;
      case Intrinsic::cj_gcwrite_static_struct:
        handleGCWriteAgg(II, true);
        return true;
      case Intrinsic::cj_array_copy_ref:
      case Intrinsic::cj_array_copy_struct:
        handleArrayCopyto(I);
        return true;
      case Intrinsic::cj_gcread_ref:
        handleGCRead(II, false);
        return true;
      case Intrinsic::cj_gcread_static_ref:
        handleGCRead(II, true);
        return true;
      case Intrinsic::cj_gcread_struct:
        handleGCReadAgg(II, false);
        return true;
      case Intrinsic::cj_gcread_static_struct:
        handleGCReadAgg(II, true);
        return true;
      default:
        break;
      }
    }
    return false;
  }

  void handleCallArgs(CallBase &I) {
    escapeAllOperand(&I);
    return;
  }

  void insertGCNew(Instruction *I) {
    BasicBlock *CurBB = I->getParent();
    LI = &LookupLoopInfo(*CurBB->getParent());
    Loop *L = LI->getLoopFor(CurBB);
    BasicBlock *PreHeader = nullptr;
    if (L) {
      PreHeader = L->getOutermostLoop()->getLoopPreheader();
    }
    GCNew.insert(std::make_pair(I, PreHeader));
  }

  void escapeAllOperand(Instruction *I) {
    int Offset = 0;
    if (isGCPointerType(I->getType())) {
      GCPtr *PI = GCPtr::create(I, *this, LI);
      setEscaped(PI, I->getParent());
    }

    for (unsigned Idx = 0; Idx < I->getNumOperands(); Idx++) {
      auto V = I->getOperand(Idx);
      if (!(isGCPointerType(V->getType()) ||
            isMemoryContainsGCPtrType(V->getType())))
        continue;
      GCPtr *Base = GCPtr::create(getBaseValue(V, Offset), *this, LI);
      if (Base != nullptr) {
        setEscaped(Base, I->getParent());
      }
    }
  }

  // If both GP1 and GP2 are not in a loop, should not call this functon.
  bool isInSameLoop(GCPtr *GP1, GCPtr *GP2) {
    assert(GP1->LoopDepth + GP2->LoopDepth > 0);
    if (GP1->LoopDepth != GP2->LoopDepth)
      return false;
    if (auto *I1 = dyn_cast<Instruction>(GP1->P)) {
      if (auto *I2 = dyn_cast<Instruction>(GP2->P)) {
        return LI->getLoopFor(I1->getParent()) ==
               LI->getLoopFor(I2->getParent());
      }
    }
    return false;
  }

  void visitPHINode(PHINode &PN) {
    if (!PN.getType()->isPointerTy()) {
      return;
    }
    GCPtr *P = GCPtr::create(&PN, *this, LI);
    unsigned LD = P->LoopDepth;
    bool NeedEscapeInnerLoop = false;
    for (unsigned Idx = 0; Idx < PN.getNumIncomingValues(); Idx++) {
      Value *PhiIn = PN.getIncomingValue(Idx);
      if (isEscapedValue(PhiIn)) {
        setEscaped(P, PN.getParent());
      }
      int Offset = 0;
      Value *BasePhiIn = getBaseValue(PhiIn, Offset);
      GCPtr *PhiInPtr = GCPtr::create(BasePhiIn, *this, LI);
      if (PhiInPtr == nullptr) {
        continue;
      }
      P->BaseValue[PhiInPtr].insert(Offset);
      PhiInPtr->Alias[P].insert(Offset);
      if (P->LoopDepth == 0 && PhiInPtr->LoopDepth == 0)
        continue;
      if (!isInSameLoop(P, PhiInPtr)) {
        if (LD > PhiInPtr->LoopDepth)
          LD = PhiInPtr->LoopDepth;
        NeedEscapeInnerLoop = true;
      }
    }
    if (NeedEscapeInnerLoop) {
      P->LoopDepth = LD;
      SmallSet<Value *, 8> VisitedInsts;
      SmallVector<Value *, 8> Phis;
      for (unsigned Idx = 0; Idx < PN.getNumIncomingValues(); Idx++) {
        Value *PhiIn = PN.getIncomingValue(Idx);
        int Offset = 0;
        Phis.push_back(PhiIn);
        VisitedInsts.insert(PhiIn);
        while (!Phis.empty()) {
          Value *CurVal = Phis.pop_back_val();
          Value *BasePhiIn = getBaseValue(CurVal, Offset);
          if (auto CurPN = dyn_cast<PHINode>(CurVal)) {
            for (unsigned I = 0; I < CurPN->getNumIncomingValues(); I++) {
              Value *CurPhiIn = CurPN->getIncomingValue(I);
              if (VisitedInsts.insert(CurPhiIn).second) {
                Phis.push_back(CurPhiIn);
              }
            }
          } else if (auto CurSI = dyn_cast<SelectInst>(CurVal)) {
            Value *BaseTrue = getBaseValue(CurSI->getTrueValue(), Offset);
            if (VisitedInsts.insert(BaseTrue).second) {
              Phis.push_back(BaseTrue);
            }
            Value *BaseFalse = getBaseValue(CurSI->getFalseValue(), Offset);
            if (VisitedInsts.insert(BaseFalse).second) {
              Phis.push_back(BaseFalse);
            }
          } else {
            BasePhiIn = getBaseValue(CurVal, Offset);
            GCPtr *PhiInPtr = GCPtr::create(BasePhiIn, *this, LI);
            // It is only possible to consider setting PhiInPtr to escape if it
            // is inside a loop.
            if (PhiInPtr != nullptr &&
                (LD < PhiInPtr->LoopDepth ||
                 (LD != 0 && isInSameLoop(P, PhiInPtr)))) {
              assert(P->LoopDepth <= PhiInPtr->LoopDepth);
              setEscaped(PhiInPtr, PN.getParent());
            }
            break;
          }
        }
      }
    }
  }

  void visitSelectInst(SelectInst &SI) {
    if (!SI.getType()->isPointerTy()) {
      return;
    }

    assert(SI.getType()->isPointerTy() && "selectInst.");
    GCPtr *P = GCPtr::create(&SI, *this, LI);
    if (isEscapedValue(SI.getTrueValue()) ||
        isEscapedValue(SI.getFalseValue())) {
      setEscaped(P, SI.getParent());
    }
    int Offset = 0;
    Value *BaseTrue = getBaseValue(SI.getTrueValue(), Offset);
    GCPtr *TruePtr = GCPtr::create(BaseTrue, *this, LI);
    if (TruePtr != nullptr) {
      P->BaseValue[TruePtr].insert(Offset);
      TruePtr->Alias[P].insert(Offset);
    }
    Value *BaseFalse = getBaseValue(SI.getFalseValue(), Offset);
    GCPtr *FalsePtr = GCPtr::create(BaseFalse, *this, LI);
    if (FalsePtr != nullptr) {
      P->BaseValue[FalsePtr].insert(Offset);
      FalsePtr->Alias[P].insert(Offset);
    }
  }

  void processLoadBase(GCPtr *DefValue, GCPtr *LoadPtr, BasicBlock *CurBB) {
    LoadPtr->BaseValue[DefValue].insert(0);
    if (DefValue->isPtr()) {
      DefValue->Alias[LoadPtr].insert(0);
      return;
    }
    MemPtr *MP = reinterpret_cast<MemPtr *>(DefValue);
    MP->insertDefine(CurBB, LoadPtr);
  }

  void handleLoad(Value *P, Instruction *V) {
    GCPtr *LoadPtr = GCPtr::create(V, *this, LI);
    LoadValues.insert(LoadPtr);
    BasicBlock *CurBB = V->getParent();
    if (isEscapedValue(P)) {
      setEscaped(LoadPtr, CurBB);
      return;
    }
    int SrcOffset = 0;
    Value *BaseSrc = getBaseValue(P, SrcOffset);
    if (auto EVT = dyn_cast<ExtractValueInst>(V)) {
      const DataLayout &DL = M->getDataLayout();
      SrcOffset = EVT->getIndexOffset(DL);
    }
    if (Def[CurBB][BaseSrc].count(-1)) {
      auto DefValueAny = *Def[CurBB][BaseSrc][-1].begin();
      processLoadBase(DefValueAny, LoadPtr, CurBB);
    }
    if (SrcOffset < 0 || Def[CurBB][BaseSrc][SrcOffset].size() == 0) {
      MemPtr *MP = MemPtr::create(BaseSrc, SrcOffset, *this, LI);
      if (MP) {
        LoadPtr->BaseValue[MP].insert(0);
        MP->insertDefine(CurBB, LoadPtr);
      }
      return;
    }
    auto DefValue =
        *Def[CurBB][BaseSrc][SrcOffset].begin();
    processLoadBase(DefValue, LoadPtr, CurBB);
  }

  void visitLoadInst(LoadInst &I) {
    if (!containsGCPtrType(I.getType())) {
      return;
    }

    if (isa<StructType>(I.getType())) {
      Value *V = I.getPointerOperand();
      handleMemcpyOrWriteAgg(&I, &I, V, nullptr);
      return;
    }
    auto Src = I.getPointerOperand();
    handleLoad(Src, &I);
  }

  void visitStoreInst(StoreInst &SI) {
    Value *V = SI.getValueOperand();
    if (!containsGCPtrType(V->getType()) || isValueInvalid(V)) {
      return;
    }
    if (isa<StructType>(V->getType())) {
      Value *P = SI.getPointerOperand();
      handleMemcpyOrWriteAgg(&SI, P, V, nullptr);
      return;
    }

    Value *P = SI.getPointerOperand();
    handleWrite(P, V, SI.getParent());
  }

  void visitMemCpyOrMove(Instruction &I) {
    Value *P = I.getOperand(0);
    Value *V = I.getOperand(1);
    ConstantInt *CSI = dyn_cast<ConstantInt>(I.getOperand(2));
    handleMemcpyOrWriteAgg(&I, P, V, CSI);
  }

  bool tryStackAllocateNewFinalizer(CallBase *CB) {
    Function *Callee = CB->getCalledFunction();
    if (!Callee || !isRewriteableCangjieMallocFunc(Callee))
      return false;
    GlobalVariable *Klass = getNewKlass(CB);
    if (Klass == nullptr) {
      setEscaped(GCPtr::create(CB, *this, LI), CB->getParent());
      return true;
    }
    if (isCangjieNewFinalizerFunc(Callee)) {
      LoopInfo *CurLI = &LookupLoopInfo(*CB->getParent()->getParent());
      if (!canStackAllocateFinalizer(CB, M, CurLI)) {
        setEscaped(GCPtr::create(CB, *this, LI), CB->getParent());
        return true;
      }
    }
    bool HasRefs = false;
    uint32_t AllocaSize = 0;
    bool NonMoveArray = false;
    Type *AllocaType =
        getAllocaType(Klass, HasRefs, AllocaSize, NonMoveArray, CB);
    if (AllocaType != nullptr || NonMoveArray) {
      MaxObjSize = std::max(MaxObjSize, AllocaSize);
      insertGCNew(CB);
    } else {
      setEscaped(GCPtr::create(CB, *this, LI), CB->getParent());
    }
    return true;
  }

  void visitInvokeInst(InvokeInst &II) {
    if (tryStackAllocateNewFinalizer(&II)) {
      return;
    }
    handleCallArgs(II);
  }

  void visitCallInst(CallInst &CI) {
    if (CI.getCalledFunction()) {
      if (handleCangjieSL(&CI)) {
        return;
      }
      if (tryStackAllocateNewFinalizer(&CI)) {
        return;
      }
    }
    if (const Function *F = CI.getCalledFunction()) {
      switch (F->getIntrinsicID()) {
      default:
        break;
      case Intrinsic::memcpy:
      case Intrinsic::memmove:
        visitMemCpyOrMove(CI);
        return;
      case Intrinsic::memset:
      case Intrinsic::lifetime_start:
      case Intrinsic::lifetime_end:
        return;
      }
    }
    handleCallArgs(CI);
  }

  // if not has any new. skip this func
  void visitReturnInst(ReturnInst &I) {
    if (I.getNumOperands() == 0) {
      return;
    }
    Value *Ret = I.getOperand(0);
    if (containsGCPtrType(Ret->getType())) {
      GCPtr *RetPtr = GCPtr::create(Ret, *this, LI);
      if (RetPtr != nullptr) {
        setEscaped(RetPtr, I.getParent());
      }
    }
  }

  void visitExtractValueInst(ExtractValueInst &EVT) {
    if (!containsGCPtrType(EVT.getType())) {
      return;
    }
    Value *V = EVT.getAggregateOperand();
    if (isa<StructType>(EVT.getType())) {
      handleMemcpyOrWriteAgg(&EVT, &EVT, V, nullptr);
      return;
    }
    handleLoad(V, &EVT);
  }

private:
  CallGraphUpdater &CGUpdater;

  Module *M;

  function_ref<LoopInfo &(Function(&))> LookupLoopInfo;

  LoopInfo *LI;

  SmallSetVector<Function *, 8> SCCFunctions;

  DenseMap<Value *, Value *> PtrToBaseMap;

  DenseMap<BasicBlock *, DenseMap<Value *, SmallSetVector<Value *, 8>>>
      StoreRelations;

  DenseMap<BasicBlock *, DenseMap<Value *, SmallSetVector<Value *, 8>>>
      CopyRelations;

  DenseMap<BasicBlock *, SmallSetVector<LoadInst *, 8>> LoadRelations;
  DenseMap<BasicBlock *, DenseMap<GCPtr *, SmallSetVector<GCPtr *, 8>>>
      AliasRelations;

  SmallSetVector<std::pair<Instruction *, BasicBlock*>, 8> GCNew;
  SmallVector<BasicBlock *, 8> BBs;
  SmallSetVector<BasicBlock *, 8> VisitedBBs;
  SmallSetVector<PHINode *, 8> VisitedPhis;
  DenseMap<Value *, unsigned> VisitedEscaped;
  SmallSetVector<Value *, 8> VisitedNew;
  SmallSetVector<Value *, 8> OneWayNotEscaped;
  DenseMap<Value *, SmallVector<SmallVector<BasicBlock *, 8>, 8>>
      PartialEscapeds;
  SmallSetVector<Value *, 8> OnceEscapedValue;
  // load alias find cycle. not process yet
  SmallSetVector<Value *, 8> LoadCycle;
  DenseMap<Value *, SmallSetVector<GCPtr *, 8>> LoadBaseValue;
  DenseMap<Value *, unsigned> LoadState;
  SmallSetVector<GCPtr *, 8> LoadValues;
  using StructRefOffsetsBySize =
      DenseMap<uint64_t, SmallVector<uint64_t, 8>>;
  DenseMap<StructType *, StructRefOffsetsBySize> StructRefOffsetsCache;

  using DefOffsetMap = DenseMap<int, SmallSetVector<GCPtr *, 8>>;
  using DefBaseMap = DenseMap<Value *, DefOffsetMap>;
  using DefBBMap = std::map<BasicBlock *, DefBaseMap>;
  DefBBMap Def;
  DefBBMap Out;
  DefBBMap In;
};

static bool skipLargeFunction(Function *F) {
  uint32_t AllocaCount = 0;
  for (const Instruction &I : F->getEntryBlock()) {
    if (isa<AllocaInst>(I)) {
      AllocaCount++;
    } else {
      continue; // alloc inst may be not continuous
    }
  }
  if (AllocaCount > MaxAllocaObj) {
    LLVM_DEBUG(dbgs() << "large alloca count:" << AllocaCount << "\n");
    return true;
  }
  return false;
}

static bool removeDeadBlocks(LazyCallGraph::SCC &C,
                             FunctionAnalysisManager &FAM) {
  bool Changed = false;
  for (auto &N : C) {
    auto &F = N.getFunction();
    Changed |= removeUnreachableBlocks(F);
  }
  return Changed;
}

static bool escapeAnalysisImpl(LazyCallGraph::SCC &C, LazyCallGraph &CG,
                               CGSCCAnalysisManager &AM,
                               CGSCCUpdateResult &UR) {
  FunctionAnalysisManager &FAM =
      AM.getResult<FunctionAnalysisManagerCGSCCProxy>(C, CG).getManager();
  auto LookLoop = [&](Function &F) -> LoopInfo & {
    return FAM.getResult<LoopAnalysis>(F);
  };
  CallGraphUpdater CGUpdater;
  CGUpdater.initialize(CG, C, AM, UR);
  Module &M = *C.begin()->getFunction().getParent();
  EADepthImpl EA(&CGUpdater, &M, LookLoop);
  for (LazyCallGraph::Node &N : C) {
    Function *F = &N.getFunction();
    if (!F->isDeclaration())
      EA.insertSCCFunctions(F);
  }
  EA.initialize();
  bool Changed = EA.run();
  bool Simplified = false;
  if (Changed) {
    SimplifyCFGOptions Options;
    Options.ConvertSwitchRangeToICmp = true;
    for (LazyCallGraph::Node &N : C) {
      Function *F = &N.getFunction();
      if (!F->isDeclaration()) {
        FAM.invalidate(*F, PreservedAnalyses::none());
        auto &TTI = FAM.getResult<TargetIRAnalysis>(*F);
        Simplified |= simplifyFunctionCFG(*F, TTI, nullptr, Options);
      }
    }
  }
  if (CJDisablePEA)
    return Changed;
  EscapeAnalysisImpl PEA(CGUpdater, &M, LookLoop);
  for (LazyCallGraph::Node &N : C) {
    Function *F = &N.getFunction();
    if (skipLargeFunction(F)) {
      continue;
    }
    if (Simplified) {
      FAM.invalidate(*F, PreservedAnalyses::none());
    }
    PEA.insertSCCFunction(F);
  }
  PEA.initialize();
  Changed |= PEA.run();
  // Dead Blocks may appear after PEA, which will cause the analysis of these
  // BBs to fail, such as LoopInfo. If these BBs are not deleted in a timely
  // manner, incorrect results may be generated during subsequent optimization
  // based on correct analysis.
  if (Changed)
    removeDeadBlocks(C, FAM);
  return Changed;
}

PreservedAnalyses CJPartialEscapeAnalysisPass::run(LazyCallGraph::SCC &C,
    CGSCCAnalysisManager &AM, LazyCallGraph &CG, CGSCCUpdateResult &UR) const {
  NonEscapeRewriter::clearRewritePhiIncoming();
  bool Changed = escapeAnalysisImpl(C, CG, AM, UR);
  if (Changed) {
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

namespace llvm {
bool escapeAnalysisFuncImpl(Function *F,
                            function_ref<LoopInfo &(Function &)> LookLoop) {
  EADepthImpl EA(nullptr, F->getParent(), LookLoop);
  EA.insertSCCFunctions(F);
  EA.initialize();
  return EA.run();
}
}
