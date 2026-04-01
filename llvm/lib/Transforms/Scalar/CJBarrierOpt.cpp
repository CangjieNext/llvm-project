//===- CJBarrierOpt.cpp - optimize cangjie write barriers -----------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This pass optimizes cangjie write barriers, including barrier split,
// barrier remove and barrier verifier.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJBarrierOpt.h"

#include "llvm/ADT/SetVector.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/SafepointIRVerifier.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/CJFillMetadata.h"
#include <cstdint>

using namespace llvm;

static cl::opt<unsigned> CJArrayThreshold(
    // 128 is cj-array-threshol default Initial Value
    "cj-array-threshold", cl::Hidden, cl::init(128 * 1024),
    cl::desc("When array size is bigger than cj-array-threshold "
             "and array element type is primitive type, "
             "its write barriers can be removed."));

const static DenseSet<StringRef> NewMallocArrayFunc {
    "CJ_MCC_NewArray", "CJ_MCC_NewArray8", "CJ_MCC_NewArray16",
    "CJ_MCC_NewArray32", "CJ_MCC_NewArray64", "CJ_MCC_NewObjArray"
};

const static DenseSet<StringRef> NewMallocObjFunc {
    "CJ_MCC_NewFinalizer", "CJ_MCC_NewObject",
    "CJ_MCC_NewPinnedObject"};

static bool isNewMallocCall(CallBase *CB) {
  if (CB->getCalledFunction() == nullptr) {
    return false;
  }
  return NewMallocArrayFunc.find(CB->getCalledFunction()->getName()) !=
             NewMallocArrayFunc.end() ||
         NewMallocObjFunc.find(CB->getCalledFunction()->getName()) !=
             NewMallocObjFunc.end();
}

class BarrierNeed {
public:
  explicit BarrierNeed(Instruction *Begin, Instruction *End, LoopInfo *LI,
                       DenseMap<BasicBlock *, bool> &BBSafepointMaps)
      : Begin(Begin), End(End), BeginLoop(LI->getLoopFor(Begin->getParent())),
        EndLoop(LI->getLoopFor(End->getParent())), LI(LI),
        BBSafepointMaps(BBSafepointMaps) {
    SameParentLoop = getSameParentLoop(BeginLoop);
  };
  ~BarrierNeed() = default;

  bool run() {
    if (isBeginSingleLoop()) {
      return true;
    }
    SmallVector<BasicBlock *> LoopLatches;
    if (SameParentLoop != nullptr) {
      SameParentLoop->getLoopLatches(LoopLatches);
    }
    Worklist.insert(Begin->getParent());
    BasicBlock *EndBB = End->getParent();
    while (!Worklist.empty()) {
      BasicBlock *BB = Worklist.pop_back_val();
      if (BBSafepointFlow.count(EndBB) > 0) {
        return BBSafepointFlow[EndBB];
      }
      if (BBSafepointFlow.count(BB) > 0) {
        continue;
      }
      if ((BB == End->getParent()) && (scanEndBB())) {
        return BBSafepointFlow[EndBB];
      } else if (BB == Begin->getParent()) {
        scanBeginBB();
      } else {
        if (!calculateSafepoint(BB, BBSafepointMaps[BB])) {
          continue;
        }
      }
      if (SameParentLoop != nullptr && isLoopLatch(BB, LoopLatches)) {
        continue;
      }
      scanSuccPath(BB);
    }
    return (BBSafepointFlow.find(End->getParent()) == BBSafepointFlow.end());
  }

private:
  Instruction *Begin;
  Instruction *End;
  Loop *BeginLoop;
  Loop *EndLoop;
  Loop *SameParentLoop;
  LoopInfo *LI;
  SetVector<BasicBlock *> Worklist;
  // Calculate if path have a safepoint.
  // If this BB has safepoint depends on himself and its predecessors in path.
  DenseMap<BasicBlock *, bool> BBSafepointFlow;
  DenseMap<BasicBlock *, bool> BBSafepointMaps;

