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
#include "llvm/Transforms/Scalar/CJFillMetadata.h"
#include "llvm/Transforms/Utils/CallGraphUpdater.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Scalar.h"

#include <algorithm>

#define DEBUG_TYPE "escape-analysis"
using namespace llvm;
constexpr int alignEight = 8;
enum InitializeState : unsigned { NotInitialize, Initializing, Initialized };
static cl::opt<unsigned> CJMaxStackObject("cj-max-stack-obj", cl::Hidden,
                                          cl::init(1024));
static cl::opt<unsigned> CJMaxNonMoveArraySize("cj-max-nonmove-array-size",
                                               cl::Hidden, cl::init(64 * 1024));
namespace llvm {
cl::opt<bool> CJDisablePEA("cj-disable-partial-ea",
                                  cl::Hidden, cl::init(false));

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
  LLVMContext &C = M ->getContext();
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
      FuncName.startswith("CJ_MCC_NewObjArray")) {
    return true;
  }
  return false;
}

class NonEscapeRewriter : public InstVisitor<NonEscapeRewriter> {
  friend class InstVisitor<NonEscapeRewriter>;

public:
  explicit NonEscapeRewriter(
      CallGraphUpdater *CGUpdater, Module *M)
      : CGUpdater(CGUpdater), M(M) {
    CJMemset = Intrinsic::getDeclaration(
        M, Intrinsic::cj_memset,
        {IntegerType::getInt8PtrTy(M->getContext(), 0)});
  }

  bool rewrite(Instruction *Old, BasicBlock *NewInstInsertBB) {
    OldInst = Old;
    Visited.clear();
    if (!createNewAllocaInst(NewInstInsertBB)) {
      return false;
    }
    rewriteNewInst();
    return true;
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
  bool StructAS1Replace = false;
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
void doPrintFuncObjects(SmallVector<ObjectLocation *, 8> &AllLocations) {
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
        Changed |= Rewriter.rewrite(dyn_cast<Instruction>(New->Val),
                                    New->NewInstInsertBB);
      }
    }

    Rewriter.setStructAS1Flag();
    for (unsigned i = 0; i < StructCastToAS1.size(); i++) {
      Changed |= Rewriter.handleStructAS1Cast(StructCastToAS1[i]);
    }

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

