//===-- GCBarrierLowering.cpp - Cangjie Barrier Lowering ------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file implements the lowering for the cangjie barrier mechanism.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/PriorityWorklist.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/CJIntrinsics.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/SafepointIRVerifier.h"
#include "llvm/IR/Statepoint.h"
#include "llvm/IR/CJStructTypeGCInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/MCContext.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/LoopUtils.h"

#include <unordered_map>

using namespace llvm;
using namespace cangjie;

#define DEBUG_TYPE "cj-barrier-lowering"

static cl::opt<bool> EnableTaggedPointer("enable-tagged-pointer",
                                         cl::init(true), cl::Hidden);
static cl::opt<bool> EnableGCPhase("enable-gc-phase", cl::init(true),
                                   cl::Hidden);
static cl::opt<bool> EnableGCFastPath("enable-gc-fast-path", cl::init(true),
                                      cl::Hidden);
static cl::opt<bool> EnableGCStateLoop("cj-gcstate-dup-loop", cl::init(false),
                                       cl::ReallyHidden);

namespace llvm {
extern cl::opt<bool> CangjieJIT;
extern cl::opt<bool> DisableGCSupport;
extern cl::opt<bool> EnableSafepointOnly;
extern cl::opt<bool> EnableSafepointOutline;
} // namespace llvm

namespace {
const static StringRef NewObjFastStr = "CJ_MCC_NewObjectFast";
const static StringRef NewObjFinalizerFastStr = "CJ_MCC_NewFinalizerFast";
constexpr StringRef SafepointStub = "CJ_Safepoint_Stub";
template <typename KeyT, typename ValT>
using StdMap = std::unordered_map<KeyT, ValT>;
const static StdMap<unsigned, StringRef> IntrinsicMap{
    {Intrinsic::cj_gcwrite_ref, "CJ_MCC_WriteRefField"},
    {Intrinsic::cj_gcwrite_struct, "CJ_MCC_WriteStructField"},
    {Intrinsic::cj_gcwrite_static_ref, "CJ_MCC_WriteStaticRef"},
    {Intrinsic::cj_gcwrite_static_struct, "CJ_MCC_WriteStaticStruct"},
    {Intrinsic::cj_gcread_ref, "CJ_MCC_ReadRefField"},
    {Intrinsic::cj_gcread_weakref, "CJ_MCC_ReadWeakRef"},
    {Intrinsic::cj_gcread_struct, "CJ_MCC_ReadStructField"},
    {Intrinsic::cj_gcread_static_ref, "CJ_MCC_ReadStaticRef"},
    {Intrinsic::cj_gcread_static_struct, "CJ_MCC_ReadStaticStruct"},
    {Intrinsic::cj_copy_struct_field, "CJ_MCC_CopyStructField"},
    {Intrinsic::cj_array_copy_ref, "CJ_MCC_ArrayCopyRef"},
    {Intrinsic::cj_array_copy_struct, "CJ_MCC_ArrayCopyStruct"},
    {Intrinsic::cj_atomic_store, "CJ_MCC_AtomicWriteReference"},
    {Intrinsic::cj_atomic_load, "CJ_MCC_AtomicReadReference"},
    {Intrinsic::cj_atomic_swap, "CJ_MCC_AtomicSwapReference"},
    {Intrinsic::cj_atomic_compare_swap, "CJ_MCC_AtomicCompareAndSwapReference"},
    {Intrinsic::cj_assign_generic, "CJ_MCC_AssignGeneric"},
    {Intrinsic::cj_gcwrite_generic, "CJ_MCC_WriteGeneric"},
    {Intrinsic::cj_gcread_generic, "CJ_MCC_ReadGeneric"},
    {Intrinsic::cj_array_copy_generic, "CJ_MCC_ArrayCopyGeneric"},
    {Intrinsic::cj_gcwrite_generic_payload, "CJ_MCC_WriteGenericPayload"}};

struct BBInfo {
  // BB last used barrier after safepoint
  CallBase *LastBarrier;

  bool HasCalls; // has GC Calls

  //  there are barriers before first safepoint
  bool BeforeCall;
  bool Visited;
  Instruction *OutCheck;
  Instruction *InCheck;

  // 8: default preBBs size
  SmallVector<BasicBlock *, 8> PreBBs;

  BBInfo() : HasCalls(false), BeforeCall(false), Visited(false) {
    LastBarrier = nullptr;
    OutCheck = nullptr;
    InCheck = nullptr;
  }
};

static const unsigned MaxLoopBlock = 64;
static const char *ClonedPinLoopTag = "cj.pin.loop.clone";

static bool mayBeSafepoint(Instruction *Inst) {
  if (auto CI = dyn_cast<CallBase>(Inst)) {
    if (CI->getIntrinsicID() == Intrinsic::cj_gc_statepoint) {
      return true;
    }
  }
  return false;
}

/// Declarations for Cangjie barrier functions.
class BarrierMaker {
public:
  BarrierMaker(Module &M, CJStructTypeGCInfo &GCInfo)
      : M(M), C(M.getContext()), GCInfo(GCInfo) {
    GCPtr = Type::getInt8PtrTy(C, 1);
    I8Ptr = Type::getInt8PtrTy(C);
    I64 = Type::getInt64Ty(C);
    I32 = Type::getInt32Ty(C);
  }

  ~BarrierMaker() = default;

