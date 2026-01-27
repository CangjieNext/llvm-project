//===- CJGCInstrRestore.cpp - -----------------------------------------*- C++
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
// This file provides interface to "Cangjie GC Instruction Restore" pass.
//
// This pass will restore load/store to gcread/gcwrite in pass pipeline ending.
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJGCInstrRestore.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Scalar.h"

using namespace llvm;

#define DEBUG_TYPE "CJGCInstrRestore"

PreservedAnalyses CJGCInstrRestore::run(Function &F,
                                        FunctionAnalysisManager &FAM) {
  if (runImpl(F)) {
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

bool CJGCInstrRestore::runImpl(Function &F) {

  LLVM_DEBUG(dbgs() << "CJGCInstrRestore: " << F.getName().str() << " start."
                    << "\n");

  initContainer();
  collectLSInstr(F);
  bool changed = restoreLSInstrToGCInstr();

  LLVM_DEBUG({
    if (changed) {
      dbgs() << "CJGCInstrRestore: " << F.getName().str() << " changed."
             << "\n";
    }
  });

  LLVM_DEBUG(dbgs() << "CJGCInstrRestore: " << F.getName().str() << " end."
                    << "\n");

  return changed;
}

void CJGCInstrRestore::initContainer() {
  LSInstrDispatchMap.clear();
  LoadForGCReadRefs.clear();
  StoreForGCWriteRefs.clear();
  LSInstrDispatchMap[Intrinsic::cj_gcread_ref] = &LoadForGCReadRefs;
  LSInstrDispatchMap[Intrinsic::cj_gcwrite_ref] = &StoreForGCWriteRefs;
}

// should use metadata to restore LSInstr more precisely
inline void CJGCInstrRestore::dispatchLSInstr(Instruction *Instr) { return; }

void CJGCInstrRestore::collectLSInstr(Function &F) {
  for (auto &I : instructions(F)) {
    // shoud use dispatchLSInstr later
    if (isa<StoreInst>(I)) {
      LSInstrDispatchMap[Intrinsic::cj_gcwrite_ref]->push_back(&I);
    } else if (isa<LoadInst>(I)) {
      LSInstrDispatchMap[Intrinsic::cj_gcread_ref]->push_back(&I);
    }
  }
}

namespace {

bool operandTypeCheck(Type *Ty) {
  if (!Ty->isPointerTy()) {
    return false;
  }
  auto *PtrTy = dyn_cast<PointerType>(Ty);
  if (PtrTy->getAddressSpace() != 1) {
    return false;
  }
  return true;
}

bool shouldRestore(Instruction *Instr) {
  if (isa<StoreInst>(Instr)) {
    return operandTypeCheck(Instr->getOperand(0)->getType()) &&
           operandTypeCheck(Instr->getOperand(1)->getType()) &&
           !isa<AllocaInst>(getUnderlyingObject(Instr->getOperand(1)));
  }
  if (isa<LoadInst>(Instr)) {
    return operandTypeCheck(Instr->getOperand(0)->getType()) &&
           !isa<AllocaInst>(getUnderlyingObject(Instr->getOperand(0)));
  }
  return false;
}

Value *castToTargetType(Value *V, IRBuilder<> &IRB, Type *TargetType) {
  if (V->getType() == TargetType) {
    return V;
  }
  auto *VTy = V->getType();
  assert(VTy->isPointerTy() &&
         "invalid type in CJGCInstrRestore pass castToValueOperandType.");
  auto *PtrTy = dyn_cast<PointerType>(VTy);
  auto *TargetPtrTy = dyn_cast<PointerType>(TargetType);
  bool AddressSpaceEqual =
      PtrTy->getAddressSpace() == TargetPtrTy->getAddressSpace();
  Value *Res = nullptr;
  if (!AddressSpaceEqual) {
    Type *TmpType = PointerType::get(TargetPtrTy->getElementType(),
                                     PtrTy->getAddressSpace());
    Res = IRB.CreateBitCast(V, TmpType);
    Res = IRB.CreateAddrSpaceCast(Res, TargetType);
  } else {
    Res = IRB.CreateBitCast(V, TargetType);
  }
  return Res;
}

// i8 addrspace(1)*
Value *castToI8AddrSpace1PtrType(Value *V, IRBuilder<> &IRB) {
  LLVMContext &Ctx = IRB.getContext();
  return castToTargetType(V, IRB, PointerType::get(Type::getInt8Ty(Ctx), 1));
}

// i8 addrspace(1)* addrspace(1)*
Value *castToI8AddrSpace1PtrTypeAddrSpace1PtrType(Value *V, IRBuilder<> &IRB) {
  LLVMContext &Ctx = IRB.getContext();
  return castToTargetType(
      V, IRB, PointerType::get(PointerType::get(Type::getInt8Ty(Ctx), 1), 1));
}

bool restoreGCReadRef(SmallVector<Instruction *, 4> *Instrs) {
  bool Changed = false;
  SmallVector<Instruction *, 4> ToBeErased;
  for (auto *LI : *Instrs) {
    if (shouldRestore(LI)) {
      Changed = true;
      IRBuilder<> IRB(LI);
      Module *M = IRB.GetInsertBlock()->getModule();
      Function *IntrinsicFunc =
          Intrinsic::getDeclaration(M, Intrinsic::cj_gcread_ref);
      auto *BasePtr = castToI8AddrSpace1PtrType(
          getUnderlyingObject(LI->getOperand(0)), IRB);
      auto *DerivedPtr =
          castToI8AddrSpace1PtrTypeAddrSpace1PtrType(LI->getOperand(0), IRB);
      auto *CI = IRB.CreateCall(IntrinsicFunc, {BasePtr, DerivedPtr});
      LI->replaceAllUsesWith(CI);
      ToBeErased.push_back(LI);
    }
  }
  for (auto *I : ToBeErased) {
    I->eraseFromParent();
  }
  return Changed;
}

bool restoreGCWriteRef(SmallVector<Instruction *, 4> *Instrs) {
  bool Changed = false;
  SmallVector<Instruction *, 4> ToBeErased;
  for (auto *SI : *Instrs) {
    if (shouldRestore(SI)) {
      Changed = true;
      IRBuilder<> IRB(SI);
      Module *M = IRB.GetInsertBlock()->getModule();
      Function *IntrinsicFunc =
          Intrinsic::getDeclaration(M, Intrinsic::cj_gcwrite_ref);
      auto *BasePtr = castToI8AddrSpace1PtrType(
          getUnderlyingObject(SI->getOperand(1)), IRB);
      auto *ValuePtr = castToI8AddrSpace1PtrType(SI->getOperand(0), IRB);
      auto *DerivedPtr =
          castToI8AddrSpace1PtrTypeAddrSpace1PtrType(SI->getOperand(1), IRB);
      IRB.CreateCall(IntrinsicFunc, {ValuePtr, BasePtr, DerivedPtr});
      ToBeErased.push_back(SI);
    }
  }
  for (auto *I : ToBeErased) {
    I->eraseFromParent();
  }
  return Changed;
}

} // namespace

bool CJGCInstrRestore::restoreLSInstrToGCInstr() {
  bool Changed = false;
  for (auto [IID, Instrs] : LSInstrDispatchMap) {
    switch (IID) {
    case Intrinsic::cj_gcread_ref:
      Changed |= restoreGCReadRef(Instrs);
      break;
    case Intrinsic::cj_gcwrite_ref:
      Changed |= restoreGCWriteRef(Instrs);
      break;
    default:
      break;
    }
  }
  return Changed;
}