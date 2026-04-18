//===- ParamMergeFunctions.cpp - Merge functions via parameters -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass is a conservative local-only companion to mergefunc. It targets
// single-basic-block functions that have the same instruction structure but
// differ in a small number of pointer constants referencing globals. The pass
// creates one internal merged implementation with extra parameters for the
// differing globals and rewrites the original functions into thunks.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/ParamMergeFunctions.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <string>

#define DEBUG_TYPE "param-mergefunc"

using namespace llvm;

STATISTIC(NumParamMergedFunctions,
          "Number of functions rewritten into parameterized thunks");
STATISTIC(NumParamMergedBodies,
          "Number of internal merged functions created");

static cl::opt<unsigned>
    ParamMergeFunctionsMaxParams("param-mergefunc-max-params", cl::Hidden,
                                 cl::init(4),
                                 cl::desc("Maximum number of parameterized "
                                          "global constant operands"));

namespace {

struct DiffLoc {
  unsigned InstIndex;
  unsigned OpndIndex;
};

typedef SmallVector<DiffLoc, 4> DiffLocs;

static bool operator==(const DiffLoc &LHS, const DiffLoc &RHS) {
  return LHS.InstIndex == RHS.InstIndex && LHS.OpndIndex == RHS.OpndIndex;
}

static bool sameDiffLocs(const DiffLocs &LHS, const DiffLocs &RHS) {
  if (LHS.size() != RHS.size())
    return false;
  for (unsigned I = 0, E = LHS.size(); I != E; ++I)
    if (!(LHS[I] == RHS[I]))
      return false;
  return true;
}

static bool isEligibleFunction(Function *F) {
  if (F->isDeclaration())
    return false;
  if (!F->hasLocalLinkage())
    return false;
  if (F->hasFnAttribute(Attribute::NoMerge) ||
      F->hasFnAttribute(Attribute::AlwaysInline))
    return false;
  if (F->hasAvailableExternallyLinkage() || F->getFunctionType()->isVarArg())
    return false;
  if (F->hasAddressTaken())
    return false;
  if (F->getCallingConv() == CallingConv::SwiftTail)
    return false;
  if (!F->hasName() || F->size() != 1 || F->hasComdat())
    return false;

  for (BasicBlock &BB : *F) {
    for (Instruction &I : BB) {
      CallBase *CB = dyn_cast<CallBase>(&I);
      if (CB && CB->isMustTailCall())
        return false;
    }
  }

  return true;
}

static bool isCalleeOperand(const CallBase *CB, unsigned OpIdx) {
  return &CB->getCalledOperandUse() == &CB->getOperandUse(OpIdx);
}

static bool canParameterizeCallOperand(const CallBase *CB, unsigned OpIdx) {
  if (CB->isInlineAsm())
    return false;

  Function *Callee = CB->getCalledOperand()
                         ? dyn_cast_or_null<Function>(
                               CB->getCalledOperand()->stripPointerCasts())
                         : nullptr;
  if (Callee) {
    if (Callee->isIntrinsic())
      return false;
    StringRef Name = Callee->getName();
    if (Name.startswith("objc_msgSend$") || Name.startswith("__dtrace"))
      return false;
  }

  if (isCalleeOperand(CB, OpIdx)) {
    if (CB->getOperandBundle(LLVMContext::OB_ptrauth).hasValue())
      return false;
  } else if (CB->isOperandBundleOfType(LLVMContext::OB_clang_arc_attachedcall,
                                       OpIdx)) {
    return false;
  }

  return true;
}

static bool isEligibleInstructionForParameterization(const Instruction *I) {
  switch (I->getOpcode()) {
  case Instruction::Load:
  case Instruction::Store:
  case Instruction::Call:
  case Instruction::Invoke:
    return true;
  default:
    return false;
  }
}

static const GlobalValue *getParameterizableGlobalValue(const Instruction *I,
                                                        unsigned OpIdx) {
  if (OpIdx >= I->getNumOperands() ||
      !isEligibleInstructionForParameterization(I))
    return nullptr;

  const Constant *C = dyn_cast<Constant>(I->getOperand(OpIdx));
  if (!C || !C->getType()->isPointerTy())
    return nullptr;

  if (const CallBase *CB = dyn_cast<CallBase>(I))
    if (!canParameterizeCallOperand(CB, OpIdx))
      return nullptr;

  return dyn_cast_or_null<GlobalValue>(C->stripPointerCasts());
}

static bool haveCompatibleHeaders(Function &LHS, Function &RHS) {
  if (LHS.getFunctionType() != RHS.getFunctionType())
    return false;
  if (LHS.getCallingConv() != RHS.getCallingConv())
    return false;
  if (LHS.getAttributes() != RHS.getAttributes())
    return false;
  if (LHS.hasGC() != RHS.hasGC())
    return false;
  if (LHS.hasGC() && LHS.getGC() != RHS.getGC())
    return false;
  if (LHS.hasPersonalityFn() != RHS.hasPersonalityFn())
    return false;
  if (LHS.hasPersonalityFn() &&
      LHS.getPersonalityFn() != RHS.getPersonalityFn())
    return false;
  if (LHS.getSection() != RHS.getSection())
    return false;
  return true;
}

static bool areValuesEquivalent(
    const Value *LHS, const Value *RHS,
    const DenseMap<const Value *, const Value *> &ValueMap,
    const DenseMap<const BasicBlock *, const BasicBlock *> &BlockMap) {
  if (LHS == RHS)
    return true;

  if (const BasicBlock *BB = dyn_cast<BasicBlock>(LHS))
    return BlockMap.lookup(BB) == RHS;

  DenseMap<const Value *, const Value *>::const_iterator It =
      ValueMap.find(LHS);
  if (It != ValueMap.end())
    return It->second == RHS;

  return false;
}

static bool compareFunctions(Function &LHS, Function &RHS, DiffLocs &Diffs) {
  if (!haveCompatibleHeaders(LHS, RHS))
    return false;

  BasicBlock &LBB = LHS.front();
  BasicBlock &RBB = RHS.front();
  if (LBB.size() != RBB.size())
    return false;

  DenseMap<const Value *, const Value *> ValueMap;
  DenseMap<const BasicBlock *, const BasicBlock *> BlockMap;
  BlockMap[&LBB] = &RBB;

  Function::arg_iterator LAI = LHS.arg_begin(), RAI = RHS.arg_begin();
  for (; LAI != LHS.arg_end() && RAI != RHS.arg_end(); ++LAI, ++RAI)
    ValueMap[&*LAI] = &*RAI;
  if (LAI != LHS.arg_end() || RAI != RHS.arg_end())
    return false;

  BasicBlock::iterator LI = LBB.begin(), RI = RBB.begin();
  unsigned InstIndex = 0;
  for (; LI != LBB.end() && RI != RBB.end(); ++LI, ++RI, ++InstIndex) {
    if (!LI->isSameOperationAs(&*RI))
      return false;

    for (unsigned OpIdx = 0, E = LI->getNumOperands(); OpIdx != E; ++OpIdx) {
      Value *LOp = LI->getOperand(OpIdx);
      Value *ROp = RI->getOperand(OpIdx);
      if (areValuesEquivalent(LOp, ROp, ValueMap, BlockMap))
        continue;

      const GlobalValue *LGV = getParameterizableGlobalValue(&*LI, OpIdx);
      const GlobalValue *RGV = getParameterizableGlobalValue(&*RI, OpIdx);
      if (LGV && RGV && LOp->getType() == ROp->getType() && LGV != RGV) {
        DiffLoc Loc;
        Loc.InstIndex = InstIndex;
        Loc.OpndIndex = OpIdx;
        Diffs.push_back(Loc);
        continue;
      }

      return false;
    }

    ValueMap[&*LI] = &*RI;
  }

  return LI == LBB.end() && RI == RBB.end() && !Diffs.empty();
}

static SmallVector<Instruction *, 16> collectInstructions(Function &F) {
  SmallVector<Instruction *, 16> Insts;
  for (Instruction &I : F.front())
    Insts.push_back(&I);
  return Insts;
}

static DebugLoc getInsertionDebugLoc(Function &F) {
  if (!F.empty()) {
    for (Instruction &I : F.front()) {
      if (I.getDebugLoc())
        return I.getDebugLoc();
    }
  }

  if (DISubprogram *SP = F.getSubprogram())
    return DILocation::get(F.getContext(), SP->getLine(), 0, SP);
  return DebugLoc();
}

static Value *createCast(IRBuilder<> &Builder, Value *V, Type *DestTy) {
  Type *SrcTy = V->getType();
  if (SrcTy == DestTy)
    return V;
  if (SrcTy->isStructTy()) {
    assert(DestTy->isStructTy());
    assert(SrcTy->getStructNumElements() == DestTy->getStructNumElements());
    Value *Result = PoisonValue::get(DestTy);
    for (unsigned I = 0, E = SrcTy->getStructNumElements(); I != E; ++I) {
      Value *Element = createCast(
          Builder, Builder.CreateExtractValue(V, makeArrayRef(I)),
          DestTy->getStructElementType(I));
      Result = Builder.CreateInsertValue(Result, Element, makeArrayRef(I));
    }
    return Result;
  }

  assert(!DestTy->isStructTy());
  if (SrcTy->isIntegerTy() && DestTy->isPointerTy())
    return Builder.CreateIntToPtr(V, DestTy);
  if (SrcTy->isPointerTy() && DestTy->isIntegerTy())
    return Builder.CreatePtrToInt(V, DestTy);
  return Builder.CreateBitCast(V, DestTy);
}

static std::string getMergedFunctionName(Function &F) {
  Module *M = F.getParent();
  std::string BaseName = F.getName().str() + ".Tpm";
  std::string Name = BaseName;
  unsigned Index = 0;
  while (M->getFunction(Name))
    Name = BaseName + "." + utostr(++Index);
  return Name;
}

static SmallVector<Constant *, 4> collectParams(Function &F,
                                                const DiffLocs &Diffs) {
  SmallVector<Instruction *, 16> Insts = collectInstructions(F);
  SmallVector<Constant *, 4> Params;
  for (unsigned I = 0, E = Diffs.size(); I != E; ++I) {
    const DiffLoc &Loc = Diffs[I];
    Params.push_back(cast<Constant>(Insts[Loc.InstIndex]->getOperand(
        Loc.OpndIndex)));
  }
  return Params;
}

static Function *createMergedFunction(Function &Root, const DiffLocs &Diffs,
                                      ArrayRef<Type *> ParamTypes) {
  Module *M = Root.getParent();
  FunctionType *OrigTy = Root.getFunctionType();
  SmallVector<Type *, 8> AllParamTypes(OrigTy->param_begin(),
                                       OrigTy->param_end());
  AllParamTypes.append(ParamTypes.begin(), ParamTypes.end());
  FunctionType *MergedTy =
      FunctionType::get(OrigTy->getReturnType(), AllParamTypes, false);

  Function *Merged =
      Function::Create(MergedTy, Root.getLinkage(), getMergedFunctionName(Root));
  M->getFunctionList().insert(Root.getIterator(), Merged);
  Merged->copyAttributesFrom(&Root);
  if (Root.hasPersonalityFn())
    Merged->setPersonalityFn(Root.getPersonalityFn());
  if (Root.hasGC())
    Merged->setGC(Root.getGC());
  if (Optional<Function::ProfileCount> EC = Root.getEntryCount())
    Merged->setEntryCount(*EC);
  Merged->setDLLStorageClass(GlobalValue::DefaultStorageClass);
  Merged->setLinkage(GlobalValue::InternalLinkage);
  Merged->addFnAttr(Attribute::NoInline);

  Merged->getBasicBlockList().splice(Merged->begin(), Root.getBasicBlockList());

  Function::arg_iterator NewArgIt = Merged->arg_begin();
  for (Argument &OrigArg : Root.args()) {
    Argument &NewArg = *NewArgIt++;
    OrigArg.replaceAllUsesWith(&NewArg);
  }

  SmallVector<Instruction *, 16> Insts = collectInstructions(*Merged);
  unsigned NumOrigArgs = Root.arg_size();
  for (unsigned I = 0, E = Diffs.size(); I != E; ++I) {
    const DiffLoc &Loc = Diffs[I];
    Argument *NewArg = Merged->getArg(NumOrigArgs + I);
    Instruction *Inst = Insts[Loc.InstIndex];
    Value *OrigOpnd = Inst->getOperand(Loc.OpndIndex);
    IRBuilder<> Builder(Inst);
    Inst->setOperand(Loc.OpndIndex, createCast(Builder, NewArg,
                                               OrigOpnd->getType()));
  }

  return Merged;
}

static void clearTailCalls(Function &F) {
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (CallInst *CI = dyn_cast<CallInst>(&I))
        CI->setTailCallKind(CallInst::TCK_NoTail);
    }
  }
}