  void replaceInstWithBarrier(IntrinsicInst *II) {
    if (II->isTailCall()) {
      II->setTailCall(false);
    }

    Function *Callee = getOrInsertRuntimeFunc(II);
    switch (II->getIntrinsicID()) {
    case Intrinsic::cj_gcwrite_struct: {
      Value *GCTib = getOrInsertGCTib(II);
      Value *Param[6] = {II->getArgOperand(GCWriteStruct::BaseObj),
                         II->getArgOperand(GCWriteStruct::Dst),
                         II->getArgOperand(GCWriteStruct::Size),
                         II->getArgOperand(GCWriteStruct::Src),
                         II->getArgOperand(GCWriteStruct::Size), GCTib};
      // gcwrite.agg src has two case:
      // i8* on the stack and i8 addrspace(1)* on the heap.
      // The runtime function has only one signature. Here, the src of i8*
      // is converted to i8 addrspace(1)*.
      if (!isGCPointerType(II->getArgOperand(GCWriteStruct::Src)->getType())) {
        Param[3] = new AddrSpaceCastInst(II->getArgOperand(GCWriteStruct::Src),
                                         Type::getInt8PtrTy(C, 1), "", II);
      }
      // Replace the original intrinsic with the runtime function.
      replaceCallInst(Callee, Param, II);
      break;
    }
    case Intrinsic::cj_gcread_struct: {
      Value *GCTib = getOrInsertGCTib(II);
      Value *Param[5] = {II->getArgOperand(GCReadStruct::Dst),
                         II->getArgOperand(GCReadStruct::BaseObj),
                         II->getArgOperand(GCReadStruct::Src),
                         II->getArgOperand(GCReadStruct::Size), GCTib};
      // Replace the original intrinsic with the runtime function.
      replaceCallInst(Callee, Param, II);
      break;
    }
    case Intrinsic::cj_gcread_static_struct:
    case Intrinsic::cj_gcwrite_static_struct: {
      Value *GCTib = getOrInsertGCTib(II);
      Value *Param[5] = {II->getArgOperand(GCWriteStaticStruct::Dst),
                         II->getArgOperand(GCWriteStaticStruct::Size),
                         II->getArgOperand(GCWriteStaticStruct::Src),
                         II->getArgOperand(GCWriteStaticStruct::Size), GCTib};
      // Replace the original intrinsic with the runtime function.
      replaceCallInst(Callee, Param, II);
      break;
    }
    case Intrinsic::cj_array_copy_ref:
    case Intrinsic::cj_array_copy_struct:
    case Intrinsic::cj_array_copy_generic: {
      Value *Param[6] = {II->getArgOperand(ArrayCopy::DstObj),
                         II->getArgOperand(ArrayCopy::DstPtr),
                         II->getArgOperand(ArrayCopy::Size),
                         II->getArgOperand(ArrayCopy::SrcObj),
                         II->getArgOperand(ArrayCopy::SrcPtr),
                         II->getArgOperand(ArrayCopy::Size)};
      // Replace the original intrinsic with the runtime function.
      replaceCallInst(Callee, Param, II);
      break;
    }
    default:
      II->setCalledFunction(Callee);
      break;
    }
  }

  void replaceCallInst(Function *Callee, ArrayRef<Value *> Args,
                       IntrinsicInst *II) const {
    IRBuilder<> Builder(II);
    CallInst *NewCI = Builder.CreateCall(Callee, Args);
    II->replaceAllUsesWith(NewCI);
    II->eraseFromParent();
  }

private:
  Module &M;
  LLVMContext &C;
  CJStructTypeGCInfo &GCInfo;
  DenseMap<StringRef, Function *> RTFuncMap;
  Type *GCPtr;
  Type *I8Ptr;
  Type *I64;
  Type *I32;

  StringRef getRuntimeFuncName(IntrinsicInst *II) {
    Intrinsic::ID IID = II->getIntrinsicID();
    auto Itr = IntrinsicMap.find(IID);
    assert(Itr != IntrinsicMap.end() && "Runtime Intrinsic don`t exist.");
    return Itr->second;
  }

  Function *getOrInsertRuntimeFunc(IntrinsicInst *II) {
    StringRef Callee = getRuntimeFuncName(II);
    assert(Callee != "" && "Callee don`t exist.");
    auto Itr = RTFuncMap.find(Callee);
    if (Itr != RTFuncMap.end())
      return Itr->second;
    // add dstLen to runtime API
    FunctionType *FuncType = nullptr;
    const Triple TT(II->getModule()->getTargetTriple());
    auto isARM = TT.isARM();
    switch (II->getIntrinsicID()) {
    case Intrinsic::cj_gcwrite_struct: {
      Type *ParamType[6] = {GCPtr, GCPtr, I64, GCPtr, I64, I8Ptr};
      if (isARM)
        ParamType[2] = ParamType[4] = I32;
      FuncType = FunctionType::get(Type::getVoidTy(C), ParamType, false);
      break;
    }
    case Intrinsic::cj_gcread_struct: {
      Type *ParamType[5] = {I8Ptr, GCPtr, GCPtr, I64, I8Ptr};
      if (isARM)
        ParamType[3] = I32;
      FuncType = FunctionType::get(Type::getVoidTy(C), ParamType, false);
      break;
    }
    case Intrinsic::cj_gcread_static_struct:
    case Intrinsic::cj_gcwrite_static_struct: {
      Type *ParamType[5] = {I8Ptr, I64, I8Ptr, I64, I8Ptr};
      if (isARM)
        ParamType[1] = ParamType[3] = I32;
      FuncType = FunctionType::get(Type::getVoidTy(C), ParamType, false);
      break;
    }
    case Intrinsic::cj_array_copy_ref:
    case Intrinsic::cj_array_copy_struct:
    case Intrinsic::cj_array_copy_generic: {
      Type *ParamType[6] = {GCPtr, GCPtr, I64, GCPtr, GCPtr, I64};
      if (isARM)
        ParamType[2] = ParamType[5] = I32;
      FuncType = FunctionType::get(Type::getVoidTy(C), ParamType, false);
      break;
    }
    default:
      FuncType = II->getFunctionType();
      break;
    }
    Function *Func =
        cast<Function>(M.getOrInsertFunction(Callee, FuncType).getCallee());
    RTFuncMap.insert({Callee, Func});
    return Func;
  }