  // Judge if Begin is in a loop alone or Beginloop is a subloop of Endloop.
  // In these cases, we need to judge if Beginloop has safepoint.
  bool isBeginSingleLoop() {
    // Begin is not in a loop.
    if (BeginLoop == nullptr) {
      return false;
    }
    // Begin is in a loop, End is not in a loop.
    if (EndLoop == nullptr) {
      return true;
    }
    // Endloop is a subloop of Beginloop.
    if (BeginLoop->contains(EndLoop)) {
      return false;
    }
    // Beginloop is subloop of Endloop.
    // Or Beginloop and Endloop are two completely unrelated loops.
    return true;
  }

  // Judge if BeginLoop and EndLoop are in a same parent loop.
  // In this case, we only need to scan the parent loop,
  // and when we meet the looplatch of the parent loop, break.
  Loop *getSameParentLoop(Loop *L) {
    if (L == nullptr) {
      return nullptr;
    }
    if (L->contains(EndLoop)) {
      return L;
    }
    return getSameParentLoop(L->getParentLoop());
  }

  // Judge if we have deal with this BB or this BB is in path to End.
  bool isNotDealPathSucc(BasicBlock *BB) {
    BasicBlock *EndBB = End->getParent();
    if (BB->isLandingPad()) {
      return false;
    }
    if (Worklist.contains(BB)) {
      return false;
    }
    if (BBSafepointFlow.count(BB) > 0) {
      return false;
    }
    return isPotentiallyReachable(BB, EndBB);
  }

  void scanSuccPath(BasicBlock *BB) {
    for (auto Succ : successors(BB)) {
      if (!isNotDealPathSucc(Succ)) {
        continue;
      }
      Loop *SuccLoop = LI->getLoopFor(Succ);
      // Judge if Succ is a node of the same big loop.
      // If Succ is a node of the same big loop, Succ is a node of path.
      // Otherwise, SuccLoop is a loop node of path.
      if ((SuccLoop != nullptr && SameParentLoop != nullptr &&
           SuccLoop != SameParentLoop) ||
          (SuccLoop != nullptr && SameParentLoop == nullptr)) {
        scanPathLoop(SuccLoop);
        continue;
      }
      Worklist.insert(Succ);
    }
  }

  void scanBeginBB() {
    bool IsSafepoint = false;
    BasicBlock *BeginBB = Begin->getParent();
    for (auto It = Begin->getIterator(), E = BeginBB->end(); It != E; ++It) {
      if (auto CB = dyn_cast<CallBase>(&*It)) {
        if (isSafepointCall(CB)) {
          IsSafepoint = true;
        }
      }
      if (It->isTerminator()) {
        calculateSafepoint(BeginBB, IsSafepoint);
      }
    }
  }

  bool scanEndBB() {
    bool IsSafepoint = false;
    BasicBlock *BeginBB = Begin->getParent();
    BasicBlock *EndBB = End->getParent();
    for (auto It = EndBB == BeginBB ? Begin->getIterator() : EndBB->begin(),
              E = EndBB->end();
         It != E; ++It) {
      if (It == End->getIterator()) {
        return calculateSafepoint(End->getParent(), IsSafepoint);
      }
      if (auto CB = dyn_cast<CallBase>(&*It)) {
        if (isSafepointCall(CB)) {
          IsSafepoint = true;
        }
      }
    }
    return false;
  }

  void scanPathLoop(Loop *CurLoop) {
    bool IsSafepointLoop = false;
    for (auto BB : CurLoop->getBlocks()) {
      IsSafepointLoop |= BBSafepointMaps[BB];
    }
    scanPreForSafepoint(CurLoop->getHeader(), IsSafepointLoop, CurLoop);
    for (auto BB : CurLoop->getBlocks()) {
      BBSafepointFlow[BB] = IsSafepointLoop;
      Worklist.insert(BB);
    }
    SmallVector<BasicBlock *> ExitBlocks;
    CurLoop->getExitBlocks(ExitBlocks);
    for (auto Exit : ExitBlocks) {
      if (!isNotDealPathSucc(Exit)) {
        continue;
      }
      Worklist.insert(Exit);
    }
  }

  bool calculateSafepoint(BasicBlock *BB, bool &IsSafepoint) {
    BasicBlock *BeginBB = Begin->getParent();
    if (BB == BeginBB) {
      BBSafepointFlow[BB] = IsSafepoint;
      return true;
    }
    if (!scanPreForSafepoint(BB, IsSafepoint, nullptr)) {
      return false;
    }
    BBSafepointFlow[BB] = IsSafepoint;
    return true;
  }

