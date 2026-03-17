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
#include "llvm/IR/SafepointIRVerifier.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/Scalarizer.h"

using namespace llvm;

#define DEBUG_TYPE "CJGCInstrRestore"

namespace {

enum class VectorGCMemoryAccessKind {
  None,
  HeapBarrier,
  StaticBarrier,
};

Type *getVectorGCValueType(Instruction *Instr) {
  if (auto *LI = dyn_cast<LoadInst>(Instr))
    return LI->getType();
  if (auto *SI = dyn_cast<StoreInst>(Instr))
    return SI->getValueOperand()->getType();
  return nullptr;
}

VectorType *getVectorGCPtrType(Type *Ty) {
  auto *VT = dyn_cast_or_null<VectorType>(Ty);
  if (!VT || !isGCPointerType(VT->getElementType()))
    return nullptr;
  return VT;
}

VectorGCMemoryAccessKind classifyVectorGCMemoryAccess(Instruction *Instr) {
  auto *VT = getVectorGCPtrType(getVectorGCValueType(Instr));
  if (!VT)
    return VectorGCMemoryAccessKind::None;

  Value *PtrOperand = nullptr;
  if (auto *LI = dyn_cast<LoadInst>(Instr)) {
    PtrOperand = LI->getPointerOperand();
  } else if (auto *SI = dyn_cast<StoreInst>(Instr)) {
    PtrOperand = SI->getPointerOperand();
  } else {
    return VectorGCMemoryAccessKind::None;
  }

  Value *PtrBase = findMemoryBasePointer(PtrOperand);
  if (isGCPointerType(PtrBase->getType()))
    return VectorGCMemoryAccessKind::HeapBarrier;
  if (isa<Constant>(PtrBase))
    return VectorGCMemoryAccessKind::StaticBarrier;
  return VectorGCMemoryAccessKind::None;
}

void reportUnsupportedVectorGCMemoryAccess(Instruction &Instr,
                                           StringRef Reason) {
  std::string Msg;
  raw_string_ostream OS(Msg);
  OS << "CJGCInstrRestore: " << Reason
     << " vector GC memory access is unsupported: " << Instr;
  report_fatal_error(Twine(OS.str()), false);
}

bool scalarizeBarrierSensitiveVectorGCMemoryOps(Function &F,
                                                FunctionAnalysisManager &FAM) {
  bool NeedsScalarization = false;

  for (Instruction &I : instructions(F)) {
    auto Kind = classifyVectorGCMemoryAccess(&I);
    if (Kind == VectorGCMemoryAccessKind::None)
      continue;

    if (Kind == VectorGCMemoryAccessKind::StaticBarrier)
      reportUnsupportedVectorGCMemoryAccess(
          I, "static-barrier-sensitive");

    auto *VT = dyn_cast<FixedVectorType>(getVectorGCValueType(&I));
    if (!VT)
      reportUnsupportedVectorGCMemoryAccess(I, "non-fixed");

    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      if (!LI->isSimple())
        reportUnsupportedVectorGCMemoryAccess(I, "non-simple");
    } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
      if (!SI->isSimple())
        reportUnsupportedVectorGCMemoryAccess(I, "non-simple");
    } else {
      llvm_unreachable("unexpected instruction kind");
    }

    NeedsScalarization = true;
  }

  if (!NeedsScalarization)
    return false;

  // CJGCInstrRestore only knows how to rebuild scalar GC barriers. Scalarize
  // simple fixed-width vector GC memory operations before the regular restore
  // path, and reject anything that still escapes this narrowing.
  ScalarizerPass SP;
  SP.setScalarizeLoadStore(true);
  SP.setScalarizeVariableInsertExtract(true);
  (void)SP.run(F, FAM);

  for (Instruction &I : instructions(F)) {
    if (classifyVectorGCMemoryAccess(&I) != VectorGCMemoryAccessKind::None)
      reportUnsupportedVectorGCMemoryAccess(
          I, "post-scalarization barrier-sensitive");
  }

  return true;
}

} // namespace