  Constant *getOrInsertGCTib(IntrinsicInst *II) const {
    const MDNode *Metadata = II->getMetadata("AggType");
    assert(Metadata && "Missing AggType Metadata.");
    assert(Metadata->getNumOperands() == 1 &&
           "AggType meta's size should be equal to 1!");

    auto STName =
        dyn_cast<MDString>(Metadata->getOperand(0).get())->getString();
    auto *ST = StructType::getTypeByName(C, STName);
    assert(ST && "AggType doesn't exsit.");

    Type *DstType = Type::getInt8PtrTy(C);
    std::string BitMapName = STName.str();
    Constant *BitMapGV = M.getGlobalVariable(BitMapName + ".bitmap");
    if (BitMapGV == nullptr) {
      auto &Info = GCInfo.getOrInsertTypeGCInfo(ST);
      BitMapGV =
          GCInfo.getOrInsertBitMap(Info.BMInfo.BMStr, DstType, BitMapName);
    } else {
      BitMapGV = ConstantExpr::getBitCast(BitMapGV, DstType);
    }
    assert(BitMapGV && "BitMapGV get or insert fail");
    return BitMapGV;
  }
};

// Insert gc-phase check for write barriers.
class GCPhaseCheck {
public:
  explicit GCPhaseCheck(Function &F) : C(F.getContext()) {
    GCStateCheckFunc = F.getParent()->getFunction("GetGCPhase");
    assert(GCStateCheckFunc && "Has no GetGCPhase");
  };
  ~GCPhaseCheck() = default;

  void initBBInfo(Function &F) {
    BBInfosMap.clear();
    Barriers.clear();
    BarrierCheckMap.clear();
    for (auto &BB : F) {
      BBInfosMap[&BB] = BBInfo();
    }
  }

  void setLastBarrier(CallBase *CI) {
    BasicBlock *BB = CI->getParent();
    auto &Info = BBInfosMap[BB];
    if (Info.LastBarrier == nullptr) {
      Info.LastBarrier = CI;
      if (Info.HasCalls) {
        createCheck(CI);
      }
    }
    if (!Info.HasCalls) {
      Info.BeforeCall = true;
    }
  }

  void setHasCall(BasicBlock *BB) {
    auto &Info = BBInfosMap[BB];
    Info.HasCalls = true;
    Info.LastBarrier = nullptr;
  }

  bool isNullPointer(Value *V) {
    if (isa<Constant>(V)) {
      return V->getValueID() == Value::ConstantPointerNullVal;
    }
    return false;
  }

  bool fastBarrier(unsigned IID, CallBase *CI) {
    switch (IID) {
    // If Write barrier baseObj is null, we will replace to store.
    // Others, inline to fastbarrier.
    case Intrinsic::cj_gcwrite_ref:
    case Intrinsic::cj_gcwrite_struct: {
      Value *BaseObj = getBaseObj(CI);
      if (isNullPointer(BaseObj)) {
        IRBuilder<> Builder(CI);
        createStoreOrMems(CI, Builder);
        CI->eraseFromParent();
        return true;
      }
      break;
    }
    // non-constant ordering.
    case Intrinsic::cj_atomic_store:
    case Intrinsic::cj_atomic_swap:
      if (!isa<Constant>(getAtomicOrder(CI))) {
        return false;
      }
      break;
    case Intrinsic::cj_atomic_compare_swap:
      if (!isa<Constant>(CI->getArgOperand(AtomicCompareSwap::SuccOrder)) ||
          !isa<Constant>(CI->getArgOperand(AtomicCompareSwap::FailOrder))) {
        return false;
      }
      break;
    default:
      break;
    }

    setLastBarrier(CI);
    Barriers.push_back(CI);
    return true;
  }

  void updateBBOut(BasicBlock *BB) {
    BBInfo &Info = BBInfosMap[BB];
    if (Info.HasCalls && Info.LastBarrier != nullptr) {
      Info.OutCheck = BarrierCheckMap[Info.LastBarrier];
      assert(Info.OutCheck && "out check don't generate.");
    }
  }

  void finishAll() {
    updateBBInfo();
    replaceBarrier();
  }

  void createGCCheck(BasicBlock *InsertBB, BasicBlock *True,
                     BasicBlock *False) {
    IRBuilder<> IRB(InsertBB);
    CallInst *GCState = IRB.CreateCall(GCStateCheckFunc);
    GCState->setCallingConv(CallingConv::CangjieGC);
    auto CheckResult = IRB.CreateICmpSLE(
        GCState, Constant::getIntegerValue(GCState->getType(), APInt(32, 8)));
    IRB.CreateCondBr(CheckResult, True, False);
  }

private:
  LLVMContext &C;
  Function *GCStateCheckFunc = nullptr;
  SmallVector<CallBase *, 8> Barriers;
  SmallMapVector<BasicBlock *, BBInfo, 8> BBInfosMap;
  SmallMapVector<Instruction *, Instruction *, 8> BarrierCheckMap;

  Instruction *createCheck(CallBase *CI) {
    Instruction *CheckResult = BarrierCheckMap[CI];
    if (CheckResult)
      return CheckResult;
    IRBuilder<> Builder(CI);
    // direct load from thread local if possible
    CallInst *GCState = Builder.CreateCall(GCStateCheckFunc);
    GCState->setCallingConv(CallingConv::CangjieGC);
    // 8: related and equal to enum GCPhase::kGCPhaseInit.
    CheckResult = dyn_cast<Instruction>(Builder.CreateICmpSLE(
        GCState, Constant::getIntegerValue(GCState->getType(), APInt(32, 8))));
    BarrierCheckMap[CI] = CheckResult;
    return CheckResult;
  }

  Instruction *createPhi(BasicBlock *BB,
                         SmallVector<Instruction *, 8> &PhiValues) {
    auto &Info = BBInfosMap[BB];
    unsigned IncomingSize = PhiValues.size();
    Type *CheckType = PhiValues[0]->getType();
    IRBuilder<> Builder(BB->getFirstNonPHI());
    PHINode *Phi = Builder.CreatePHI(CheckType, IncomingSize);
    for (unsigned i = 0; i < PhiValues.size(); i++) {
      Phi->addIncoming(PhiValues[i], Info.PreBBs[i]);
    }
    return Phi;
  }

  void updateBBInfo() {
    for (auto &Info : BBInfosMap) {
      BasicBlock *CurBB = Info.first;
      for (BasicBlock *Succ : successors(CurBB)) {
        auto &BBInfo = BBInfosMap[Succ];
        BBInfo.PreBBs.push_back(CurBB);
      }
    }
  }

  void resetLastCheckResult(Instruction *&CR, BasicBlock *CurBB, unsigned i) {
    for (BasicBlock::iterator BBInst = CurBB->begin();;) {
      if (Barriers[i] == &*BBInst)
        break;
      if (mayBeSafepoint(&*BBInst)) {
        CR = nullptr;
        break;
      }
      BBInst++;
    }
  }