  bool scanPreForSafepoint(BasicBlock *BB, bool &IsSafepoint, Loop *L) {
    BasicBlock *BeginBB = Begin->getParent();
    for (auto Pre : predecessors(BB)) {
      if (L != nullptr && L->contains(Pre)) {
        continue;
      }
      if (!isPotentiallyReachable(BeginBB, Pre)) {
        continue;
      }
      if (BBSafepointFlow.find(Pre) == BBSafepointFlow.end()) {
        return false;
      }
      IsSafepoint |= BBSafepointFlow[Pre];
    }
    return true;
  }

  bool isLoopLatch(BasicBlock *BB, SmallVector<BasicBlock *> &LoopLatches) {
    for (auto LoopLatch : LoopLatches) {
      if (BB == LoopLatch) {
        return true;
      }
    }
    return false;
  }
};

class BarriersCheck {
public:
  BarriersCheck(Function *F, LoopInfo *LI) : F(F), LI(LI),
      DL(F->getParent()->getDataLayout()) {};
  ~BarriersCheck() = default;

  bool run() {
    bool Changed = false;
    initData();

    for (auto SI : StoreSet) {
      Type *Ty = SI->getValueOperand()->stripPointerCasts()->getType();
      if (!isGCPointerType(Ty))
        continue;

      if (barriersCheckFail(SI->getPointerOperand(), SI)) {
        report_fatal_error("Need write barrier in function: " +
                           Twine(F->getName()) + "!!!");
      }
    }

    for (auto Mem : MemSet) {
      if (!verifierMemcpy(Mem)) {
        report_fatal_error("Need write barrier in function: " +
                           Twine(F->getName()) + "!!!");
      }
    }

    for (auto Barrier : BarrierSet) {
      if (optStructBarrier(Barrier) || optStackBarrier(Barrier)) {
        Changed = true;
        continue;
      }
    }

    return Changed;
  }

private:
  Function *F;
  LoopInfo *LI;
  const DataLayout &DL;
  // Record gcwrite and gcwrite.agg
  DenseSet<CallBase *> BarrierSet;
  // Record store insts
  DenseSet<StoreInst *> StoreSet;
  // Record memcpy and memmove insts
  DenseSet<CallBase *> MemSet;
  // Record defs and users
  DenseMap<Value *, SetVector<Value *>> UsesMaps;
  // Record if there is safepoint in each BB
  DenseMap<BasicBlock *, bool> BBSafepointMaps;

  bool verifierMemcpy(CallBase *Mem) {
    auto GetBeginOffset = [&](Value *V, uint64_t &BeginPos) {
      APInt Offset(DL.getIndexSizeInBits(0u), 0);
      Value *Base = V->stripAndAccumulateConstantOffsets(DL, Offset, true);
      BeginPos = Offset.getZExtValue();
      StructType *ST = dyn_cast<StructType>(
          Base->getType()->getNonOpaquePointerElementType());
      return ST;
    };

    Value *Dst = Mem->getArgOperand(0);
    Value *Src = Mem->getArgOperand(1);
    StructType *ST = nullptr;
    uint64_t BeginPos;
    if (StructType *DstST = GetBeginOffset(Dst, BeginPos)) {
      ST = DstST;
    } else if (StructType *SrcST = GetBeginOffset(Src, BeginPos)) {
      ST = SrcST;
    } else {
      return true;
    }
    uint64_t EndPos;
    if (auto Constant = dyn_cast<ConstantInt>(Mem->getArgOperand(2))) {
      EndPos = BeginPos + Constant->getZExtValue();
    } else {
      EndPos = DL.getStructLayout(ST)->getSizeInBytes();
    }
    if (!containsRefInStruct(ST, BeginPos, EndPos)) {
      return true;
    }
    return !barriersCheckFail(Mem->getArgOperand(0), Mem);
  }

