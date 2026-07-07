//===- CJGCInstrReplace.cpp - -----------------------------------------*- C++
//-*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file provides interface to "Cangjie GC Instruction Replace" pass.
//
// This pass will replace gcread/gcwrite with load/store in pass pipeline
// beginning.
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJGCInstrReplace.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Scalar.h"

using namespace llvm;

#define DEBUG_TYPE "CJGCInstrReplace"

SmallDenseMap<Intrinsic::ID, StringRef> CJGCInstrReplace::GCIntrinsicIDToLabel =
    {{Intrinsic::cj_gcread_ref, CJGCInstrReplace::GC_READ_REF_IID},
     {Intrinsic::cj_gcwrite_ref, CJGCInstrReplace::GC_WRITE_REF_IID}};

PreservedAnalyses CJGCInstrReplace::run(Function &F,
                                        FunctionAnalysisManager &FAM) {
  if (runImpl(F)) {
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

bool CJGCInstrReplace::runImpl(Function &F) {
  LLVM_DEBUG(dbgs() << "CJGCInstrReplace: " << F.getName().str() << "start."
                    << "\n");
  initContainer();
  collectGCInstr(F);
  bool changed = changeGCInstrToLSInstr();

  LLVM_DEBUG({
    if (changed) {
      dbgs() << "CJGCInstrReplace: " << F.getName().str() << " changed."
             << "\n";
    }
  });

  LLVM_DEBUG(dbgs() << "CJGCInstrReplace: " << F.getName().str() << "end."
                    << "\n");

  return changed;
}

void CJGCInstrReplace::initContainer() {
  GCInstrDispatchMap.clear();
  GCReadRefs.clear();
  GCWriteRefs.clear();
  GCInstrDispatchMap[Intrinsic::cj_gcread_ref] = &GCReadRefs;
  GCInstrDispatchMap[Intrinsic::cj_gcwrite_ref] = &GCWriteRefs;
}

inline void CJGCInstrReplace::dispatchGCInstr(CallBase *CB) {
  auto IID = CB->getIntrinsicID();
  if (GCInstrDispatchMap.count(IID)) {
    GCInstrDispatchMap[IID]->push_back(CB);
    LLVM_DEBUG(dbgs() << "dispatchGCInstr: " << *CB << "\n");
  }
}

void CJGCInstrReplace::collectGCInstr(Function &F) {
  for (auto &I : instructions(F)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      dispatchGCInstr(CB);
    }
  }
}

namespace {

void addGCInstrMetadata(Instruction *Inst, CallBase *SourceGCInstr) {
  LLVMContext &Ctx = Inst->getContext();
  auto IID = SourceGCInstr->getIntrinsicID();
  Constant *ConstVal = ConstantInt::get(Type::getInt64Ty(Ctx), uint64_t(IID));
  ConstantAsMetadata *CAM = ConstantAsMetadata::get(ConstVal);
  MDNode *Node = MDNode::get(Ctx, CAM);
  Inst->setMetadata(CJGCInstrReplace::GCIntrinsicIDToLabel[IID], Node);
  SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
  SourceGCInstr->getAllMetadata(MDs);
  for (const auto &MD : MDs) {
    Inst->setMetadata(MD.first, MD.second);
  }
}

/// Return true if \p V (the result of a cj_gcread_ref) is used, directly or
/// through pointer-transparent instructions (GEP / bitcast / addrspacecast /
/// PHI / select / ConstantExpr), as the *pointer operand* of a LoadInst or
/// StoreInst.
///
/// When this is the case, rewriting the gcread into a plain load would let
/// DSE/GVN treat the loaded pointer as PointerUnknown and mis-reason about
/// stores that are only reachable through that pointer (the MM0002
/// mis-optimization: the GC write barrier on the referenced object's field
/// gets deleted as a "dead store"). Keeping such gcreads as the opaque
/// intrinsic preserves the conservative MemoryDef that DSE cannot see through.
///
/// Uses that consume the gcread result as a plain value (storing it into a
/// field, passing it to a call, comparing it, ...) do not hit this rule and
/// are still rewritten to a plain load so that CSE/GVN can fold
/// `gcwrite a to b; c = gcread from b` into `c = a`.
static bool isGcreadResultUsedAsAddrBase(Value *V,
                                         SmallPtrSetImpl<Value *> &Visited) {
  if (!Visited.insert(V).second)
    return false; // already on the stack, avoid infinite recursion

  for (User *U : V->users()) {
    // Pointer-transparent producers: keep walking their uses.
    if (isa<GetElementPtrInst>(U) || isa<BitCastInst>(U) ||
        isa<AddrSpaceCastInst>(U) || isa<PHINode>(U) || isa<SelectInst>(U)) {
      if (isGcreadResultUsedAsAddrBase(U, Visited))
        return true;
      continue;
    }
    if (auto *LI = dyn_cast<LoadInst>(U)) {
      if (LI->getPointerOperand() == V)
        return true;
      continue;
    }
    if (auto *SI = dyn_cast<StoreInst>(U)) {
      // Pointer operand (operand 1) -> address-base use: hit.
      // Value operand (operand 0) -> the gcread result is stored as a value,
      // which is safe and is exactly the CSE-foldable site we want to keep.
      if (SI->getPointerOperand() == V)
        return true;
      continue;
    }
    if (auto *CE = dyn_cast<ConstantExpr>(U)) {
      // Only pointer-typed ConstantExprs can propagate the address.
      if (CE->getType()->isPointerTy() &&
          isGcreadResultUsedAsAddrBase(CE, Visited))
        return true;
      continue;
    }
    // Any other use (call/invoke argument, arithmetic, comparison, ...)
    // consumes the value rather than using it as an address base, so it is
    // safe to rewrite.
  }
  return false;
}

bool changeGCReadRef(SmallVector<CallBase *, 4> *GCInstrs) {
  bool Changed = false;
  SmallVector<CallBase *, 4> ToRewrite;
  for (auto *GCReadRef : *GCInstrs) {
    // Selective rewrite (Path A): only lower a gcread to a plain load when
    // its result is NOT used as a load/store pointer base. When the result
    // flows into an address, keep the intrinsic so DSE/GVN cannot see through
    // the loaded pointer and delete live barriers (MM0002).
    SmallPtrSet<Value *, 16> Visited;
    if (isGcreadResultUsedAsAddrBase(GCReadRef, Visited))
      continue;
    ToRewrite.push_back(GCReadRef);
  }
  for (auto *GCReadRef : ToRewrite) {
    Changed = true;
    IRBuilder<> IRB(GCReadRef);
    auto *AddedLoadInstr = IRB.CreateLoad(
        GCReadRef->getArgOperand(0)->getType(), GCReadRef->getArgOperand(1));
    addGCInstrMetadata(AddedLoadInstr, GCReadRef);
    GCReadRef->replaceAllUsesWith(AddedLoadInstr);
  }
  for (auto *GCReadRef : ToRewrite) {
    GCReadRef->eraseFromParent();
  }
  return Changed;
}

bool changeGCWriteRef(SmallVector<CallBase *, 4> *GCInstrs) {
  bool Changed = false;
  for (auto *GCWriteRef : *GCInstrs) {
    Changed = true;
    IRBuilder<> IRB(GCWriteRef);
    auto *AddedStoreInstr = IRB.CreateStore(GCWriteRef->getArgOperand(0),
                                            GCWriteRef->getArgOperand(2));
    addGCInstrMetadata(AddedStoreInstr, GCWriteRef);
  }
  for (auto *GCWriteRef : *GCInstrs) {
    GCWriteRef->eraseFromParent();
  }
  return Changed;
}

} // namespace

bool CJGCInstrReplace::changeGCInstrToLSInstr() {
  bool Changed = false;
  for (auto [IID, GCInstrs] : GCInstrDispatchMap) {
    switch (IID) {
    case Intrinsic::cj_gcread_ref:
      Changed |= changeGCReadRef(GCInstrs);
      break;
    case Intrinsic::cj_gcwrite_ref:
      Changed |= changeGCWriteRef(GCInstrs);
      break;
    default:
      break;
    }
  }
  return Changed;
}
