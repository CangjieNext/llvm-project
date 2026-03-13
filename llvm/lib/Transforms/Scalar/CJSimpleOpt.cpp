//===- CJSimpleOpt.cpp - Simple opt of cangjie -------------------*- C++-*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Some simple optimization points of cangjie
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJSimpleOpt.h"

#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/SafepointIRVerifier.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Scalar.h"

using namespace llvm;

static cl::opt<bool> CJCallRetOpt("cj-callret-opt", cl::Hidden, cl::init(true));
static cl::opt<bool> CJTsanSupport("cj-tsan-support-for-mutexopt", cl::Hidden,
                                   cl::init(false));

// Call ret opt
struct CallRetState {
  Function *F;
  PostDominatorTree &PDT;
  CallRetState(Function *F, PostDominatorTree &PDT) : F(F), PDT(PDT) {}

  class CallRetUnit {
  public:
    CallRetUnit(CallBase *CB, AllocaInst *CallRet, CallBase *CJMemset,
                CallBase *Memcpy, Value *DST, CallBase *DstCJMemset)
        : CB(CB), CallRet(CallRet), CJMemset(CJMemset), Memcpy(Memcpy),
          DST(DST), DstCJMemset(DstCJMemset){};
    ~CallRetUnit() = default;

    // The call of sret
    CallBase *CB;
    // Sret of the call
    AllocaInst *CallRet;
    // CJ memset of the sret if it exists
    CallBase *CJMemset;
    // Memcpy from sret to dst
    CallBase *Memcpy;
    // Dst of memcpy
    Value *DST;
    // Dst CJ memset if it exists
    CallBase *DstCJMemset;
  };

  // Scan all users of sret. When sret is only used as an intermediate
  // local variable of an assignment operation, it can be optimized.
  bool scanCallRetUses(CallRetUnit &CRU, Instruction *I,
                       SetVector<Instruction *> &Uses) {
    for (auto *Use : I->users()) {
      auto *Inst = dyn_cast<Instruction>(Use);
      if (Inst == nullptr)
        return false;
      if (Inst == CRU.CB || Uses.contains(Inst))
        continue;
      Uses.insert(Inst);
      if (isa<BitCastInst>(Inst) || isa<AddrSpaceCastInst>(Inst)) {
        if (scanCallRetUses(CRU, Inst, Uses))
          continue;
        return false;
      }
      auto *CB = dyn_cast<CallBase>(Inst);
      if (CB == nullptr) {
        return false;
      }
      if (CB->getIntrinsicID() != Intrinsic::cj_memset &&
          CB->getIntrinsicID() != Intrinsic::memcpy) {
        return false;
      }
      if (CB->getIntrinsicID() == Intrinsic::cj_memset) {
        if (CRU.CJMemset == nullptr) {
          CRU.CJMemset = CB;
          continue;
        }
        return false;
      }
      if (CB->getArgOperand(1)->stripPointerCasts() == CRU.CallRet &&
          CRU.Memcpy == nullptr) {
        CRU.Memcpy = CB;
        CRU.DST = CB->getArgOperand(0)->stripPointerCasts();
        continue;
      }
      return false;
    }
    return true;
  }

  void getDstUses(Value *Val, SetVector<Value *> &DstUses) {
    for (auto *Use : Val->users()) {
      auto *V = dyn_cast<Value>(Use);
      if (DstUses.contains(V))
        continue;
      DstUses.insert(V);
      getDstUses(V, DstUses);
    }
  }

  bool optCJCallRet() {
    if (!CJCallRetOpt) {
      return false;
    }
    bool Changed = false;
    SetVector<CallBase *> Memsets;
    SetVector<Instruction *> FuncInsts;
    for (auto I = inst_begin(F); I != inst_end(F);) {
      FuncInsts.insert(&*I++);
    }
    for (auto *FuncInst : FuncInsts) {
      auto *CB = dyn_cast<CallBase>(FuncInst);
      if (CB == nullptr ||
          CB->getArgOperandWithAttribute(Attribute::StructRet) == nullptr)
        continue;
      auto *CallRet =
          dyn_cast<AllocaInst>(CB->getArgOperand(0)->stripPointerCasts());
      if (CallRet == nullptr)
        continue;
      CallRetUnit CRU(CB, CallRet, nullptr, nullptr, nullptr, nullptr);
      SetVector<Instruction *> Uses;
      if (!scanCallRetUses(CRU, CallRet, Uses) || CRU.Memcpy == nullptr ||
          !isPotentiallyReachable(CRU.CB, CRU.Memcpy)) {
        continue;
      }
      if (!isa<AllocaInst>(CRU.DST) && !isa<Argument>(CRU.DST) &&
          !isa<GetElementPtrInst>(CRU.DST)) {
        continue;
      }
      SetVector<Value *> DstUses;
      auto *GEP = dyn_cast<GetElementPtrInst>(CRU.DST);
      if (GEP != nullptr) {
        Value *Src = GEP->getPointerOperand()->stripPointerCasts();
        if (!isa<AllocaInst>(Src) && !isa<Argument>(Src)) {
          continue;
        }
        getDstUses(Src, DstUses);
      } else {
        getDstUses(CRU.DST, DstUses);
      }

      if (!scanDstUses(CRU, DstUses) ||
          CRU.DST->getType() != CRU.CallRet->getType()) {
        continue;
      }
      Changed |= replaceCJCallRet(CRU, GEP, Memsets);
    }
    for (auto *Memset : Memsets) {
      Memset->eraseFromParent();
    }
    return Changed;
  }