  bool isStackBarrier(CallBase *Barrier) {
    switch (Barrier->getIntrinsicID()) {
    case Intrinsic::cj_gcwrite_ref:
    case Intrinsic::cj_gcread_struct:
      return isa<AllocaInst>(Barrier->getArgOperand(1)->stripPointerCasts());
    case Intrinsic::cj_gcwrite_struct:
    case Intrinsic::cj_gcread_ref:
      return isa<AllocaInst>(Barrier->getArgOperand(0)->stripPointerCasts());
    case Intrinsic::cj_array_copy_ref:
    case Intrinsic::cj_array_copy_struct:
      return isa<AllocaInst>(Barrier->getArgOperand(0)->stripPointerCasts()) &&
             isa<AllocaInst>(Barrier->getArgOperand(2)->stripPointerCasts());
    default:
      return false;
    }
  }

  bool optStackBarrier(CallBase *Barrier) {
    if (!isStackBarrier(Barrier))
      return false;
    replaceBarrier(Barrier);
    return true;
  }

  void replaceBarrier(CallBase *Barrier) {
    IRBuilder<> Builder(Barrier);
    Builder.SetInsertPoint(Barrier);
    createStoreOrMems(Barrier, Builder);
    Barrier->eraseFromParent();
  }

  bool argumentCheck(Function *Callee, unsigned ArgNo,
                     DenseSet<std::pair<CallBase *, unsigned>> &CallSets,
                     const std::function<bool(Value *)> &Check) {
    for (Use &U : Callee->uses()) {
      CallBase *CB = dyn_cast<CallBase>(U.getUser());
      // Must be a direct call.
      if (CB == nullptr || !CB->isCallee(&U) ||
          CB->getFunctionType() != F->getFunctionType())
        return false;

      if (CallSets.contains({CB, ArgNo})) {
        continue;
      }
      CallSets.insert({CB, ArgNo});
      if (Check(CB->getArgOperand(ArgNo)))
        continue;

      auto Arg =
          dyn_cast<Argument>(findMemoryBasePointer(CB->getArgOperand(ArgNo)));
      if (Arg == nullptr || !CB->getFunction()->hasLocalLinkage()) {
        return false;
      }
      unsigned CallerArgNo = Arg->getArgNo();
      if (argumentCheck(CB->getFunction(), CallerArgNo, CallSets, Check)) {
        continue;
      }
      return false;
    }
    return true;
  }

  std::pair<Value *, Value *> getBaseAndFieldPtrByBarrier(CallBase *Barrier) {
    Value *BasePtr = nullptr;
    Value *FieldPtr = nullptr;
    switch (Barrier->getIntrinsicID()) {
    default:
      report_fatal_error("Unsupported Cangjie Barrier Intrinsic!");
      break;
    // Value, BaseObj, FieldPtr
    case Intrinsic::cj_gcwrite_ref:
    // DstPtr, BaseObj, SrcPtr, Size
    case Intrinsic::cj_gcread_struct:
      BasePtr = Barrier->getArgOperand(1);
      FieldPtr = Barrier->getArgOperand(2);
      break;
    // BaseObj, FieldPtr
    case Intrinsic::cj_gcread_ref:
    // BaseObj, DstPtr, SrcPtr, Size
    case Intrinsic::cj_gcwrite_struct:
      BasePtr = Barrier->getArgOperand(0);
      FieldPtr = Barrier->getArgOperand(1);
      break;
    // DstBaseObj, DstFieldPtr, SrcBaseObj, SrcFieldPtr, Size
    case Intrinsic::cj_array_copy_ref:
    case Intrinsic::cj_array_copy_struct:
      BasePtr = Barrier->getArgOperand(0);
      break;
    }
    return {getUnderlyingObject(BasePtr),
            FieldPtr ? getUnderlyingObject(FieldPtr) : nullptr};
  }