  bool isValueInvalid(Value *V) {
    if (V->getValueID() == llvm::Value::UndefValueVal) {
      report_fatal_error("store/gcwrite an undef value!");
    }
    return V->getValueID() == llvm::Value::ConstantPointerNullVal;
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
      Loc->Escaped = true;
      Loc->EscapedInst = I;
      Loc->EscapeState = EscapeOrigin | EscapeGlobal;
      escapeHeap(*Loc);
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
      Loc->Escaped = true;
      Loc->EscapedInst = &EVT;
      Loc->EscapeState = EscapeOrigin | EscapeGlobal;
      escapeHeap(*Loc);
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

  SmallPtrSet<Function *, 8> SCCFunctions;

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
        Loc->Escaped = true;
        Loc->EscapedInst = I;
        Loc->EscapeState |= EscapeOrigin | EscapeGlobal;
        escapeHeap(*Loc);
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
        Loc->Escaped = true;
        Loc->EscapedInst = I;
        Loc->EscapeState |= EscapeOrigin | EscapeGlobal;
        escapeHeap(*Loc);
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
    Loc->Escaped = true;
    Loc->EscapedInst = II;
    Loc->EscapeState |= EscapeOrigin | EscapeGlobal;
    escapeHeap(*Loc);
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
                               DenseMap<GCPtr *, unsigned> &SuccBBInfo,
                               GCPtr *P, SmallVector<int, 8> &Offset,
                               bool Direct);

static void countRefOffsets(StructType *ST, SmallVector<uint64_t, 8> &Offsets,
                            uint64_t BaseOff, const DataLayout &DL,
                            unsigned Left, unsigned Right);

static void countArrayOffsets(ArrayType *AT, SmallVector<uint64_t, 8> &Offsets,
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

static void countRefOffsets(StructType *ST, SmallVector<uint64_t, 8> &Offsets,
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
        LoopDepth(Depth), EA(EAImpl) {
    if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(Val))
      IsLoad |= II->getIntrinsicID() == Intrinsic::cj_gcread_ref;
  }

  virtual ~GCPtr() = default;

  virtual bool equal(GCPtr *V) { return V->isPtr() && (P == V->P); }

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
      EAImpl.EscapeBBInfo[&CurFunc->getEntryBlock()][New] = Escaped;
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
                                DenseMap<GCPtr *, unsigned> &SuccBBInfo,
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
      for (auto &V : BaseValue) {
        V.first->spreadInfoEscape(BB, ES, SuccBBInfo, true);
      }
    }
    if (Alias.size() != 0) {
      for (auto V : Alias) {
        V.first->spreadInfoEscape(BB, ES, SuccBBInfo, false);
      }
    }
    for (auto &V : InfoEscapedVec) {
      V->spreadInfoEscape(BB, ES, SuccBBInfo, Direct);
    }
  }

  virtual void spreadMemEscape(BasicBlock *BB, unsigned ES,
                               DenseMap<GCPtr *, unsigned> &SuccBBInfo,
                               SmallVector<int, 8> &Offsets,
                               bool Direct = true) {
    if (IsVisiting) {
      return;
    }
    IsVisiting = true;
    int CurOffset = Offsets.pop_back_val();
    if (CurOffset >= 0) {
      Offsets.push_back(CurOffset);
      memPtrspreadEscape(BB, ES, SuccBBInfo, this, Offsets, Direct);
      IsVisiting = false;
      return;
    }
    if (isPhiOrSelect()) {
      if (Offsets.size() > 0) {
        for (unsigned I = 0; I < MPs.size(); ++I)
          MPs[I]->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, Direct);
        for (auto &V : InfoEscapedVec) {
          for (unsigned I = 0; I < V->MPs.size(); ++I)
            V->MPs[I]->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, Direct);
        }
      } else {
        for (unsigned I = 0; I < MPs.size(); ++I)
          MPs[I]->spreadEscape(BB, ES, SuccBBInfo, true);
        for (auto &V : InfoEscapedVec) {
          for (unsigned I = 0; I < V->MPs.size(); ++I)
            V->MPs[I]->spreadEscape(BB, ES, SuccBBInfo, Direct);
        }
      }
      Offsets.push_back(CurOffset);
      if (Direct) {
        for (auto &V : BaseValue) {
          V.first->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, Direct);
        }
      }
      for (auto V : Alias) {
        V.first->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, Direct);
      }
      IsVisiting = false;
      return;
    }
    if (Offsets.size() > 0) {
      for (unsigned I = 0; I < MPs.size(); ++I)
        MPs[I]->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, Direct);
      if (Direct) {
        for (auto &V : BaseValue) {
          for (unsigned I = 0; I < V.first->MPs.size(); ++I)
            V.first->MPs[I]->spreadMemEscape(BB, ES, SuccBBInfo, Offsets,
                                             Direct);
        }
      }
      for (auto V : Alias) {
        for (unsigned I = 0; I < V.first->MPs.size(); ++I)
          V.first->MPs[I]->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, Direct);
      }
      for (auto &V : InfoEscapedVec) {
        for (unsigned I = 0; I < V->MPs.size(); ++I)
          V->MPs[I]->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, Direct);
      }
      Offsets.push_back(CurOffset);
    } else {
      for (unsigned I = 0; I < MPs.size(); ++I)
        MPs[I]->spreadEscape(BB, ES, SuccBBInfo, true);
      if (Direct) {
        for (auto &V : BaseValue) {
          for (unsigned I = 0; I < V.first->MPs.size(); ++I)
            V.first->MPs[I]->spreadEscape(BB, ES, SuccBBInfo, true);
        }
      }
      for (auto V : Alias) {
        for (unsigned I = 0; I < V.first->MPs.size(); ++I)
          V.first->MPs[I]->spreadEscape(BB, ES, SuccBBInfo, Direct);
      }
      for (auto &V : InfoEscapedVec) {
        for (unsigned I = 0; I < V->MPs.size(); ++I)
          V->MPs[I]->spreadEscape(BB, ES, SuccBBInfo, Direct);
      }
      Offsets.push_back(CurOffset);
    }
    IsVisiting = false;
  }

  virtual void spreadEscape(BasicBlock *BB, unsigned ES,
                            DenseMap<GCPtr *, unsigned> &SuccBBInfo,
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
      for (auto &V : BaseValue) {
        if (!SuccBBInfo.count(V.first) || SuccBBInfo[V.first] < ES ||
            canSpread(V.first, ES)) {
          SuccBBInfo[V.first] = ES;
          V.first->spreadEscape(BB, ES, SuccBBInfo, true);
        }
      }
    }
    if (Alias.size() != 0) {
      for (auto &V : Alias) {
        if (!SuccBBInfo.count(V.first) || SuccBBInfo[V.first] < ES) {
          SuccBBInfo[V.first] = ES;
          V.first->spreadEscape(BB, ES, SuccBBInfo, false);
        }
      }
    }
    for (auto &V : InfoEscapedVec) {
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
  bool IsVisiting = false;
  int MemOff = 0;
  InitializeState InitializeFlag = NotInitialize;
  unsigned LoopDepth;
  CJEscapeAnalysis &EA;
  // for load
  DenseMap<GCPtr *, SmallSet<uint64_t, 2>> BaseValue;
  // Ptr's memPtr info
  SmallVector<GCPtr *, 8> MPs;
  // pass to other instruction, such as
  // %3 = phi [%1, %bb1], [%2, %bb2]
  // which %1， %2's obj Alias stored %3.
  // Value is the offset of %1's base and %3
  DenseMap<GCPtr *, SmallSet<int, 2>> Alias;
  SmallSetVector<GCPtr *, 8> InfoEscapedVec;
};

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
                        LoopInfo *LI = nullptr) {
    if (EAImpl.AllMemLocInfo.count(std::make_pair(Val, Off))) {
      return EAImpl.AllMemLocInfo[std::make_pair(Val, Off)];
    }
    GCPtr *P = GCPtr::create(Val, EAImpl, LI);
    if (P == nullptr)
      return nullptr;
    if (Off < 0) {
      Off = --P->MemOff;
    }
    MemPtr *New = new MemPtr(Val, Off, P->LoopDepth, EAImpl, P);
    EAImpl.AllMemLocInfo[std::make_pair(Val, Off)] = New;
    P->insertMem(New);
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

  void insertDefine(BasicBlock *BB, GCPtr *Val) {
    DefineValues[BB].push_back(Val);
  }

  void spreadInfoEscape(BasicBlock *BB, unsigned ES,
                        DenseMap<GCPtr *, unsigned> &SuccBBInfo,
                        bool Direct = true) override {
    if (isDerivedPtr() && Direct && DirectSpread < InfoEscaped) {
      DirectSpread = InfoEscaped;
    } else if (SuccBBInfo.count(this) && SuccBBInfo[this] >= InfoEscaped) {
      return;
    }
    if (SuccBBInfo[this] < InfoEscaped) {
      SuccBBInfo[this] = InfoEscaped;
    }
    for (auto DefineMap : DefineValues) {
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
    if (Direct) {
      for (auto &V : Base->BaseValue) {
        for (auto BaseValueOffset : V.second) {
          Offsets.push_back(BaseValueOffset + Offset);
          V.first->spreadMemEscape(BB, ES, SuccBBInfo, Offsets);
          Offsets.pop_back();
        }
      }
    }
    if (Base->Alias.size() != 0) {
      for (auto &V : Base->Alias) {
        for (auto AliasOffset : V.second) {
          if (Offset < AliasOffset) {
            continue;
          }
          if (AliasOffset != -1) {
            Offsets.push_back(Offset - AliasOffset);
          } else {
            Offsets.push_back(AliasOffset);
          }
          V.first->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, false);
          Offsets.pop_back();
        }
      }
    }
  }

  void spreadMemEscape(
      BasicBlock *BB, unsigned ES, DenseMap<GCPtr *, unsigned> &SuccBBInfo,
      SmallVector<int, 8> &Offsets, bool Direct) override {
    if (IsVisiting) {
      return;
    }
    IsVisiting = true;
    int CurOffset = Offsets.back();
    for (auto DefineMap : DefineValues) {
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
    Offsets.push_back(Offset);
    Base->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, Direct);
    Offsets.pop_back();
    if (Offset < 0) {
      IsVisiting = false;
      return;
    }
    if (Direct) {
      for (auto &V : Base->BaseValue) {
        for (auto BaseValueOffset : V.second) {
          Offsets.push_back(BaseValueOffset + Offset);
          V.first->spreadMemEscape(BB, ES, SuccBBInfo, Offsets);
          Offsets.pop_back();
        }
      }
    }
    if (Base->Alias.size() != 0) {
      for (auto &V : Base->Alias) {
        for (auto AliasOffset : V.second) {
          if (Offset < AliasOffset) {
            continue;
          }
          if (AliasOffset != -1) {
            Offsets.push_back(Offset - AliasOffset);
          } else {
            Offsets.push_back(AliasOffset);
          }
          V.first->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, false);
          Offsets.pop_back();
        }
      }
    }
    IsVisiting = false;
  }

  void spreadEscape(BasicBlock *BB, unsigned ES,
                    DenseMap<GCPtr *, unsigned> &SuccBBInfo,
                    bool Direct) override {
    if (isDerivedPtr() && Direct && DirectSpread < ES) {
      DirectSpread = ES;
    }
    for (auto DefineMap : DefineValues) {
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
    if (Direct) {
      for (auto &V : Base->BaseValue) {
        for (auto BaseValueOffset : V.second) {
          Offsets.push_back(BaseValueOffset + Offset);
          V.first->spreadMemEscape(BB, ES, SuccBBInfo, Offsets);
          Offsets.pop_back();
        }
      }
    }

    if (Base->Alias.size() != 0) {
      for (auto &V : Base->Alias) {
        for (auto AliasOffset : V.second) {
          if (Offset < AliasOffset) {
            continue;
          }
          if (AliasOffset != -1) {
             Offsets.push_back(Offset - AliasOffset);
           } else {
             Offsets.push_back(-1);
           }
           V.first->spreadMemEscape(BB, ES, SuccBBInfo, Offsets, false);
           Offsets.pop_back();
        }
      }
    }
  }

  bool isDerivedPtr() override { return Base->isDerivedPtr(); }
  GCPtr *Base;
  int Offset;
  DenseMap<BasicBlock *, SmallVector<GCPtr *, 8>> DefineValues;
};