  Instruction *createFastInstr(BasicBlock *TrueBranch, CallBase *CI,
                               const DebugLoc &CurDbg) {
    IRBuilder<> Builder(TrueBranch->getTerminator());
    Instruction *NewInst = createStoreOrMems(CI, Builder);
    if (NewInst != nullptr) {
      NewInst->setDebugLoc(CurDbg);
    }
    return NewInst;
  }

  void handleSuccPhi(CallBase *CI, BasicBlock *FalseBranch, Instruction *New,
                     BasicBlock *TrueBranch, BasicBlock *Succ) {
    Intrinsic::ID IID = CI->getIntrinsicID();
    if (IID != Intrinsic::cj_atomic_swap &&
        IID != Intrinsic::cj_atomic_compare_swap) {
      return;
    }
    Value *Result = nullptr;
    IRBuilder<> BuilderExtract(TrueBranch->getTerminator());
    if (IID == Intrinsic::cj_atomic_compare_swap) {
      Result = BuilderExtract.CreateExtractValue(New, 1, "swapResult");
    } else {
      Result = BuilderExtract.CreateIntToPtr(
          New, Type::getInt8Ty(CI->getContext())->getPointerTo(1));
    }
    IRBuilder<> Builder(Succ->getFirstNonPHI());
    PHINode *Phi = Builder.CreatePHI(CI->getType(), 2);
    CI->replaceAllUsesWith(Phi);
    Phi->addIncoming(CI, FalseBranch);
    Phi->addIncoming(Result, TrueBranch);
  }
  // do the following conversion:
  //   call void @llvm.cj.gcwrite
  // =====>
  //   br i1 CondVal, label %gcNoRunning, label %gcRunning
  // gcRunning:
  //   call void @llvm.cj.gcwrite
  //   br label %storeFinish
  // gcNoRunning:
  //   store xxx
  //   br label %storeFinish
  // storeFinish:
  //   ...
  BasicBlock *SplitFastPathAndSlowPath(CallBase *CurInst, BasicBlock *SplitBB,
                                       Value *CondVal) {
    const DebugLoc &CurDbg = CurInst->getDebugLoc();
    BasicBlock *FalseBranch = SplitBB->splitBasicBlock(CurInst, "gcRunning");
    BasicBlock *Succ =
        FalseBranch->splitBasicBlock(CurInst->getNextNode(), "storeFinish");
    BasicBlock *TrueBranch =
        BasicBlock::Create(C, "gcNoRunning", SplitBB->getParent(), Succ);
    BranchInst::Create(Succ, TrueBranch);
    Instruction *NewInst = createFastInstr(TrueBranch, CurInst, CurDbg);

    Instruction *OriginBr = SplitBB->getTerminator();
    IRBuilder<> BuilderBr(OriginBr);
    BuilderBr.CreateCondBr(CondVal, TrueBranch, FalseBranch);
    OriginBr->eraseFromParent();
    FalseBranch->getTerminator()->setDebugLoc(CurDbg);
    TrueBranch->getTerminator()->setDebugLoc(CurDbg);
    handleSuccPhi(CurInst, FalseBranch, NewInst, TrueBranch, Succ);
    return Succ;
  }

  void replaceBarrier() {
    if (Barriers.empty())
      return;
    // cal in state
    for (auto &Info : BBInfosMap) {
      // use pre bb's gc state value
      auto &BBInfo = Info.second;
      auto BB = Info.first;
      calculateBBCheck(BB, BBInfo);
    }

    for (unsigned i = 0; i < Barriers.size();) {
      BasicBlock *CurBB = Barriers[i]->getParent();
      auto &Info = BBInfosMap[CurBB];
      Instruction *CR = Info.InCheck;
      do {
        if (!CR) {
          CR = createCheck(Barriers[i]);
        }
        CallBase *CI = Barriers[i];
        CurBB = SplitFastPathAndSlowPath(CI, CurBB, CR);
        i++;
        if (i == Barriers.size() || CurBB != Barriers[i]->getParent()) {
          break;
        }
        resetLastCheckResult(CR, CurBB, i);
      } while (true);
    }
  }

  void calculateBBCheck(BasicBlock *BB, BBInfo &Info) {
    if (Info.Visited || (Info.HasCalls && !Info.BeforeCall)) {
      return;
    }
    Info.Visited = true;
    SmallVector<Instruction *, 8> PhiValues;
    for (auto CurBB : Info.PreBBs) {
      auto &CurInfo = BBInfosMap[CurBB];
      if (!CurInfo.HasCalls) {
        calculateBBCheck(CurBB, CurInfo);
      }
      // no barrier, and pre BB has no last barrier
      if (CurInfo.OutCheck == nullptr) {
        PhiValues.clear();
        break;
      } else {
        PhiValues.push_back(CurInfo.OutCheck);
      }
    }
    // update outcheck
    if (PhiValues.size() > 1) {
      Info.InCheck = createPhi(BB, PhiValues);
    } else if (PhiValues.size() == 1) {
      Info.InCheck = PhiValues[0];
    } else if (!Info.HasCalls && Info.LastBarrier != nullptr) {
      Info.InCheck = createCheck(Info.LastBarrier);
    }
    if (!Info.HasCalls) {
      Info.OutCheck = Info.InCheck;
    }
  }
};

class ReadBarrier {
public:
  explicit ReadBarrier(Function &F) : M(F.getParent()), C(F.getContext()) {
    const Triple TT(M->getTargetTriple());
    IsX86_64 = TT.getArch() == Triple::x86_64;
  }

  ~ReadBarrier() = default;

  void readFastPath(CallInst *ReadBarrier, Value *RefFieldPtr,
                    uint64_t Order = 0) {
    setBarrier(ReadBarrier);
    // %0 = load i8 addrspace1*, i8 addrspace1* addrspace1* %3
    // %1 = ptrtoint i8 addrspace1* %0 to i64
    IRBuilder<> Builder(ReadInst);
    LoadInst *Load = loadTaggedPointer(Builder, RefFieldPtr, Order);
    Instruction *PtrToInt =
        cast<Instruction>(Builder.CreatePtrToInt(Load, Type::getInt64Ty(C)));
    PtrToInt->setDebugLoc(*Loc);
    Value *CmpEQ = cmpTaggedPointer(PtrToInt, Builder);
    splitFastPathAndSlowPath(ReadInst->getParent(), CmpEQ, Load);
  }