  // Scan all DST users and check whether any other operations
  // are performed on DST between call and memcpy.
  bool scanDstUses(CallRetUnit &CRU, SetVector<Value *> &DstUses) {
    for (auto *Use : DstUses) {
      auto *I = dyn_cast<Instruction>(Use);
      if (I == nullptr || I == CRU.Memcpy || I == CRU.DST)
        continue;
      if (isPotentiallyReachable(CRU.CB, I) &&
          isPotentiallyReachable(I, CRU.Memcpy)) {
        if (isa<BitCastInst>(I) || isa<AddrSpaceCastInst>(I))
          continue;
        auto *CB = dyn_cast<CallBase>(I);
        if (CB == nullptr)
          return false;
        if (CB->getIntrinsicID() == Intrinsic::cj_memset &&
            CRU.DstCJMemset == nullptr) {
          CRU.DstCJMemset = CB;
          continue;
        }
        return false;
      }
    }
    return true;
  }

  bool replaceCJCallRet(CallRetUnit &CRU, GetElementPtrInst *GEP,
                        SetVector<CallBase *> &Memsets) {
    if (GEP != nullptr) {
      if (CRU.CJMemset == nullptr) {
        auto *CallRetWithoutCast =
            dyn_cast<Instruction>(CRU.CB->getArgOperand(0));
        if (CallRetWithoutCast == CRU.CallRet) {
          GEP->moveBefore(CRU.CB);
        } else {
          GEP->moveBefore(CallRetWithoutCast);
        }
      } else
        return false;
    }
    if (CRU.CJMemset != nullptr && !PDT.dominates(CRU.CB, CRU.CJMemset))
      return false;
    if (isa<Argument>(CRU.DST) && CRU.CJMemset != nullptr)
      Memsets.insert(CRU.CJMemset);
    if (CRU.DstCJMemset != nullptr)
      Memsets.insert(CRU.DstCJMemset);
    CRU.CallRet->replaceAllUsesWith(CRU.DST);
    return true;
  }
};

// FPToSI Optimization
//
//             fadd
//             xxx
//             bitcast
//             and1
//             cmp1
//        /            \
//  inf.or.nan     not.inf.nan
//  overflow      /           \
//            overflow   lower.bound.ok
//                       /             \
//                  overflow      upper.bound.ok
//                                    fptosi
// to
//
//               fadd
//               xxx
//               fptosi
//               call get.fp.state
//               and2
//               cmp2
//        /                \
// upper.bound.ok  fp.convert.exception
//                  call reset.fp.state
//                      bitcast
//                        and1
//                        cmp1
//                   /            \
//              inf.or.nan    not.inf.nan
//              overflow      /         \
//                        overflow   lower.bound.ok
//                                    /          \
//                                overflow  upper.bound.ok
struct FPToSIState {
  static constexpr char FPExceptionBBName[] = "fp.convert.exception";
  // Float value is Inf or Nan in IEEE754.
  static constexpr uint64_t DOUBLE_INF_OR_NAN = 0x7FF0000000000000;
  static constexpr uint64_t FLOAT_INF_OR_NAN = 0x7F800000;
  Function &F;
  explicit FPToSIState(Function &F) : F(F) {}

  BasicBlock *getSuccBBByName(BranchInst *BR, StringRef Name) {
    for (unsigned i = 0; i < BR->getNumSuccessors(); i++) {
      if (BR->getSuccessor(i)->getName().startswith(Name)) {
        return BR->getSuccessor(i);
      }
    }
    return nullptr;
  }

  BasicBlock *getNotInfNanBB(BranchInst *BR) {
    return getSuccBBByName(BR, "not.inf.nan");
  }

  BasicBlock *getLowerBoundOkBB(BranchInst *BR) {
    return getSuccBBByName(BR, "lower.bound.ok");
  }

  BasicBlock *getUpperBoundOkBB(BranchInst *BR) {
    return getSuccBBByName(BR, "upper.bound.ok");
  }

