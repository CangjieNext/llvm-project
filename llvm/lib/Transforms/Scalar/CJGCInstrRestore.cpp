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
#include "llvm/Support/Casting.h"
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
#ifndef NDEBUG
  LLVM_DEBUG(dbgs() << "CJGCInstrRestore: " << F.getName().str() << " start."
                    << "\n");
#endif
  initContainer();
  collectLSInstr(F);
  bool changed = restoreLSInstrToGCInstr();
#ifndef NDEBUG
  if (changed) {
    LLVM_DEBUG(dbgs() << "CJGCInstrRestore: " << F.getName().str()
                      << " changed." << "\n");
  }
  LLVM_DEBUG(dbgs() << "CJGCInstrRestore: " << F.getName().str() << " end."
                    << "\n");
#endif
  return changed;
}

void CJGCInstrRestore::initContainer() {
  LSInstrDispatchMap.clear();
  LoadForGCReadRefs.clear();
  StoreForGCWriteRefs.clear();
  LSInstrDispatchMap[Intrinsic::cj_gcread_ref] = &LoadForGCReadRefs;
  LSInstrDispatchMap[Intrinsic::cj_gcwrite_ref] = &StoreForGCWriteRefs;
}

inline void CJGCInstrRestore::dispatchLSInstr(Instruction *Instr) {
  LSInstrDispatchMap[Intrinsic::cj_gcwrite_ref]->push_back(Instr);
}

void CJGCInstrRestore::collectLSInstr(Function &F) {
  for (auto &I : instructions(F)) {
    if (isa<StoreInst>(I)) {
      dispatchLSInstr(&I);
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

Value * getBasePointer(Value *V) {
  assert(V->getType()->isPointerTy() && "Unexpected operand type in getBasePointer!");
  auto *Res = getUnderlyingObject(V);
  if (!isa<PHINode>(Res)) {
    return Res;
  }
  PHINode *PhiInstr = const_cast<PHINode*>(dyn_cast<PHINode>(Res));
  unsigned IncomingNum = PhiInstr->getNumIncomingValues();
  SmallVector<Value *, 4> IncomingValues;
  for (Value *IncValue : PhiInstr->incoming_values()) {
    IncomingValues.push_back(getBasePointer(IncValue));
  }
  IRBuilder<> Builder(PhiInstr->getContext());
  BasicBlock *BB = PhiInstr->getParent();
  BasicBlock::iterator IP = ++(PhiInstr->getIterator());
  Builder.SetInsertPoint(BB, IP);
  auto *AddPhiNode = Builder.CreatePHI(V->getType(), IncomingNum);
  unsigned Index = 0;
  for (auto *IncValue : IncomingValues) {
    AddPhiNode->addIncoming(IncValue, PhiInstr->getIncomingBlock(Index++));
  }
  return AddPhiNode;
}

bool isFromAlloc(Value *V) {
  auto *SourceInstr = getUnderlyingObject(V);
  if (!isa<PHINode>(SourceInstr)) {
    return isa<AllocaInst>(SourceInstr);
  }
  PHINode *PhiInstr = const_cast<PHINode*>(dyn_cast<PHINode>(SourceInstr));
  for (Value *IncValue : PhiInstr->incoming_values()) {
    if (isFromAlloc(IncValue)) {
      return true;
    }
  }
  return false;
}

bool shouldRestore(Instruction *Instr) {
  if (isa<StoreInst>(Instr)) {
    return operandTypeCheck(Instr->getOperand(0)->getType()) &&
           operandTypeCheck(Instr->getOperand(1)->getType()) &&
           !isFromAlloc(Instr->getOperand(1));
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
Value *castToI8AddrNum1PtrType(Value *V, IRBuilder<> &IRB) {
  LLVMContext &Ctx = IRB.getContext();
  return castToTargetType(V, IRB, PointerType::get(Type::getInt8Ty(Ctx), 1));
}

// i8 addrspace(1)* addrspace(1)*
Value *castToI8AddrNum1PtrTypeAddrNum1PtrType(Value *V, IRBuilder<> &IRB) {
  LLVMContext &Ctx = IRB.getContext();
  return castToTargetType(
      V, IRB, PointerType::get(PointerType::get(Type::getInt8Ty(Ctx), 1), 1));
}

bool restoreGCReadRef(SmallVector<Instruction *, 4> *Instrs) { return false; }

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
      auto *BasePtr =
          castToI8AddrNum1PtrType(getBasePointer(SI->getOperand(1)), IRB);
      auto *ValuePtr = castToI8AddrNum1PtrType(SI->getOperand(0), IRB);
      auto *DerivedPtr =
          castToI8AddrNum1PtrTypeAddrNum1PtrType(SI->getOperand(1), IRB);
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