  // insert a load from RefFieldPtr:
  //   %val = load i8 addrspace1*, i8 addrspace1* addrspace1* %RefFieldPtr
  LoadInst *loadTaggedPointer(IRBuilder<> &Builder, Value *RefFieldPtr,
                              uint64_t Order) {
    LoadInst *Load = Builder.CreateLoad(DstTy, RefFieldPtr);
    Load->setDebugLoc(*Loc);
    if (Order) {
      Load->setAtomic(
          (AtomicOrdering)(Order + (uint64_t)AtomicOrdering::Monotonic));
    }
    return Load;
  }

  // %tag = lshr i64 %Ptr, 48
  // %ret = icmp eq i64 %tag, 0
  Value *cmpTaggedPointer(Value *TagPtr, IRBuilder<> &Builder) {
    Value *Tag = Builder.CreateLShr(TagPtr, (uint64_t)48);
    cast<Instruction>(Tag)->setDebugLoc(*Loc);
    Value *CmpEQ = Builder.CreateICmpEQ(
        Tag, ConstantInt::get(Type::getInt64Ty(C), (uint64_t)0));
    cast<Instruction>(CmpEQ)->setDebugLoc(*Loc);
    return CmpEQ;
  }

  // preBB:
  //   %Cond = icmp eq i64 %tag, 0
  //   br i1 %Cond, label %gcNoMarked label %gcMarked
  // gcNoMarked:
  //   %val1 = %val
  //   br label %loadFinish
  // gcMarked:
  //   %val2 = call @llvm.cj.gcread.ref
  //   br label %loadFinish
  // loadFinish:
  //   %val = phi [%val1, gcNoMarked], [%val2, gcMarked]
  void splitFastPathAndSlowPath(BasicBlock *SplitBB, Value *Condition,
                                Instruction *LoadVal) {
    BasicBlock *FalseBranch = SplitBB->splitBasicBlock(ReadInst, "gcMarked");
    BasicBlock *Succ =
        FalseBranch->splitBasicBlock(ReadInst->getNextNode(), "loadFinish");
    BasicBlock *TrueBranch =
        BasicBlock::Create(C, "gcNoMarked", SplitBB->getParent(), Succ);
    BranchInst::Create(Succ, TrueBranch);
    Instruction *OriginBr = SplitBB->getTerminator();
    IRBuilder<> BuilderBr(OriginBr);
    BuilderBr.CreateCondBr(Condition, TrueBranch, FalseBranch);
    OriginBr->eraseFromParent();
    TrueBranch->getTerminator()->setDebugLoc(*Loc);
    FalseBranch->getTerminator()->setDebugLoc(*Loc);
    handleSuccPhi(FalseBranch, LoadVal, TrueBranch, Succ);
    return;
  }

  PHINode *handleSuccPhi(BasicBlock *FalseBranch, Value *FastInst,
                         BasicBlock *TrueBranch, BasicBlock *Succ) {
    IRBuilder<> Builder(Succ->getFirstNonPHI());
    PHINode *Phi = Builder.CreatePHI(DstTy, 2);
    ReadInst->replaceAllUsesWith(Phi);
    Phi->addIncoming(ReadInst, FalseBranch);
    Phi->addIncoming(FastInst, TrueBranch);
    return Phi;
  }

  void setBarrier(CallInst *CI) {
    ReadInst = CI;
    DstTy = CI->getType();
    Loc = &CI->getDebugLoc();
  }

private:
  Module *M;
  LLVMContext &C;
  bool IsX86_64;
  CallInst *ReadInst = nullptr;
  Type *DstTy = nullptr;
  const DebugLoc *Loc = nullptr;
};

/// CJBarrierLowering - This pass rewrites calls to the llvm.gcread or
/// llvm.gcwrite intrinsics, replacing them with simple loads and stores as
/// directed by the GCStrategy. It also performs automatic root initialization
/// and custom intrinsic lowering.
class CJBarrierLowering : public FunctionPass {
  bool isCJBarrier(Instruction *I);
  Instruction *createReadFastPath(BasicBlock *TrueBranch, Instruction *PtrToInt,
                                  const DebugLoc &Loc);
  PHINode *handleSuccPhi(Instruction *SlowInst, BasicBlock *FalseBranch,
                         Instruction *FastInst, BasicBlock *TrueBranch,
                         BasicBlock *Succ);
  void splitFastPathAndSlowPath(CallBase *ReadInst, BasicBlock *SplitBB,
                                Value *Condition, Instruction *PtrToInt,
                                const DebugLoc &Loc);
  void writeBarrierFastPath(Function &F, SetVector<CallInst *> &Barriers);
  void readBarrierFastPath(Function &F, SetVector<CallInst *> &Barriers);
  void doLowering(Function &F);

public:
  static char ID;
  CodeGenOpt::Level OptLevel;

  CJBarrierLowering(CodeGenOpt::Level OptLevel = CodeGenOpt::Default);
  StringRef getPassName() const override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  bool doInitialization(Module &M) override;
  bool runOnFunction(Function &F) override;
};
} // namespace

// -----------------------------------------------------------------------------

INITIALIZE_PASS_BEGIN(CJBarrierLowering, "cj-barrier-lowering",
                      "Cangjie Barrier Lowering", false, false)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_END(CJBarrierLowering, "cj-barrier-lowering",
                    "Cangjie Barrier Lowering", false, false)

char CJBarrierLowering::ID = 0;
char &llvm::CJBarrierLoweringID = CJBarrierLowering::ID;

FunctionPass *llvm::createCJBarrierLoweringPass(CodeGenOpt::Level OptLevel) {
  return new CJBarrierLowering(OptLevel);
}

CJBarrierLowering::CJBarrierLowering(CodeGenOpt::Level OptLevel)
    : FunctionPass(ID), OptLevel(OptLevel) {
  initializeCJBarrierLoweringPass(*PassRegistry::getPassRegistry());
}