  void getFloatBitcastInsts(SmallVectorImpl<BitCastInst *> &InstList) {
    for (auto I = inst_begin(F); I != inst_end(F);) {
      // check float inf or nan:
      //   %1 = bitcast double %0 to i64
      //   %2 = and i64 %1, 9218868437227405312
      //   %notInfOrNan = icmp ne i64 %2, 9218868437227405312
      //   br i1 %notInfOrNan, label %not.inf.nan, label %inf.or.nan
      auto *BCI = dyn_cast<BitCastInst>(&*I++);
      if (!BCI ||
          // 32: int32 bitSize, 64: int64 bitSize
          !(BCI->getType()->isIntegerTy(32) ||
            BCI->getType()->isIntegerTy(64)) ||
          !(BCI->getOperand(0)->getType()->isDoubleTy() ||
            BCI->getOperand(0)->getType()->isFloatTy()) ||
          BCI->getParent()->getName().startswith(FPExceptionBBName)) {
        continue;
      }

      auto *AndInst = cast<Instruction>(BCI->getNextNode());
      if (AndInst->getOpcode() != Instruction::And)
        continue;
      auto *AndOp1 = dyn_cast<ConstantInt>(AndInst->getOperand(1));
      if (!AndOp1)
        continue;
      if (!(AndOp1->equalsInt(DOUBLE_INF_OR_NAN) ||
            AndOp1->equalsInt(FLOAT_INF_OR_NAN)))
        continue;

      InstList.push_back(BCI);
    }
  }

  bool optFPToSIThreeCmp() {
    // AArch64 is supported first, and x86 is later.
    if (!Triple(F.getParent()->getTargetTriple()).isAArch64() ||
        !F.hasCangjieGC()) {
      return false;
    }

    bool Changed = false;
    SmallVector<BitCastInst *, 16> BitCastInstList;
    getFloatBitcastInsts(BitCastInstList);
    // check successor BB
    for (auto *BCI : BitCastInstList) {
      // not.inf.nan:   // check integer lower bound
      //   %f2i.lt.min = fcmp olt double 0xC3E0000000000001, %0
      //   br i1 %f2i.lt.min, label %lower.bound.ok, label %lower.bound.overflow
      BasicBlock *NotInfNanBB = nullptr;
      if (auto *BR = dyn_cast<BranchInst>(BCI->getParent()->getTerminator())) {
        NotInfNanBB = getNotInfNanBB(BR);
        if (!NotInfNanBB) {
          continue;
        }
      } else {
        continue;
      }

      // lower.bound.ok:   // check integer upper bound
      //   %f2i.gt.max = fcmp olt double %0, 0x43E0000000000000
      //   br i1 %f2i.gt.max, label %upper.bound.ok, label %upper.bound.overflow
      BasicBlock *LowerBoundOkBB = nullptr;
      if (auto *BR = dyn_cast<BranchInst>(NotInfNanBB->getTerminator())) {
        LowerBoundOkBB = getLowerBoundOkBB(BR);
        if (!LowerBoundOkBB) {
          continue;
        }
      } else {
        continue;
      }

      BasicBlock *UpperBoundOkBB = nullptr;
      if (auto *BR = dyn_cast<BranchInst>(LowerBoundOkBB->getTerminator())) {
        UpperBoundOkBB = getUpperBoundOkBB(BR);
        if (!UpperBoundOkBB) {
          continue;
        }
      } else {
        continue;
      }

      bool HasFPToInt = false;
      for (auto &I : *UpperBoundOkBB) {
        Instruction *FI = dyn_cast<FPToSIInst>(&I);
        if (!FI) {
          FI = dyn_cast<FPToUIInst>(&I);
        }
        if (FI && FI->getOperand(0) == BCI->getOperand(0) &&
            // 32: int32 bitSize, 64: int64 bitSize
            (FI->getType()->isIntegerTy(32) ||
             FI->getType()->isIntegerTy(64))) {
          FI->moveBefore(BCI);
          HasFPToInt = true;
          break;
        }
      }

      if (!HasFPToInt) {
        continue;
      }

      repalceThreeOverflowCmp(BCI, UpperBoundOkBB, F);
      Changed = true;
    }
    return Changed;
  }

  void repalceThreeOverflowCmp(BitCastInst *BCI, BasicBlock *UpperBoundOkBB,
                               Function &F) {
    IRBuilder<> Builder(BCI);
    auto *FI = BCI->getPrevNode();

    // insert call i64 @llvm.cj.get.fp.state
    Function *GetFpStateFunc = Intrinsic::getDeclaration(
        F.getParent(), Intrinsic::cj_get_fp_state, {FI->getType()});
    GetFpStateFunc->setDoesNotAccessMemory();
    CallInst *GetFpStateCI = Builder.CreateCall(GetFpStateFunc, {FI});

    // if fp to int failed, the lastBit of %fpstate is 1
    // insert and %fpstate, 1
    Value *LastBit = Builder.CreateAnd(GetFpStateCI, (uint64_t)1);
    // insert icmp %and, 0
    auto *CmpEQ = dyn_cast<Instruction>(Builder.CreateICmpEQ(
        LastBit,
        ConstantInt::get(Type::getInt64Ty(F.getContext()), (uint64_t)0)));

    // add reset fpstate instr after CJSpecificOpt
    // split new BB
    BasicBlock *ConvertExcepBB = BCI->getParent()->splitBasicBlock(
        CmpEQ->getNextNode(), FPExceptionBBName);
    Instruction *NewBr = CmpEQ->getParent()->getTerminator();
    IRBuilder<> BuilderBr(NewBr);
    BuilderBr.CreateCondBr(CmpEQ, UpperBoundOkBB, ConvertExcepBB);
    NewBr->eraseFromParent();
  }
};