static void memPtrspreadEscape(
    BasicBlock *BB, unsigned ES, DenseMap<GCPtr *, unsigned> &SuccBBInfo,
    GCPtr *P, SmallVector<int, 8> &Offsets, bool Direct) {
  int CurOffset = Offsets.back();
  MemPtr *MPtr = MemPtr::create(P->P, CurOffset, P->EA);
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

void CJEscapeAnalysis::setInfoEscaped(GCPtr *GP, BasicBlock *BB) {
  GP->setInfoEscaped();
  if (!EscapeBBInfo[BB].count(GP)) {
    EscapeBBInfo[BB][GP] = NotEscape;
  }
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
        case TransEscaped:
          LLVM_DEBUG(dbgs() << "TransEscaped\n");
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
        for (auto BBInfo : EscapeBBInfo[Pred]) {
          if (BBInfo.second >= TransEscaped &&
              (!SuccBBInfo.count(BBInfo.first) ||
               SuccBBInfo[BBInfo.first] < TransEscaped)) {
            Changed = true;
            SuccBBInfo[BBInfo.first] = TransEscaped;
            // obj stored to Escaped or TransEscaped is to be Escaped.
            BBInfo.first->spreadEscape(&BB, TransEscaped, SuccBBInfo, true);
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

  bool run() {
    bool Changed = false;
    for (Function *Func : SCCFunctions) {
      ProcessedFunc = Func;
      handleLiveness(Func);
    }
    for (auto LoadPtr : LoadValues) {
      processLoadNew(LoadPtr);
    }
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
        Rewriter.rewrite(Partial.first, Partial.second);
        Changed = true;
      } else {
        LLVM_DEBUG(dbgs() << "partial escaped obj:");
        LLVM_DEBUG(dbgs() << "  " << *Partial.first << "\n");
      }
    }

    for (auto PInfo : AllPtrLocInfo) {
      delete PInfo.second;
    }
    AllPtrLocInfo.clear();
    for (auto MemInfo : AllMemLocInfo) {
      delete MemInfo.second;
    }
    AllMemLocInfo.clear();
    return Changed;
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

  bool isValueInvalid(Value *V) {
    if (V->getValueID() == llvm::Value::UndefValueVal) {
      report_fatal_error("store/gcwrite an undef value!");
    }
    return V->getValueID() == llvm::Value::ConstantPointerNullVal;
  }

  void bindInfoEscape(GCPtr *P, GCPtr *V, BasicBlock *BB) {
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

  void handleGCWrite(IntrinsicInst *II) {
    Value *V = II->getOperand(0);
    Type *T = V->getType();
    if (!T->isPointerTy() || T->getPointerAddressSpace() != 1 ||
        isValueInvalid(V)) {
      return;
    }
    Value *P = II->getOperand(2);
    GCPtr *GP = GCPtr::create(P, *this, LI);
    GCPtr *GV = GCPtr::create(V, *this, LI);
    if (isEscapedValue(P) || GP->LoopDepth < GV->LoopDepth) {
      if (auto VI = dyn_cast<Instruction>(V)) {
        EscapeBBInfo[II->getParent()][GV] = Escaped;
      }
    }
    int Offset = 0;
    Value *BaseP = getBaseValue(P, Offset); // direct get from intrinsic
    MemPtr *MP = MemPtr::create(BaseP, Offset, *this, LI);
    MP->insertDefine(II->getParent(), GV);

    if (isEscapedValue(V)) {
      EscapeBBInfo[II->getParent()][MP] = Escaped;
    }

    StoreRelations[II->getParent()][P].insert(V);
    if (Offset >=0 && !Def[II->getParent()][BaseP][Offset].empty()) {
      Def[II->getParent()][BaseP][Offset].clear();
    }
    Def[II->getParent()][BaseP][Offset].insert(GV);
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
    EscapeBBInfo[II->getParent()][GCPtr::create(V, *this, LI)] = Escaped;
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
      EscapeBBInfo[I->getParent()][PI] = Escaped;
    }

    for (unsigned Idx = 0; Idx < I->getNumOperands(); Idx++) {
      auto V = I->getOperand(Idx);
      if (!(isGCPointerType(V->getType()) ||
            isMemoryContainsGCPtrType(V->getType())))
        continue;
      GCPtr *Base = GCPtr::create(getBaseValue(V, Offset), *this, LI);
      if (Base != nullptr) {
        EscapeBBInfo[I->getParent()][Base] = Escaped;
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
        EscapeBBInfo[PN.getParent()][P] = Escaped;
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
              EscapeBBInfo[PN.getParent()][PhiInPtr] = Escaped;
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
      EscapeBBInfo[SI.getParent()][P] = Escaped;
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
      EscapeBBInfo[CurBB][LoadPtr] = Escaped;
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
    GCPtr *GP = GCPtr::create(P, *this, LI);
    GCPtr *GV = GCPtr::create(V, *this, LI);
    if (isEscapedValue(P) || GP->LoopDepth < GV->LoopDepth) {
      if (auto VI = dyn_cast<Instruction>(V)) {
        EscapeBBInfo[SI.getParent()][GV] = Escaped;
      }
    }
    int Offset = 0;
    Value *BaseP = getBaseValue(P, Offset);
    MemPtr *MP = MemPtr::create(BaseP, Offset, *this, LI);
    MP->insertDefine(SI.getParent(), GV);

    if (isEscapedValue(V)) {
      EscapeBBInfo[SI.getParent()][MP] = Escaped;
    }

    StoreRelations[SI.getParent()][P].insert(V);
    if (Offset >= 0 && !Def[SI.getParent()][BaseP][Offset].empty()) {
      Def[SI.getParent()][BaseP][Offset].clear();
    }
    Def[SI.getParent()][BaseP][Offset].insert(GV);
  }

  void visitMemCpyOrMove(Instruction &I) {
    Value *P = I.getOperand(0);
    Value *V = I.getOperand(1);
    ConstantInt *CSI = dyn_cast<ConstantInt>(I.getOperand(2));
    handleMemcpyOrWriteAgg(&I, P, V, CSI);
  }

  void visitInvokeInst(InvokeInst &II) {
    Function *Callee = II.getCalledFunction();
    if (Callee && isRewriteableCangjieMallocFunc(Callee)) {
      GlobalVariable *Klass = getNewKlass(&II);
      if (Klass == nullptr) {
        EscapeBBInfo[II.getParent()][GCPtr::create(&II, *this, LI)] = Escaped;
        return;
      }
      bool HasRefs = false;
      uint32_t AllocaSize = 0;
      bool NonMoveArray = false;
      Type *AllocaType =
          getAllocaType(Klass, HasRefs, AllocaSize, NonMoveArray, &II);
      if (AllocaType != nullptr || NonMoveArray) {
        insertGCNew(&II);
      } else {
        EscapeBBInfo[II.getParent()][GCPtr::create(&II, *this, LI)] = Escaped;
      }
      return;
    }

    handleCallArgs(II);
  }

  void visitCallInst(CallInst &CI) {
    if (CI.getCalledFunction()) {
      if (handleCangjieSL(&CI)) {
        return;
      }
      Function *Callee = CI.getCalledFunction();
      if (isRewriteableCangjieMallocFunc(Callee)) {
        GlobalVariable *Klass = getNewKlass(&CI);
        if (Klass == nullptr) {
          EscapeBBInfo[CI.getParent()][GCPtr::create(&CI, *this, LI)] = Escaped;
          return;
        }
        bool HasRefs = false;
        uint32_t AllocaSize = 0;
        bool NonMoveArray = false;
        Type *AllocaType =
            getAllocaType(Klass, HasRefs, AllocaSize, NonMoveArray, &CI);
        if (AllocaType != nullptr || NonMoveArray) {
          insertGCNew(&CI);
        } else {
          EscapeBBInfo[CI.getParent()][GCPtr::create(&CI, *this, LI)] = Escaped;
        }
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
        EscapeBBInfo[I.getParent()][RetPtr] = Escaped;
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