StringRef CJBarrierLowering::getPassName() const {
  return "Cangjie Lower Garbage Collection Barrier Function";
}

void CJBarrierLowering::getAnalysisUsage(AnalysisUsage &AU) const {
  FunctionPass::getAnalysisUsage(AU);
  if (EnableGCStateLoop) {
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addPreserved<LoopInfoWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addPreserved<DominatorTreeWrapperPass>();
    AU.addRequired<ScalarEvolutionWrapperPass>();
    AU.addPreserved<ScalarEvolutionWrapperPass>();
  }
}

/// doInitialization - If this module uses the GC intrinsics, find them now.
bool CJBarrierLowering::doInitialization(Module &M) {
  Function *GCStateCheckFunc = M.getFunction("GetGCPhase");
  if (GCStateCheckFunc != nullptr) { // have GetGCPhase in module
    return false;
  }
  LLVMContext &C = M.getContext();
  FunctionType *FuncType = FunctionType::get(Type::getInt32Ty(C), false);
  GCStateCheckFunc =
      cast<Function>(M.getOrInsertFunction("GetGCPhase", FuncType).getCallee());
  GCStateCheckFunc->addFnAttr(Attribute::get(C, "gc-leaf-function"));
  GCStateCheckFunc->addFnAttr(Attribute::get(C, "cj-runtime"));
  GCStateCheckFunc->setCallingConv(CallingConv::CangjieGC);
  GCStateCheckFunc->setUnnamedAddr(GlobalValue::UnnamedAddr::Local);
  return false;
}

static bool fastBarrierInline(Function &F, GCPhaseCheck &GCPhase) {
  bool Changed = false;
  GCPhase.initBBInfo(F);
  for (BasicBlock &BB : F) {
    for (auto It = BB.begin(), E = BB.end(); It != E;) {
      auto *CI = dyn_cast<CallBase>(&*It++);
      if (!CI) {
        continue;
      }
      if (mayBeSafepoint(CI)) {
        GCPhase.setHasCall(&BB);
      }
      unsigned IID = CI->getIntrinsicID();
      switch (IID) {
      default:
        break;
      case Intrinsic::cj_gcwrite_ref:
      case Intrinsic::cj_gcwrite_static_ref:
      case Intrinsic::cj_gcwrite_struct:
      case Intrinsic::cj_gcwrite_static_struct:
        if (DisableGCSupport || EnableSafepointOnly) {
          IRBuilder<> Builder(CI);
          createStoreOrMems(CI, Builder);
          CI->eraseFromParent();
          Changed = true;
        } else {
          Changed |= GCPhase.fastBarrier(IID, CI);
        }
        continue;
      case Intrinsic::cj_array_copy_ref:
      case Intrinsic::cj_array_copy_struct:
      case Intrinsic::cj_atomic_store:
        Changed |= GCPhase.fastBarrier(IID, CI);
        continue;
      case Intrinsic::cj_atomic_swap:
      case Intrinsic::cj_atomic_compare_swap:
        // Do not implement fastpath currently now!
        continue;
      }
    }
    GCPhase.updateBBOut(&BB);
  }
  GCPhase.finishAll();
  return Changed;
}

static SmallVector<CallBase *, 8> GCWriteBarriers;

// on safepoint and contain barriers's loop
static bool containBarrier(Loop &L, bool &containSafepoint) {
  if (!L.isInnermost() || L.getNumBlocks() > MaxLoopBlock ||
      !L.getLoopPreheader()) {
    return false;
  }
  GCWriteBarriers.clear();
  BasicBlock *Latch = L.getLoopLatch();
  assert(Latch && "Simplified loops only have one latch!");
  if (Latch->getTerminator()->getMetadata(ClonedPinLoopTag)) {
    return false;
  }
  Value *BP = nullptr;
  for (auto *LoopBB : L.blocks()) {
    for (auto It = LoopBB->begin(), E = LoopBB->end(); It != E;) {
      auto *CI = dyn_cast<CallBase>(&*It++);
      if (!CI) {
        continue;
      }
      containSafepoint |= mayBeSafepoint(CI);
      unsigned IID = CI->getIntrinsicID();
      switch (IID) {
      default:
        break;
      case Intrinsic::cj_gcwrite_ref:
        BP = getBaseObj(CI);
        break;
      case Intrinsic::cj_gcwrite_static_ref:
        BP = getPointerArg(CI);
        break;
      case Intrinsic::cj_gcwrite_struct:
        BP = getBaseObj(CI);
        break;
      case Intrinsic::cj_gcwrite_static_struct:
        BP = getDest(CI);
        break;
      case Intrinsic::cj_array_copy_ref:
      case Intrinsic::cj_array_copy_struct:
        BP = CI->getArgOperand(ArrayCopy::DstObj);
        break;
      }
      if (BP == nullptr) {
        continue;
      }
      GCWriteBarriers.push_back(CI);
      BP = nullptr;
    }
  }
  return GCWriteBarriers.size() != 0;
}

static Loop &cloneLoop(Loop *L, Function &F, LoopInfo &LI) {
  ValueToValueMapTy Map;
  SmallVector<BasicBlock *, 64> Blocks;
  for (BasicBlock *BB : L->getBlocks()) {
    BasicBlock *Clone = CloneBasicBlock(BB, Map, Twine(".pin"), &F);
    Blocks.push_back(Clone);
    Map[BB] = Clone;
  }
  auto GetClonedValue = [&Map](Value *V) {
    assert(V && "null values not in domain!");
    auto It = Map.find(V);
    if (It == Map.end()) {
      return V;
    }
    return static_cast<Value *>(It->second);
  };
  auto *ClonedLatch = cast<BasicBlock>(GetClonedValue(L->getLoopLatch()));
  LLVMContext &Ctx = L->getHeader()->getContext();
  ClonedLatch->getTerminator()->setMetadata(ClonedPinLoopTag,
                                            MDNode::get(Ctx, {}));
  for (unsigned i = 0, e = Blocks.size(); i != e; ++i) {
    BasicBlock *ClonedBB = Blocks[i];
    BasicBlock *OriginalBB = L->getBlocks()[i];
    for (Instruction &I : *ClonedBB) {
      RemapInstruction(&I, Map,
                       RF_NoModuleLevelChanges | RF_IgnoreMissingLocals);
    }
    for (auto *SBB : successors(OriginalBB)) {
      if (L->contains(SBB)) {
        continue;
      }
      for (PHINode &PN : SBB->phis()) {
        Value *OldIncoming = PN.getIncomingValueForBlock(OriginalBB);
        PN.addIncoming(GetClonedValue(OldIncoming), ClonedBB);
      }
    }
  }
  Loop &New = *LI.AllocateLoop();
  if (L->getParentLoop()) {
    L->getParentLoop()->addChildLoop(&New);
  } else {
    LI.addTopLevelLoop(&New);
  }
  for (auto *BB : L->blocks()) {
    if (LI.getLoopFor(BB) == L) {
      New.addBasicBlockToLoop(cast<BasicBlock>(Map[BB]), LI);
    }
  }
  return New;
}