// Create CJ_MCC_MutexLock fastpath.
struct MutexLockLower {
  Function *F;
  explicit MutexLockLower(Function *F) : F(F) {}

  static constexpr char StubFuncName[] = "CJMutexLockStub";
  static constexpr char RTSlowPathName[] = "CJ_MCC_MutexLockSlowPath";
  static constexpr char GetCJThreadIDName[] = "GetCJThreadIdForMutexOpt";

  Function *createMutexLockStub(FunctionType *FT, Module *M) {
    Function *F =
        cast<Function>(M->getOrInsertFunction(StubFuncName, FT).getCallee());
    F->setLinkage(GlobalValue::InternalLinkage);
    F->addFnAttr(Attribute::AlwaysInline);

    // Create function body.
    BasicBlock *BB0 = BasicBlock::Create(F->getContext(), "entry", F);
    BasicBlock *BB1 = BasicBlock::Create(F->getContext(), "if.then", F);
    BasicBlock *BB2 = BasicBlock::Create(F->getContext(), "if.else", F);
    IRBuilder<> IRB(BB0);
    // Prepare mutex params.
    Value *MutexPtr = F->getArg(0);
    Type *MutexType = MutexPtr->getType()->getNonOpaquePointerElementType();
    Value *MutexCjthreadIdPtr =
        IRB.CreateBitCast(IRB.CreateGEP(MutexType, MutexPtr, IRB.getInt32(8)),
                          Type::getInt64PtrTy(F->getContext(), 1));
    Value *MutexOwnCountPtr =
        IRB.CreateBitCast(IRB.CreateGEP(MutexType, MutexPtr, IRB.getInt32(16)),
                          Type::getInt64PtrTy(F->getContext(), 1));
    Value *MutexStatePtr =
        IRB.CreateBitCast(IRB.CreateGEP(MutexType, MutexPtr, IRB.getInt32(24)),
                          Type::getInt64PtrTy(F->getContext(), 1));
    Value *Expected = ConstantInt::get(IRB.getInt64Ty(), 0);
    // 0x4: LOCKED flag which is defined in runtime
    Value *Desired = ConstantInt::get(IRB.getInt64Ty(), 0x4);
    // try to get cj mutex
    AtomicCmpXchgInst *CmpRes =
        IRB.CreateAtomicCmpXchg(MutexStatePtr, Expected, Desired, MaybeAlign(),
                                AtomicOrdering::SequentiallyConsistent,
                                AtomicOrdering::SequentiallyConsistent);
    Value *Success = IRB.CreateExtractValue(CmpRes, 1, "success");
    IRB.CreateCondBr(Success, BB1, BB2);

    // Build BB1
    IRB.SetInsertPoint(BB1);
    auto *MutexOwnCount = IRB.CreateLoad(
        MutexOwnCountPtr->getType()->getNonOpaquePointerElementType(),
        MutexOwnCountPtr);
    Value *OwnCountInc =
        IRB.CreateAdd(MutexOwnCount, ConstantInt::get(IRB.getInt64Ty(), 1));
    IRB.CreateStore(OwnCountInc, MutexOwnCountPtr);
    FunctionType *FuncType = FunctionType::get(IRB.getInt64Ty(), false);
    auto *GetCJThreadIdFunc = dyn_cast<Function>(
        F->getParent()
            ->getOrInsertFunction(GetCJThreadIDName, FuncType)
            .getCallee());
    GetCJThreadIdFunc->addFnAttr(
        Attribute::get(F->getContext(), "gc-leaf-function"));
    GetCJThreadIdFunc->addFnAttr(Attribute::get(F->getContext(), "cj-runtime"));
    GetCJThreadIdFunc->setCallingConv(CallingConv::CangjieGC);
    GetCJThreadIdFunc->setUnnamedAddr(GlobalValue::UnnamedAddr::Local);
    auto *CallInst = IRB.CreateCall(GetCJThreadIdFunc);
    CallInst->setCallingConv(CallingConv::CangjieGC);
    auto *StoreInst = IRB.CreateStore(CallInst, MutexCjthreadIdPtr);
    StoreInst->setAtomic(AtomicOrdering::Release);
    IRB.CreateRetVoid();

    // Build BB2
    IRB.SetInsertPoint(BB2);
    FunctionType *SlowPathFuncType =
        FunctionType::get(IRB.getVoidTy(), {MutexPtr->getType()}, false);
    auto *SlowPathFunc = dyn_cast<Function>(
        F->getParent()
            ->getOrInsertFunction(RTSlowPathName, SlowPathFuncType)
            .getCallee());
    IRB.CreateCall(SlowPathFunc, {MutexPtr});
    IRB.CreateRetVoid();

    return F;
  }