PreservedAnalyses CJGCInstrRestore::run(Function &F,
                                        FunctionAnalysisManager &FAM) {
  bool Changed = false;
  Changed |= scalarizeBarrierSensitiveVectorGCMemoryOps(F, FAM);
  Changed |= runImpl(F);
  if (Changed) {
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
  ValueToBasePointer.clear();
  LSInstrDispatchMap[Intrinsic::cj_gcread_ref] = &LoadForGCReadRefs;
  LSInstrDispatchMap[Intrinsic::cj_gcwrite_ref] = &StoreForGCWriteRefs;
}

// should use metadata to restore LSInstr more precisely
inline void CJGCInstrRestore::dispatchLSInstr(Instruction *Instr) {
  if (isa<StoreInst>(Instr)) {
    LSInstrDispatchMap[Intrinsic::cj_gcwrite_ref]->push_back(Instr);
  } else if (isa<LoadInst>(Instr)) {
    LSInstrDispatchMap[Intrinsic::cj_gcread_ref]->push_back(Instr);
  }
}

void CJGCInstrRestore::collectLSInstr(Function &F) {
  for (auto &I : instructions(F)) {
    dispatchLSInstr(&I);
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

bool isFromAlloc(Value *V, std::set<Value *> &Visited) {
  // indicate the cyclic tracking chain gains nothing info,
  // return true to continue other incoming values anlysis.
  if (Visited.count(V)) {
    return true;
  }
  Visited.insert(V);
  auto *SourceInstr = getUnderlyingObject(V);
  if (!isa<PHINode>(SourceInstr)) {
    return isa<AllocaInst>(SourceInstr);
  }
  PHINode *PhiInstr = const_cast<PHINode *>(dyn_cast<PHINode>(SourceInstr));
  for (Value *IncValue : PhiInstr->incoming_values()) {
    if (!isFromAlloc(IncValue, Visited)) {
      return false;
    }
  }
  return true;
}

bool shouldRestore(Instruction *Instr) {
  if (isa<StoreInst>(Instr)) {
    std::set<Value *> Visited;
    return operandTypeCheck(Instr->getOperand(0)->getType()) &&
           operandTypeCheck(Instr->getOperand(1)->getType()) &&
           !isFromAlloc(Instr->getOperand(1), Visited);
  }
  if (isa<LoadInst>(Instr)) {
    std::set<Value *> Visited;
    return operandTypeCheck(Instr->getOperand(0)->getType()) &&
           operandTypeCheck(Instr->getType()) &&
           !isFromAlloc(Instr->getOperand(0), Visited);
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
inline Value *castToI8AddrSpace1PtrType(Value *V, IRBuilder<> &IRB) {
  LLVMContext &Ctx = IRB.getContext();
  return castToTargetType(V, IRB, PointerType::get(Type::getInt8Ty(Ctx), 1));
}

// i8 addrspace(1)* addrspace(1)*
inline Value *castToI8AddrSpace1PtrTypeAddrSpace1PtrType(Value *V, IRBuilder<> &IRB) {
  LLVMContext &Ctx = IRB.getContext();
  return castToTargetType(
      V, IRB, PointerType::get(PointerType::get(Type::getInt8Ty(Ctx), 1), 1));
}

Value *getBasePointer(Value *V, Function *F,
                      SmallDenseMap<Value *, Value *> &ValueToBasePointer) {
  assert(V->getType()->isPointerTy() &&
         "Unexpected operand type in getBasePointer!");
  if (ValueToBasePointer.count(V)) {
    return ValueToBasePointer[V];
  }

  auto *Res = getUnderlyingObject(V);
  if (!isa<PHINode>(Res)) {
    ValueToBasePointer[V] = Res;
    return Res;
  }

  PHINode *PhiInstr = const_cast<PHINode *>(dyn_cast<PHINode>(Res));
  LLVMContext &Ctx = PhiInstr->getContext();
  auto *TargetType = PointerType::get(Type::getInt8Ty(Ctx), 1);
  unsigned IncomingNum = PhiInstr->getNumIncomingValues();
  BasicBlock *BB = PhiInstr->getParent();
  BasicBlock::iterator It = ++(PhiInstr->getIterator());
  IRBuilder<> Builder(Ctx);
  Builder.SetInsertPoint(BB, It);
  auto *AddPhiNode = Builder.CreatePHI(TargetType, IncomingNum);
  ValueToBasePointer[V] = AddPhiNode;
  SmallVector<Value *, 4> IncomingValues;

  for (Value *IncValue : PhiInstr->incoming_values()) {
    auto *BasePoint = getBasePointer(IncValue, F, ValueToBasePointer);
    IRBuilder<> IncValueCastBuilder(Ctx);
    if (auto *DefInstr = dyn_cast<Instruction>(BasePoint)) {
      BasicBlock *BB = DefInstr->getParent();
      BasicBlock::iterator It = ++(DefInstr->getIterator());
      while (It != BB->end() && isa<PHINode>(It))
        ++It;
      IncValueCastBuilder.SetInsertPoint(BB, It);
    } else {
      BasicBlock *BB = &(F->getEntryBlock());
      IncValueCastBuilder.SetInsertPoint(BB,
                                         BB->getFirstNonPHI()->getIterator());
    }
    IncomingValues.push_back(
        castToTargetType(BasePoint, IncValueCastBuilder, TargetType));
  }

  unsigned Index = 0;
  for (auto *IncValue : IncomingValues) {
    AddPhiNode->addIncoming(IncValue, PhiInstr->getIncomingBlock(Index++));
  }
  return AddPhiNode;
}

void addMetadataToGCInstr(CallBase *RestoreGCInstr, Instruction *Inst) {
  SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
  Inst->getAllMetadata(MDs);
  for (const auto &MD : MDs) {
    RestoreGCInstr->setMetadata(MD.first, MD.second);
  }
}

bool restoreGCReadRef(SmallVector<Instruction *, 4> *Instrs, SmallDenseMap<Value *, Value *> &ValueToBasePointer) {
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
          getBasePointer(LI->getOperand(0), LI->getFunction(), ValueToBasePointer), IRB);
      auto *DerivedPtr =
          castToI8AddrSpace1PtrTypeAddrSpace1PtrType(LI->getOperand(0), IRB);
      auto *GCInstr = IRB.CreateCall(IntrinsicFunc, {BasePtr, DerivedPtr});
      addMetadataToGCInstr(GCInstr, LI);
      auto * ReplacerValue = castToTargetType(GCInstr, IRB, LI->getType());
      for (auto &[V, BasePointer] : ValueToBasePointer) {
        if (BasePointer == LI) {
          ValueToBasePointer[V] = ReplacerValue;
        }
      }
      LI->replaceAllUsesWith(ReplacerValue);
      LI->eraseFromParent();
    }
  }
  return Changed;
}

bool restoreGCWriteRef(SmallVector<Instruction *, 4> *Instrs, SmallDenseMap<Value *, Value *> &ValueToBasePointer) {
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
          getBasePointer(SI->getOperand(1), SI->getFunction(), ValueToBasePointer), IRB);
      auto *ValuePtr = castToI8AddrSpace1PtrType(SI->getOperand(0), IRB);
      auto *DerivedPtr =
          castToI8AddrSpace1PtrTypeAddrSpace1PtrType(SI->getOperand(1), IRB);
      auto *GCInstr = IRB.CreateCall(IntrinsicFunc, {ValuePtr, BasePtr, DerivedPtr});
      addMetadataToGCInstr(GCInstr, SI);
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
      Changed |= restoreGCReadRef(Instrs, ValueToBasePointer);
      break;
    case Intrinsic::cj_gcwrite_ref:
      Changed |= restoreGCWriteRef(Instrs, ValueToBasePointer);
      break;
    default:
      break;
    }
  }
  return Changed;
}