static void handleGCStateLoop(Loop &New, Loop &L, GCPhaseCheck &GCPhase) {
  BasicBlock *PreHeader = L.getLoopPreheader();
  BranchInst *PreTerm = dyn_cast<BranchInst>(PreHeader->getTerminator());
  assert(PreTerm != nullptr && "PreHeader's trem inst is not branch inst");
  BasicBlock *Header = L.getHeader();
  assert(!PreTerm->isConditional() && "PreHeader's term has conditional");
  GCPhase.createGCCheck(PreHeader, Header, New.getHeader());
  PreTerm->eraseFromParent();
}

static void replaceBarriers() {
  for (unsigned Index = 0; Index < GCWriteBarriers.size(); Index++) {
    CallBase *CI = GCWriteBarriers[Index];
    IRBuilder<> IRB(CI);
    Instruction *Inst = createStoreOrMems(CI, IRB);
    if (Inst != nullptr) {
      Inst->setDebugLoc(CI->getDebugLoc());
      CI->eraseFromParent();
    }
  }
}

static void checkLoopBarrier(Function &F, LoopInfo &LI, DominatorTree &DT,
                             ScalarEvolution &SE, GCPhaseCheck &GCPhase) {
  if (LI.empty()) {
    return;
  }

  for (auto L : LI) {
    simplifyLoop(L, &DT, &LI, &SE, nullptr, nullptr, false);
    formLCSSARecursively(*L, DT, &LI, &SE);
  }

  SmallPriorityWorklist<Loop *, 4> Worklist;
  appendLoopsToWorklist(LI, Worklist);

  while (!Worklist.empty()) {
    Loop *L = Worklist.pop_back_val();
    bool containSafepoint = false;
    if (!containBarrier(*L, containSafepoint)) {
      continue;
    }
    if (!containSafepoint && EnableGCStateLoop) {
      Loop &New = cloneLoop(L, F, LI);
      handleGCStateLoop(New, *L, GCPhase);
      replaceBarriers();
      DT.recalculate(F);
      formLCSSARecursively(New, DT, &LI, &SE);
      simplifyLoop(&New, &DT, &LI, &SE, nullptr, nullptr, true);
      formLCSSARecursively(*L, DT, &LI, &SE);
      simplifyLoop(L, &DT, &LI, &SE, nullptr, nullptr, true);
    }
  }
}

void CJBarrierLowering::writeBarrierFastPath(Function &F,
                                             SetVector<CallInst *> &Barriers) {
  if (!EnableGCPhase || CangjieJIT)
    return;

  GCPhaseCheck GCPhase(F);
  if (EnableGCStateLoop) {
    auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    auto &DT = getAnalysisIfAvailable<DominatorTreeWrapperPass>()->getDomTree();
    auto &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();
    checkLoopBarrier(F, LI, DT, SE, GCPhase);
  }
  fastBarrierInline(F, GCPhase);
}

// do the following conversion:
//   %val = call void @llvm.cj.gcread.ref
// =====>
//   %0 = load i8 addrspace1*, i8 addrspace1* addrspace1* %RefFieldPtr
//   %1 = ptrtoint i8 addrspace1* %0 to i64
//   %2 = lshr i64 %1, 48
//   %3 = icmp eq i64 %2, 0
//   br i1 %3, label %gcNoMarked label %gcMarked
// gcNoMarked:
//   %val1 = and %0, 0x0000ffffffffffff
//   br label %loadFinish
// gcMarked:
//   %val2 = call @llvm.cj.gcread.ref
//   br label %loadFinish
// loadFinish:
//   %val = phi [%val1, gcNoMarked], [%val2, gcMarked]
void CJBarrierLowering::readBarrierFastPath(Function &F,
                                            SetVector<CallInst *> &Barriers) {
  if (!EnableTaggedPointer || CangjieJIT)
    return;

  ReadBarrier RB(F);
  for (CallInst *CI : Barriers) {
    unsigned ID = CI->getIntrinsicID();
    if (ID == Intrinsic::cj_gcread_ref ||
        ID == Intrinsic::cj_gcread_static_ref) {
      RB.readFastPath(CI, getPointerArg(CI));
      continue;
    }
    if (ID == Intrinsic::cj_atomic_load) {
      if (auto AO = dyn_cast<ConstantInt>(getAtomicOrder(CI))) {
        RB.readFastPath(CI, CI->getArgOperand(AtomicLoad::Field),
                        AO->getZExtValue());
      }
    }
  }
}

void CJBarrierLowering::doLowering(Function &F) {
  Module *M = F.getParent();
  CJStructTypeGCInfo GCInfo(*M);
  BarrierMaker GCBarrier(*M, GCInfo);

  for (BasicBlock &BB : F) {
    for (Instruction &I : llvm::make_early_inc_range(BB)) {
      if (isCJBarrier(&I))
        GCBarrier.replaceInstWithBarrier(cast<IntrinsicInst>(&I));
    }
  }
}