  bool optCJMutexLock() {
    if (!F->hasCangjieGC() || CJTsanSupport)
      return false;

    SetVector<CallBase *> CallBases;
    for (auto I = inst_begin(F); I != inst_end(F);) {
      CallBase *CB = dyn_cast<CallBase>(&*I++);
      if (!CB || !CB->getCalledFunction())
        continue;
      if (CB->getCalledFunction()->getName().isCJMutexLock())
        CallBases.insert(CB);
    }
    if (CallBases.empty())
      return false;

    FunctionType *FT = FunctionType::get(
        Type::getVoidTy(F->getContext()),
        {CallBases.front()->getArgOperand(0)->getType()}, false);
    Function *Stub = createMutexLockStub(FT, F->getParent());
    for (auto *CB : CallBases) {
      CallInst::Create(Stub, {CB->getArgOperand(0)}, "", CB);
      CB->eraseFromParent();
    }
    return true;
  }
};

// Icmp of a and b.
// 1. If a > 0, then
//   a slt b => a ult b;
//   also for sgt and ugt.
// 2. If a < 0, then
//   a ult b => a slt b;
//   also for ugt and sgt.
struct ICmpOptimization {
  Function &F;
  DominatorTree &DT;

  ICmpOptimization(Function &F, DominatorTree &DT) : F(F), DT(DT) {}

  BranchInst *brAndICmpConditional(BasicBlock *BB) {
    BranchInst *BI = dyn_cast<BranchInst>(BB->getTerminator());
    if (!BI || BI->isUnconditional() || !isa<ICmpInst>(BI->getCondition()))
      return nullptr;
    return BI;
  }

  bool isLoadFromArray(Value *Ptr, unsigned Times = 3) {
    auto IsArrayType = [](Type *Ty) {
      auto *ST = dyn_cast<StructType>(Ty);
      return ST && ST->hasName() &&
             (ST->getName().isCangjieArrayLayout() ||
              ST->getName().isCangjieArrayRecord());
    };
    if (Times == 0)
      return false;
    if (auto *GEP = dyn_cast<GEPOperator>(Ptr))
      return IsArrayType(GEP->getSourceElementType()) ||
             isLoadFromArray(GEP->getPointerOperand(), --Times);
    else if (auto *BC = dyn_cast<BitCastInst>(Ptr)) {
      Value *OP = BC->getOperand(0);
      return (isa<PointerType>(OP->getType()) &&
              IsArrayType(OP->getType()->getNonOpaquePointerElementType())) ||
             isLoadFromArray(OP, --Times);
    }
    return false;
  }

  bool isPositiveOrZeroValue(Value *V, SmallSet<Value *, 8> &Set) {
    if (auto *CI = dyn_cast<ConstantInt>(V))
      return CI->getValue().isNonNegative();
    if (Set.contains(V))
      return true;
    Instruction *I = dyn_cast<Instruction>(V);
    if (!I)
      return false;
    Set.insert(I);
    switch (I->getOpcode()) {
    default:
      return false;
    case Instruction::Call: {
      auto *CB = cast<CallBase>(I);
      auto IID = CB->getIntrinsicID();
      if (IID != Intrinsic::smul_with_overflow &&
          IID != Intrinsic::umul_with_overflow &&
          IID != Intrinsic::sadd_with_overflow &&
          IID != Intrinsic::uadd_with_overflow)
        return false;
    }
      [[fallthrough]];
    case Instruction::Add:
    case Instruction::Mul:
    case Instruction::Xor:
    case Instruction::And:
      return isPositiveOrZeroValue(I->getOperand(0), Set) &&
             isPositiveOrZeroValue(I->getOperand(1), Set);
    case Instruction::PHI: {
      auto *Phi = cast<PHINode>(I);
      for (Value *Val : Phi->incoming_values()) {
        if (!isPositiveOrZeroValue(Val, Set))
          return false;
      }
      return true;
    } break;
    case Instruction::Select: {
      auto *SI = cast<SelectInst>(I);
      return isPositiveOrZeroValue(SI->getTrueValue(), Set) &&
             isPositiveOrZeroValue(SI->getFalseValue(), Set);
    } break;
    case Instruction::ExtractValue:
      return isPositiveOrZeroValue(I->getOperand(0), Set);
    case Instruction::Load:
      auto *LI = cast<LoadInst>(I);
      return isLoadFromArray(LI->getPointerOperand());
    }
  }