static bool needsConservativeMerging(Function &Merged, unsigned NumOrigArgs) {
  if (Merged.hasGC() || Merged.hasPersonalityFn())
    return true;

  for (unsigned I = NumOrigArgs, E = Merged.arg_size(); I != E; ++I) {
    Type *Ty = Merged.getArg(I)->getType();
    PointerType *PTy = dyn_cast<PointerType>(Ty);
    if (!PTy)
      continue;
    if (isa<FunctionType>(PTy->getPointerElementType()))
      return true;
  }

  for (BasicBlock &BB : Merged) {
    for (Instruction &I : BB) {
      CallBase *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      Value *Callee = CB->getCalledOperand()->stripPointerCasts();
      if (isa<Argument>(Callee))
        return true;
    }
  }

  return false;
}

static void createThunk(Function &Thunk, ArrayRef<Constant *> Params,
                        Function &Target) {
  DebugLoc DL = getInsertionDebugLoc(Thunk);
  Optional<Function::ProfileCount> EC = Thunk.getEntryCount();
  while (!Thunk.empty())
    Thunk.begin()->eraseFromParent();

  BasicBlock *BB = BasicBlock::Create(Thunk.getContext(), "", &Thunk);
  IRBuilder<> Builder(BB);
  Builder.SetCurrentDebugLocation(DL);
  SmallVector<Value *, 16> Args;
  FunctionType *TargetTy = Target.getFunctionType();
  unsigned ParamIdx = 0;

  for (Argument &Arg : Thunk.args()) {
    Args.push_back(createCast(Builder, &Arg,
                              TargetTy->getParamType(ParamIdx)));
    ++ParamIdx;
  }
  for (unsigned I = 0, E = Params.size(); I != E; ++I, ++ParamIdx) {
    Args.push_back(createCast(Builder, Params[I],
                              TargetTy->getParamType(ParamIdx)));
  }

  CallInst *CI = Builder.CreateCall(&Target, Args);
  bool NeedsConservativeThunk =
      Thunk.hasGC() || Target.hasGC() || Thunk.hasPersonalityFn() ||
      Target.hasPersonalityFn() ||
      Target.hasFnAttribute("disable-tail-calls");
  if (NeedsConservativeThunk) {
    Thunk.addFnAttr("disable-tail-calls", "true");
    CI->setTailCallKind(CallInst::TCK_NoTail);
  } else {
    CI->setTailCallKind(CallInst::TCK_Tail);
  }
  CI->setCallingConv(Target.getCallingConv());
  CI->setAttributes(Target.getAttributes());

  if (Thunk.getReturnType()->isVoidTy())
    Builder.CreateRetVoid();
  else
    Builder.CreateRet(createCast(Builder, CI, Thunk.getReturnType()));

  if (EC.hasValue())
    Thunk.setEntryCount(*EC);
}