bool CJBarrierLowering::isCJBarrier(Instruction *I) {
  IntrinsicInst *CI = dyn_cast<IntrinsicInst>(I);
  if (!CI)
    return false;

  switch (CI->getIntrinsicID()) {
  default:
    return false;
  case Intrinsic::cj_gcwrite_ref:
  case Intrinsic::cj_gcwrite_struct:
  case Intrinsic::cj_gcwrite_static_ref:
  case Intrinsic::cj_gcwrite_static_struct:
  case Intrinsic::cj_gcread_ref:
  case Intrinsic::cj_gcread_weakref:
  case Intrinsic::cj_gcread_struct:
  case Intrinsic::cj_gcread_static_ref:
  case Intrinsic::cj_gcread_static_struct:
  case Intrinsic::cj_copy_struct_field:
  case Intrinsic::cj_atomic_store:
  case Intrinsic::cj_atomic_load:
  case Intrinsic::cj_atomic_swap:
  case Intrinsic::cj_atomic_compare_swap:
  case Intrinsic::cj_array_copy_ref:
  case Intrinsic::cj_array_copy_struct:
  case Intrinsic::cj_array_copy_generic:
  case Intrinsic::cj_assign_generic:
  case Intrinsic::cj_gcwrite_generic:
  case Intrinsic::cj_gcread_generic:
  case Intrinsic::cj_gcwrite_generic_payload:
    return true;
  }
}

static void replaceFastFunc(Function &F, GCStatepointInst *CI,
                            StringRef FuncName) {
  Module *M = F.getParent();
  Function *Func = M->getFunction(FuncName);
  if (Func == nullptr) {
    Function *Callee = CI->getActualCalledFunction();
    Func = M->declareCJRuntimeFunc(FuncName, Callee->getFunctionType(), true);
    Func->setLinkage(GlobalValue::InternalLinkage);
    Func->addFnAttr(Attribute::NoInline);
    Func->addFnAttr("cj-fast-new-obj");
    BasicBlock *BB = BasicBlock::Create(M->getContext(), "entry", Func);
    IRBuilder<> IRB(BB);
    auto Ret = IRB.CreateCall(Callee, {Func->getArg(0), Func->getArg(1)});
    IRB.CreateRet(Ret);
  }
  CI->setArgOperand(GCStatepointInst::CalledFunctionPos, Func);
}

static bool doNewFastPath(Function &F, SetVector<GCStatepointInst *> &NewObjs) {
  if (CangjieJIT)
    return false;
  for (GCStatepointInst *CI : NewObjs) {
    Function *Callee = CI->getActualCalledFunction();
    if (Callee->getName().equals("CJ_MCC_NewObject")) {
      replaceFastFunc(F, CI, NewObjFastStr);
    } else {
      replaceFastFunc(F, CI, NewObjFinalizerFastStr);
    }
  }
  return true;
}

static bool isNewObj(Instruction *I) {
  if (auto CI = dyn_cast<GCStatepointInst>(I)) {
    Function *Callee = CI->getActualCalledFunction();
    if (Callee && Callee->getName().isCangjieNewObjFunction()) {
      return true;
    }
  }
  return false;
}

static bool isSafepointCall(Instruction *I) {
  if (auto *CI = dyn_cast<GCStatepointInst>(I)) {
    Function *Callee = CI->getActualCalledFunction();
    if (Callee && Callee->isCangjieSafePoint())
      return true;
  }
  return false;
}

static Function *getOrInsertSafepointStub(Module *M, Function *Callee) {
  if (Function *F = M->getFunction(SafepointStub))
    return F;
  FunctionType *FuncType =
      FunctionType::get(Type::getVoidTy(M->getContext()), false);
  Function *F = cast<Function>(
      M->getOrInsertFunction(SafepointStub, FuncType).getCallee());
  F->addFnAttr(Attribute::NoInline);
  F->addFnAttr(Attribute::Naked);
  F->setLinkage(GlobalValue::PrivateLinkage);
  // Create function body.
  BasicBlock *BB = BasicBlock::Create(M->getContext(), "", F);
  IRBuilder<> IRB(BB);
  IRB.CreateCall(Callee);
  // Ret does not need to be generated, but the verification fails.
  IRB.CreateRetVoid();
  return F;
}

static bool combineSafepointStub(Module *M,
                                 SetVector<GCStatepointInst *> &Safepoints) {
  if (Safepoints.empty())
    return false;
  Function *F = getOrInsertSafepointStub(
      M, Safepoints.front()->getActualCalledFunction());
  for (auto *SI : Safepoints) {
    // Although the callee is replaced, the call is still a safepoint.
    SI->setArgOperand(GCStatepointInst::CalledFunctionPos, F);
    SI->setArgOperand(GCStatepointInst::IDPos,
                      ConstantInt::get(Type::getInt64Ty(M->getContext()),
                                       Cangjie::CJStatepointID::SafepointStub));
  }
  return true;
}

bool CJBarrierLowering::runOnFunction(Function &F) {
  // Quick exit for functions that do not use Cangjie GC.
  if (!F.hasCangjieGC())
    return false;

  const Triple TT(F.getParent()->getTargetTriple());
  if (TT.isARM()){
    EnableTaggedPointer = false;
    EnableGCPhase = false;
    EnableGCFastPath = false;
  }

  bool Changed = false;
  SetVector<CallInst *> Barriers;
  SetVector<GCStatepointInst *> News;
  SetVector<GCStatepointInst *> Safepoints;

  for (BasicBlock &BB : F) {
    for (Instruction &I : llvm::make_early_inc_range(BB)) {
      if (isCJBarrier(&I))
        Barriers.insert(cast<CallInst>(&I));
      else if (EnableGCFastPath && isNewObj(&I))
        News.insert(cast<GCStatepointInst>(&I));
      else if (EnableSafepointOutline && isSafepointCall(&I))
        Safepoints.insert(cast<GCStatepointInst>(&I));
    }
  }

  if (!News.empty()) {
    Changed = doNewFastPath(F, News);
  }

  if (!Safepoints.empty() && !TT.isARM())
    Changed |= combineSafepointStub(F.getParent(), Safepoints);

  if (Barriers.empty()) {
    return Changed;
  }

  if (OptLevel != CodeGenOpt::None) {
    writeBarrierFastPath(F, Barriers);
    readBarrierFastPath(F, Barriers);
  }
  doLowering(F);
  return true;
}