  bool tryEliminateICmp(BranchInst *BI, BasicBlock *BB, unsigned Times = 3) {
    auto *Condition = cast<ICmpInst>(BI->getCondition());
    auto Pred = Condition->getPredicate();
    switch (Pred) {
    default:
      return false; // Current, return false
    case CmpInst::ICMP_ULT:
      Value *OP = Condition->getOperand(0);
      SmallSet<Value *, 8> Set;
      if (!isPositiveOrZeroValue(OP, Set))
        return false;
      break;
    }
    auto *DTNode = DT.getNode(BB);
    if (DTNode == nullptr)
      return false;
    auto *N = DTNode->getIDom();
    while (N != nullptr && Times--) {
      BasicBlock *BB = N->getBlock();
      assert(DT.getNode(BB) && "DTNode of BB can not be nullptr!");
      N = DT.getNode(BB)->getIDom();
      if (auto *BI1 = brAndICmpConditional(BB); BI1 && icmpInstMatch(BI, BI1))
        return true;
    }
    return false;
  }

  bool icmpInstMatch(BranchInst *OriginBI, BranchInst *CurrentBI) {
    using namespace PatternMatch;
    auto *ICI0 = cast<ICmpInst>(OriginBI->getCondition());
    auto *ICI1 = cast<ICmpInst>(CurrentBI->getCondition());

    // If we know that OP1 and OP2 are related by a simple relation like OP2 =
    // OP1
    // + x (x > 0), then we can determine that OP1 < OP2.
    auto IsLessThanEQ = [](Value *OP1, Value *OP2, bool EQ) {
      Value *V1 = nullptr;
      Value *V2 = nullptr;
      if (match(OP2, m_Add(m_Value(V1), m_Value(V2))) ||
          match(OP2, m_Or(m_Value(V1), m_Value(V2)))) {
        // OP2 = OP1 + x, x >= 0
        auto *CI1 = V1 == OP1 ? dyn_cast<ConstantInt>(V2) : nullptr;
        auto *CI2 = V2 == OP1 ? dyn_cast<ConstantInt>(V1) : nullptr;
        return (CI1 && (EQ ? CI1->getValue().isNonNegative()
                           : CI1->getValue().isStrictlyPositive())) ||
               (CI2 && (EQ ? CI2->getValue().isNonNegative()
                           : CI2->getValue().isStrictlyPositive()));
      } else if (match(OP2, m_Sub(m_Value(V1), m_Value(V2)))) {
        // OP2 = OP1 - x, x <= 0
        auto *CI = V1 == OP1 ? dyn_cast<ConstantInt>(V2) : nullptr;
        return CI && (EQ ? CI->getValue().isNonPositive()
                         : CI->getValue().isNegative());
      }

      if (match(OP1, m_Sub(m_Value(V1), m_Value(V2)))) {
        // OP1 = OP2 - x, x >= 0
        auto *CI = V1 == OP2 ? dyn_cast<ConstantInt>(V2) : nullptr;
        return CI && (EQ ? CI->getValue().isNonNegative()
                         : CI->getValue().isStrictlyPositive());
      } else if (match(OP1, m_Add(m_Value(V1), m_Value(V2))) ||
                 match(OP1, m_Add(m_Value(V1), m_Value(V2)))) {
        // OP1 = OP2 + x, x <= 0
        auto *CI1 = V1 == OP2 ? dyn_cast<ConstantInt>(V2) : nullptr;
        auto *CI2 = V2 == OP2 ? dyn_cast<ConstantInt>(V1) : nullptr;
        return (CI1 && (EQ ? CI1->getValue().isNonPositive()
                           : CI1->getValue().isNegative())) ||
               (CI2 && (EQ ? CI2->getValue().isNonPositive()
                           : CI2->getValue().isNegative()));
      }
      return false;
    };

    switch (ICI0->getPredicate()) {
    default:
      break;
    case CmpInst::ICMP_ULT:
      Value *OP = ICI0->getOperand(0);
      auto Pred = ICI1->getPredicate();
      if (ICI1->getOperand(0) == OP) {
        bool Tag1 =
            (Pred == CmpInst::ICMP_ULT || Pred == CmpInst::ICMP_SLT) &&
            DT.dominates(CurrentBI->getSuccessor(0), OriginBI->getParent());
        bool Tag2 =
            (Pred == CmpInst::ICMP_UGE || Pred == ICmpInst::ICMP_SGE) &&
            DT.dominates(CurrentBI->getSuccessor(1), OriginBI->getParent());
        if (Tag1 || Tag2)
          return IsLessThanEQ(ICI1->getOperand(1), ICI0->getOperand(1), true);
        bool Tag3 =
            (Pred == CmpInst::ICMP_ULE || Pred == CmpInst::ICMP_SLE) &&
            DT.dominates(CurrentBI->getSuccessor(0), OriginBI->getParent());
        bool Tag4 =
            (Pred == CmpInst::ICMP_UGT || Pred == CmpInst::ICMP_SGT) &&
            DT.dominates(CurrentBI->getSuccessor(1), OriginBI->getParent());
        if (Tag3 || Tag4)
          return IsLessThanEQ(ICI1->getOperand(1), ICI0->getOperand(1), false);
      } else if (ICI1->getOperand(1) == OP) {
        bool Tag1 =
            (Pred == CmpInst::ICMP_UGT || Pred == CmpInst::ICMP_SGT) &&
            DT.dominates(CurrentBI->getSuccessor(0), OriginBI->getParent());
        bool Tag2 =
            (Pred == CmpInst::ICMP_SLE || Pred == CmpInst::ICMP_ULE) &&
            DT.dominates(CurrentBI->getSuccessor(1), OriginBI->getParent());
        if (Tag1 || Tag2)
          return IsLessThanEQ(ICI1->getOperand(0), ICI0->getOperand(1), true);
        bool Tag3 =
            (Pred == CmpInst::ICMP_UGE || Pred == CmpInst::ICMP_SGE) &&
            DT.dominates(CurrentBI->getSuccessor(0), OriginBI->getParent());
        bool Tag4 =
            (Pred == CmpInst::ICMP_SLT || Pred == CmpInst::ICMP_ULT) &&
            DT.dominates(CurrentBI->getSuccessor(1), OriginBI->getParent());
        if (Tag3 || Tag4)
          return IsLessThanEQ(ICI1->getOperand(0), ICI0->getOperand(1), false);
      }
    }
    return false;
  }