static bool mergeGroup(SmallVectorImpl<Function *> &Group,
                       const DiffLocs &Diffs) {
  if (Group.size() < 2 || Diffs.empty() || Diffs.size() > ParamMergeFunctionsMaxParams)
    return false;

  SmallVector<SmallVector<Constant *, 4>, 4> ParamsPerFunction;
  SmallVector<Type *, 4> ParamTypes;

  for (unsigned I = 0, E = Group.size(); I != E; ++I)
    ParamsPerFunction.push_back(collectParams(*Group[I], Diffs));

  for (unsigned I = 0, E = ParamsPerFunction.front().size(); I != E; ++I)
    ParamTypes.push_back(ParamsPerFunction.front()[I]->getType());

  Function *Merged = createMergedFunction(*Group.front(), Diffs, ParamTypes);
  if (needsConservativeMerging(*Merged, Group.front()->arg_size())) {
    Merged->addFnAttr("disable-tail-calls", "true");
    clearTailCalls(*Merged);
  }
  ++NumParamMergedBodies;

  for (unsigned I = 0, E = Group.size(); I != E; ++I) {
    createThunk(*Group[I], ParamsPerFunction[I], *Merged);
    ++NumParamMergedFunctions;
  }

  LLVM_DEBUG({
    dbgs() << "[param-mergefunc] Created " << Merged->getName() << " for";
    for (unsigned I = 0, E = Group.size(); I != E; ++I)
      dbgs() << " " << Group[I]->getName();
    dbgs() << "\n";
  });

  return true;
}