  bool optStructBarrier(CallBase *Barrier) {
    auto PtrCheck = [this](Value *Ptr,
                           const std::function<bool(Value *)> &Check) {
      if (Check(Ptr))
        return true;

      // Ptr is argument, check the parameters transferred by the caller.
      auto *Arg = dyn_cast<Argument>(findMemoryBasePointer(Ptr));
      if (Arg == nullptr || !F->hasLocalLinkage())
        return false;
      DenseSet<std::pair<CallBase *, unsigned>> CallSets;
      if (!argumentCheck(F, Arg->getArgNo(), CallSets, Check)) {
        return false;
      }
      return true;
    };

    auto [BasePtr, FieldPtr] = getBaseAndFieldPtrByBarrier(Barrier);
    if (BasePtr && FieldPtr) {
      bool IsBaseNull = PtrCheck(BasePtr, [](Value *Ptr) {
        Value *BP = Ptr->stripPointerCasts();
        auto *Const = dyn_cast<Constant>(BP);
        if (Const != nullptr && Const->isNullValue())
          return true;
        return false;
      });
      bool IsFieldNotGV = PtrCheck(FieldPtr, [](Value *Ptr) {
        Value *BP = Ptr->stripPointerCasts();
        return !isa<GlobalValue>(BP) && !isa<Argument>(BP);
      });
      // gcread/gcwrite to static pointer cannot be replace, even if base is
      // nullptr.
      if (IsBaseNull && IsFieldNotGV) {
        replaceBarrier(Barrier);
        return true;
      }
    }

    if (Barrier->getIntrinsicID() == Intrinsic::cj_gcwrite_struct) {
      auto Src = Barrier->getArgOperand(2)->stripPointerCasts();
      // 64 is 64-bit integer
      APInt Offset(DL.getIndexSizeInBits(0u), -1);
      StructType *ST = nullptr;
      auto Size = dyn_cast<ConstantInt>(Barrier->getArgOperand(3));
      if (auto AI = dyn_cast<AllocaInst>(Src)) {
        Offset = 0;
        ST = dyn_cast<StructType>(AI->getAllocatedType());
      } else if (auto GEP = dyn_cast<GetElementPtrInst>(Src)) {
        if (!GEP->accumulateConstantOffset(DL, Offset))
          Offset = -1;
        ST = dyn_cast<StructType>(GEP->getPointerOperand()
                                      ->getType()
                                      ->getNonOpaquePointerElementType());
      } else if (auto Arg = dyn_cast<Argument>(Src)) {
        Offset = 0;
        ST = dyn_cast<StructType>(
            Arg->getType()->getNonOpaquePointerElementType());
      }
      if (Offset != -1 && ST != nullptr && Size != nullptr &&
          !containsRefInStruct(ST, Offset.getSExtValue(),
                               Offset.getSExtValue() + Size->getSExtValue())) {
        replaceBarrier(Barrier);
        return true;
      }
    }

    return false;
  }

  // Normally, we need to scan from the next node of CB.
  // When CB is terminator, it does not have next node,
  // so we need to scan from the begin of next BB.
  Instruction *getNextNode(Instruction *Origin) {
    if (Origin->isTerminator()) {
      auto Invoke = dyn_cast<InvokeInst>(Origin);
      assert(Invoke != nullptr && "Call can not be terminator!");
      // When the base is invoke, scan from normal BB.
      return (&*Invoke->getNormalDest()->begin());
    } else {
      // Normally, scan from the next node.
      return (Origin->getNextNode());
    }
  }

  void fillUses(SetVector<Value *> &Uses, Value *Val) {
    Uses.insert(Val);
    for (unsigned It = 0; It < Uses.size(); ++It) {
      Value *V = Uses[It];
      Uses.insert(V);
      for (auto Use : V->users()) {
        auto I = dyn_cast<Instruction>(Use);
        switch (I->getOpcode()) {
        default:
          break;
        case Instruction::BitCast:
        case Instruction::AddrSpaceCast:
        case Instruction::GetElementPtr:
        case Instruction::PHI:
        case Instruction::Select:
          Uses.insert(I);
          break;
        }
      }
    }
  }