  bool eliminateICmp() {
    bool Changed = false;
    for (auto &BB : F) {
      if (auto *BI = brAndICmpConditional(&BB);
          BI && tryEliminateICmp(BI, &BB)) {
        BI->setCondition(ConstantInt::getTrue(BI->getContext()));
        Changed = true;
      }
    }
    return Changed;
  }
};

struct ArraySizeConstantFold {
  Function &F;

  explicit ArraySizeConstantFold(Function &F) : F(F) {}

  bool isArraySizeGEP(CallBase *CB, GetElementPtrInst *GEP) {
    // The Base of the GEP must be CB.
    Value *Base = getUnderlyingObject(GEP->getPointerOperand());
    if (Base != CB)
      return false;
    // %0 = call i8 addrspace (1)* @CJ_MCC_NewArrayStub(i64 size, arrayKlass)
    // %1 = getelementptr inbounds i8, i8 addrspace (1)* %0, i64 8
    if (isInt8AS1Pty(Base->getType())) {
      if (GEP->getNumIndices() > 1)
        return false;
      auto *Const = dyn_cast<ConstantInt>(GEP->getOperand(1));
      // Check whether GEP is to get array size.
      return Const && Const->getZExtValue() == 8;
    }
    // %ArrayLayout.xxx = type { %ArrayBase, [ 0 * xxx ] }
    // %ArrayBase = type { %ObjLayout.Object, i64 }
    // ...
    // %0 = call i8 addrspace (1)* @CJ_MCC_NewArrayStub(i64 size, arrayKlass)
    // %1 = bitcast i8 addrspace (1)* %0 to %ArrayLayout.xxx addrspace (1)*
    // %2 = getelementptr inbounds ArrayLayout.xxx,
    //   %ArrayLayout.xxx addrspace (1)* %0, i64 0, i32 0, i32 1
    auto *ST = dyn_cast<StructType>(GEP->getSourceElementType());
    // gepIndexNum: Number of other parameters except SrcPtr
    // example:
    //    GEP %SrcPtr, 0, 0, 1
    constexpr int gepIndexNum = 3;
    if (ST == nullptr || !ST->getStructName().isCangjieArrayLayout() ||
        GEP->getNumIndices() != gepIndexNum)
      return false;

    auto *Const0 = dyn_cast<ConstantInt>(GEP->getOperand(1));
    auto *Const1 = dyn_cast<ConstantInt>(GEP->getOperand(2));
    auto *Const2 = dyn_cast<ConstantInt>(GEP->getOperand(3));
    return Const0 != nullptr && Const0->getZExtValue() == 0 &&
           Const1 != nullptr && Const1->getZExtValue() == 0 &&
           Const2 != nullptr && Const2->getZExtValue() == 1;
  }

  void getArraySizeGEP(CallBase *CB, SetVector<Value *> &Uses,
                       SetVector<GetElementPtrInst *> &ArraySizeGEP) {
    for (auto *Use : Uses) {
      if (auto *GEP = dyn_cast<GetElementPtrInst>(Use);
          GEP && isArraySizeGEP(CB, GEP)) {
        ArraySizeGEP.insert(GEP);
      }
    }
  }