class ParamMergeFunctions {
public:
  bool run(Module &M) {
    SmallVector<Function *, 32> EligibleFunctions;
    for (Function &F : M)
      if (isEligibleFunction(&F))
        EligibleFunctions.push_back(&F);

    bool Changed = false;
    SmallPtrSet<Function *, 16> MergedFunctions;

    for (unsigned I = 0, E = EligibleFunctions.size(); I != E; ++I) {
      Function *Root = EligibleFunctions[I];
      if (MergedFunctions.count(Root))
        continue;

      SmallVector<Function *, 4> Group;
      Group.push_back(Root);
      DiffLocs GroupDiffs;

      for (unsigned J = I + 1; J != E; ++J) {
        Function *Other = EligibleFunctions[J];
        if (MergedFunctions.count(Other))
          continue;

        DiffLocs Diffs;
        if (!compareFunctions(*Root, *Other, Diffs))
          continue;

        if (Group.size() == 1)
          GroupDiffs = Diffs;
        else if (!sameDiffLocs(GroupDiffs, Diffs))
          continue;

        Group.push_back(Other);
      }

      if (!mergeGroup(Group, GroupDiffs))
        continue;

      for (unsigned J = 0, JE = Group.size(); J != JE; ++J)
        MergedFunctions.insert(Group[J]);
      Changed = true;
    }

    return Changed;
  }
};

} // namespace

PreservedAnalyses ParamMergeFunctionsPass::run(Module &M,
                                               ModuleAnalysisManager &AM) {
  (void)AM;
  if (ParamMergeFunctions().run(M))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}