  void initData() {
    for (auto &Arg : F->args()) {
      fillUses(UsesMaps[&Arg], &Arg);
    }

    for (auto &BB : *F) {
      bool IsSafepointBB = false;
      for (auto &I : BB) {
        if (isa<LoadInst>(&I) && containsGCPtrType(I.getType())) {
          fillUses(UsesMaps[&I], &I);
        }
        if (auto SI = dyn_cast<StoreInst>(&I)) {
          StoreSet.insert(SI);
        }
        CallBase *CB = dyn_cast<CallBase>(&I);
        if (CB == nullptr) {
          continue;
        }
        if (isSafepointCall(CB)) {
          IsSafepointBB = true;
        }

        switch (CB->getIntrinsicID()) {
        default:
          break;
        case Intrinsic::cj_gcwrite_ref:
        case Intrinsic::cj_gcwrite_struct:
        case Intrinsic::cj_gcread_ref:
        case Intrinsic::cj_gcread_struct:
        case Intrinsic::cj_array_copy_ref:
        case Intrinsic::cj_array_copy_struct:
          BarrierSet.insert(CB);
          continue;
        case Intrinsic::memcpy:
        case Intrinsic::memmove:
          MemSet.insert(CB);
          continue;
        }

        if (!containsGCPtrType(CB->getType()) &&
            !isMemoryContainsGCPtrType(CB->getType())) {
          continue;
        }
        fillUses(UsesMaps[CB], CB);
      }
      BBSafepointMaps[&BB] = IsSafepointBB;
    }
  }

  bool containsRefInArray(ArrayType *AT, uint64_t Start, uint64_t End,
                          uint64_t CurPos) {
    bool Result = false;
    Type *ElementTy = AT->getElementType();
    if (isGCPointerType(ElementTy)) {
      return true;
    } else if (auto AST = dyn_cast<StructType>(ElementTy)) {
      for (unsigned Idx = 0; Idx < AT->getNumElements(); Idx++) {
        Result |= containsRefInStruct(AST, Start, End, CurPos);
      }
    }
    return Result;
  }

  bool containsRefInStruct(StructType *ST, uint64_t Start, uint64_t End,
                           uint64_t CurPos = 0) {
    const StructLayout *Layout = DL.getStructLayout(ST);
    bool Result = false;
    for (uint32_t STNum = 0; STNum < ST->getNumElements(); STNum++) {
      uint64_t Pos = Layout->getElementOffset(STNum) + CurPos;
      if (Pos < Start)
        continue;

      if (Pos >= End)
        return Result;

      auto ET = ST->getElementType(STNum);
      if (isGCPointerType(ET)) {
        return true;
      } else if (auto EST = dyn_cast<StructType>(ET)) {
        Result |= containsRefInStruct(EST, Start, End, Pos);
      } else if (auto EAT = dyn_cast<ArrayType>(ET)) {
        Result |= containsRefInArray(EAT, Start, End, Pos);
      }
    }
    return Result;
  }

  bool tryCheckMemContainsRef(MemIntrinsic *Mem) {
    auto SizeOp = dyn_cast<ConstantInt>(Mem->getLength());
    if (!SizeOp)
      return false;

    uint64_t Size = SizeOp->getZExtValue();
    Value *Src = Mem->getArgOperand(1)->stripPointerCasts();
    Value *Dst = Mem->getArgOperand(0)->stripPointerCasts();
    Type *SrcTy = Src->getType()->getNonOpaquePointerElementType();
    Type *DstTy = Dst->getType()->getNonOpaquePointerElementType();
    if (SrcTy->isStructTy() &&
        containsRefInStruct(cast<StructType>(SrcTy), 0, Size))
      return true;

    if (DstTy->isStructTy() &&
        containsRefInStruct(cast<StructType>(DstTy), 0, Size))
      return true;

    APInt Offset(DL.getIndexSizeInBits(0), 0);
    Value *PtrBase = Src->stripAndAccumulateConstantOffsets(DL, Offset, false);
    uint64_t Off = Offset.getZExtValue();
    uint64_t End = Off + Size;
    Type *BaseTy = PtrBase->getType()->getNonOpaquePointerElementType();
    if (BaseTy->isStructTy() &&
        containsRefInStruct(cast<StructType>(BaseTy), Off, End))
      return true;

    return false;
  }