  bool replaceSizeGEPtoConst(SetVector<Value *> &Uses, ConstantInt *Size,
                             SetVector<GetElementPtrInst *> &ArraySizeGEP) {
    bool Changed = false;
    for (auto *Use : Uses) {
      auto *LI = dyn_cast<LoadInst>(Use);
      if (LI == nullptr)
        continue;
      auto *GEP = dyn_cast<GetElementPtrInst>(
          LI->getPointerOperand()->stripPointerCasts());
      if (GEP == nullptr)
        continue;
      if (ArraySizeGEP.contains(GEP)) {
        if (LI->getType() == Size->getType()) {
          LI->replaceAllUsesWith(Size);
          Changed = true;
        } else {
          continue;
        }
      }
    }
    return Changed;
  }

  bool optArraySizeConst() {
    bool Changed = false;

    DenseSet<StringRef> ArrayFuncs = {
        "CJ_MCC_NewArray",   "CJ_MCC_NewArray8",  "CJ_MCC_NewArray16",
        "CJ_MCC_NewArray32", "CJ_MCC_NewArray64", "CJ_MCC_NewObjArray"};

    auto GetUses = [](Value *Val, SetVector<Value *> &Uses) {
      SmallVector<Value *> Worklist = {Val};
      while (!Worklist.empty()) {
        Value *V = Worklist.pop_back_val();
        for (auto *Use : V->users()) {
          auto *U = dyn_cast<Value>(Use);
          if (Uses.contains(U))
            continue;
          Uses.insert(U);
          Worklist.push_back(U);
        }
      }
    };

    for (auto &I : instructions(F)) {
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        Function *Callee = CB->getCalledFunction();
        if (Callee && ArrayFuncs.contains(Callee->getName())) {
          if (auto *Size = dyn_cast<ConstantInt>(CB->getArgOperand(1))) {
            SetVector<Value *> Uses;
            GetUses(CB, Uses);
            SetVector<GetElementPtrInst *> ArraySizeGEP;
            getArraySizeGEP(CB, Uses, ArraySizeGEP);
            Changed |= replaceSizeGEPtoConst(Uses, Size, ArraySizeGEP);
          }
        }
      }
    }

    return Changed;
  }
};

struct AnalysisResults {
  DominatorTree *DT = nullptr;
  PostDominatorTree *PDT = nullptr;
};

static bool runImpl(Function &F, function_ref<AnalysisResults()> GetAnalysis) {
  bool Changed = false;

  Changed |= FPToSIState(F).optFPToSIThreeCmp();
  AnalysisResults AR = GetAnalysis();
  if (AR.PDT)
    Changed |= CallRetState(&F, *AR.PDT).optCJCallRet();
  if (AR.DT)
    Changed |= ICmpOptimization(F, *AR.DT).eliminateICmp();
  Changed |= ArraySizeConstantFold(F).optArraySizeConst();

  return Changed;
}

PreservedAnalyses CJSimpleOpt::run(Function &F,
                                   FunctionAnalysisManager &FAM) const {
  auto GetAnalysis = [&]() -> AnalysisResults {
    if (!F.isDeclaration())
      return {&FAM.getResult<DominatorTreeAnalysis>(F),
              &FAM.getResult<PostDominatorTreeAnalysis>(F)};
    return AnalysisResults();
  };
  if (runImpl(F, GetAnalysis)) {
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

// The following instruction is redundant because the runtime has already done
// it when the catch type is unique.
//  landing_pad_bb:
//    ...
//    %x = call i1 @llvm.cj.is.subtype(...)
//    br i1 %x, label %succ0, %succ1
static bool simplifyLandingPad(Function &F) {
  bool Changed = false;
  for (auto &I : instructions(F)) {
    auto *LPI = dyn_cast<LandingPadInst>(&I);
    if (!LPI)
      continue;
    // Catch type should be unique.
    if (LPI->getNumOperands() != 1 ||
        isa<ConstantPointerNull>(LPI->getOperand(0)))
      continue;
    auto *BI = dyn_cast<BranchInst>(LPI->getParent()->getTerminator());
    if (!BI || !BI->isConditional())
      continue;
    auto *CB = dyn_cast<CallBase>(BI->getCondition());
    if (!CB || !CB->getCalledFunction()->hasName() ||
        CB->getCalledFunction()->getName() != "CJ_MCC_IsSubType")
      continue;
    CB->replaceAllUsesWith(
        ConstantInt::getTrue(Type::getInt1Ty(LPI->getContext())));
    Changed = true;
  }
  return Changed;
}

PreservedAnalyses
CJAfterInlineSimpleOpt::run(Function &F, FunctionAnalysisManager &FAM) const {
  if (simplifyLandingPad(F)) {
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}