  // Returns true when should be a barrier, false otherwise. Note that you need
  // to check whether the assignment instruction contains a ref.
  bool barriersCheckFail(Value *Op, Instruction *Cur) {
    SetVector<Value *> Origins;
    SetVector<Value *> Cache;
    // When the base is not gc pointer, do not need write barrier
    if (!isGCPointerOrGlobal(Op, Origins, Cache)) {
      return false;
    }

    if (isa<StoreInst>(Cur)) {
      // When base is global or gcpointer, always need write barrier.
      return true;
    }

    MemIntrinsic *MI = dyn_cast<MemIntrinsic>(Cur);
    assert(MI && "It should be a memory instruction.");
    bool ContainsRef = tryCheckMemContainsRef(MI);
    if (!ContainsRef) {
      return false;
    }

    bool NeedBarrier = false;
    for (auto Origin : Origins) {
      if (isa<AllocaInst>(Origin))
        continue;
      auto *CB = dyn_cast<CallBase>(Origin);
      if (!CB || !isNewMallocCall(CB))
        return true;

      Instruction *Begin = getNextNode(CB);
      BarrierNeed BN(Begin, Cur, LI, BBSafepointMaps);
      NeedBarrier |= BN.run();
    }

    return NeedBarrier;
  }

  bool isGCPointerOrGlobal(Value *Val, SetVector<Value *> &Origins,
                           SetVector<Value *> &Cache) {
    assert(Val != nullptr && "Illegal to ask for the type of nullptr");
    assert(Val->getType()->isPointerTy() && "should be a pointer type");

    bool Result = false;
    Val = Val->stripPointerCasts();
    if (Cache.contains(Val))
      return Result;

    Cache.insert(Val);
    if (auto GEP = dyn_cast<GetElementPtrInst>(Val)) {
      Result |= isGCPointerOrGlobal(GEP->getPointerOperand(), Origins, Cache);
    } else if (auto PI = dyn_cast<PHINode>(Val)) {
      for (Value *PIVal : PI->incoming_values()) {
        Result |= isGCPointerOrGlobal(PIVal, Origins, Cache);
      }
    } else if (auto SI = dyn_cast<SelectInst>(Val)) {
      Result |= isGCPointerOrGlobal(SI->getTrueValue(), Origins, Cache);
      Result |= isGCPointerOrGlobal(SI->getFalseValue(), Origins, Cache);
    } else {
      Result |= (isInt8AS1Pty(Val->getType()) || isa<GlobalVariable>(Val));
      Origins.insert(Val);
    }

    return Result;
  }
};

static bool optCJBarrierModule(Module &M,
                               function_ref<LoopInfo &(Function &)> GetLI) {
  bool Changed = false;

  for (Function &F : M) {
    if (F.isDeclaration()) {
      continue;
    }
    if (F.hasCangjieGC()) {
      auto &LI = GetLI(F);
      BarriersCheck BC(&F, &LI);
      Changed |= BC.run();
    }
  }
  return Changed;
}

PreservedAnalyses CJBarrierOpt::run(Module &M, ModuleAnalysisManager &AM) const {
  FunctionAnalysisManager &FAM =
      AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  auto GetLoopInfo = [&FAM](Function &F) -> LoopInfo & {
    return FAM.getResult<LoopAnalysis>(F);
  };
  if (optCJBarrierModule(M, GetLoopInfo)) {
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

namespace {
class CJBarrierOptLegacyPass : public ModulePass {
public:
  static char ID;

  explicit CJBarrierOptLegacyPass() : ModulePass(ID) {
    initializeCJBarrierOptLegacyPassPass(
        *PassRegistry::getPassRegistry());
  }
  ~CJBarrierOptLegacyPass() = default;

  bool runOnModule(Module &M) override {
    bool Changed = false;
    auto GetLoopInfo = [this, &Changed](Function &F) -> LoopInfo & {
      return this->getAnalysis<LoopInfoWrapperPass>(F, &Changed).getLoopInfo();
    };
    return optCJBarrierModule(M, GetLoopInfo);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addPreserved<LoopInfoWrapperPass>();
  }
};
} // namespace

char CJBarrierOptLegacyPass::ID = 0;

ModulePass *llvm::createCJBarrierOptLegacyPass() {
  return new CJBarrierOptLegacyPass();
}

INITIALIZE_PASS_BEGIN(CJBarrierOptLegacyPass, "cj-barrier-opt",
                      "CJ Barrier Opt", false, false)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_END(CJBarrierOptLegacyPass, "cj-barrier-opt",
                    "CJ Barrier Opt", false, false)
