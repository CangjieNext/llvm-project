//===- CJAnnotationInitDedup.cpp -----------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This pass deduplicates profitable Cangjie annotation init and annotation
// factory functions with identical bodies. The varying static slots are moved
// into helper parameters so each original wrapper can stay as a tiny thunk.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJAnnotationInitDedup.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include <string>

using namespace llvm;

namespace {

constexpr StringRef AnnotationInitPrefix = "_CAO";
constexpr StringRef AnnotationFactoryPrefix = "_CAF";
constexpr StringRef AnnotationInitSuffix = "iiHv";
constexpr StringRef AnnotationInitHelperSuffix = "_imp";

struct AnnotationInitCandidate {
  Function *Fn = nullptr;
  BasicBlock *BB = nullptr;
  CallInst *FinalStaticWrite = nullptr;
  unsigned BodyInstCount = 0;
};

struct AnnotationFactoryCandidate {
  Function *Fn = nullptr;
  BasicBlock *BB = nullptr;
  SmallVector<CallInst *, 4> StaticReads;
  unsigned BodyInstCount = 0;
};

static bool isAnnotationInitName(StringRef Name) {
  return Name.startswith(AnnotationInitPrefix) &&
         Name.endswith(AnnotationInitSuffix);
}

static bool isAnnotationFactoryName(StringRef Name) {
  return Name.startswith(AnnotationFactoryPrefix);
}

static bool isBodyInstruction(const Instruction &I,
                              const CallInst *FinalStaticWrite) {
  return !I.isDebugOrPseudoInst() && !isa<ReturnInst>(&I) &&
         &I != FinalStaticWrite;
}

static bool isHelperInstruction(const Instruction &I) {
  return !I.isDebugOrPseudoInst() && !isa<ReturnInst>(&I);
}

static bool isStaticRefWrite(const Instruction &I) {
  const auto *II = dyn_cast<IntrinsicInst>(&I);
  return II != nullptr &&
         II->getIntrinsicID() == Intrinsic::cj_gcwrite_static_ref;
}

static bool isStaticRefRead(const Instruction &I) {
  const auto *II = dyn_cast<IntrinsicInst>(&I);
  return II != nullptr &&
         II->getIntrinsicID() == Intrinsic::cj_gcread_static_ref;
}

static bool matchAnnotationInit(Function &F, AnnotationInitCandidate &Candidate) {
  if (F.isDeclaration() || !F.hasInternalLinkage() || !F.arg_empty() ||
      !F.getReturnType()->isVoidTy() || !isAnnotationInitName(F.getName()) ||
      F.getName().endswith(AnnotationInitHelperSuffix) || F.size() != 1) {
    return false;
  }

  BasicBlock &BB = F.getEntryBlock();
  auto *Ret = dyn_cast<ReturnInst>(BB.getTerminator());
  if (Ret == nullptr || Ret->getReturnValue() != nullptr) {
    return false;
  }

  Instruction *Last = Ret->getPrevNonDebugInstruction();
  if (Last == nullptr || !isStaticRefWrite(*Last)) {
    return false;
  }

  auto *StaticWrite = cast<CallInst>(Last);
  if (StaticWrite->getArgOperand(1) == nullptr ||
      !isa<GlobalVariable>(StaticWrite->getArgOperand(1))) {
    return false;
  }

  unsigned BodyInstCount = 0;
  for (Instruction &I : BB) {
    if (isBodyInstruction(I, StaticWrite)) {
      ++BodyInstCount;
    }
  }

  if (BodyInstCount == 0) {
    return false;
  }

  Candidate = {&F, &BB, StaticWrite, BodyInstCount};
  return true;
}

static bool matchAnnotationFactory(Function &F,
                                   AnnotationFactoryCandidate &Candidate) {
  if (F.isDeclaration() || !F.getReturnType()->isVoidTy() ||
      !isAnnotationFactoryName(F.getName()) ||
      F.getName().endswith(AnnotationInitHelperSuffix) || F.size() != 1) {
    return false;
  }

  BasicBlock &BB = F.getEntryBlock();
  auto *Ret = dyn_cast<ReturnInst>(BB.getTerminator());
  if (Ret == nullptr || Ret->getReturnValue() != nullptr) {
    return false;
  }

  SmallVector<CallInst *, 4> StaticReads;
  unsigned BodyInstCount = 0;
  for (Instruction &I : BB) {
    if (!isHelperInstruction(I)) {
      continue;
    }
    ++BodyInstCount;
    if (!isStaticRefRead(I)) {
      continue;
    }

    auto *StaticRead = cast<CallInst>(&I);
    if (!isa<GlobalVariable>(StaticRead->getArgOperand(0))) {
      return false;
    }
    StaticReads.push_back(StaticRead);
  }

  if (StaticReads.empty()) {
    return false;
  }

  Candidate = {&F, &BB, std::move(StaticReads), BodyInstCount};
  return true;
}

static bool hasSameNonDebugMetadata(const Instruction &LHS,
                                    const Instruction &RHS) {
  SmallVector<std::pair<unsigned, MDNode *>, 4> LHSMetadata;
  SmallVector<std::pair<unsigned, MDNode *>, 4> RHSMetadata;
  LHS.getAllMetadataOtherThanDebugLoc(LHSMetadata);
  RHS.getAllMetadataOtherThanDebugLoc(RHSMetadata);
  return LHSMetadata == RHSMetadata;
}

static bool hasSameMappedOperand(
    const Value *LHS, const Value *RHS,
    const DenseMap<const Value *, const Value *> &ValueMap) {
  if (LHS == RHS) {
    return true;
  }

  auto It = ValueMap.find(LHS);
  if (It != ValueMap.end()) {
    return It->second == RHS;
  }

  return isa<Constant>(LHS) && isa<Constant>(RHS) && LHS == RHS;
}

static bool haveEquivalentBodies(const AnnotationInitCandidate &LHS,
                                 const AnnotationInitCandidate &RHS) {
  if (LHS.BodyInstCount != RHS.BodyInstCount) {
    return false;
  }

  DenseMap<const Value *, const Value *> ValueMap;
  auto LHSIt = LHS.BB->begin();
  auto RHSIt = RHS.BB->begin();

  while (LHSIt != LHS.BB->end() && RHSIt != RHS.BB->end()) {
    while (LHSIt != LHS.BB->end() &&
           !isBodyInstruction(*LHSIt, LHS.FinalStaticWrite)) {
      ++LHSIt;
    }
    while (RHSIt != RHS.BB->end() &&
           !isBodyInstruction(*RHSIt, RHS.FinalStaticWrite)) {
      ++RHSIt;
    }

    if (LHSIt == LHS.BB->end() || RHSIt == RHS.BB->end()) {
      return LHSIt == LHS.BB->end() && RHSIt == RHS.BB->end();
    }

    if (!LHSIt->isSameOperationAs(&*RHSIt) ||
        !hasSameNonDebugMetadata(*LHSIt, *RHSIt) ||
        LHSIt->getNumOperands() != RHSIt->getNumOperands()) {
      return false;
    }

    for (unsigned OperandIndex = 0; OperandIndex < LHSIt->getNumOperands();
         ++OperandIndex) {
      if (!hasSameMappedOperand(LHSIt->getOperand(OperandIndex),
                                RHSIt->getOperand(OperandIndex), ValueMap)) {
        return false;
      }
    }

    ValueMap[&*LHSIt] = &*RHSIt;
    ++LHSIt;
    ++RHSIt;
  }

  return LHS.FinalStaticWrite->isSameOperationAs(RHS.FinalStaticWrite) &&
         hasSameNonDebugMetadata(*LHS.FinalStaticWrite, *RHS.FinalStaticWrite) &&
         hasSameMappedOperand(LHS.FinalStaticWrite->getArgOperand(0),
                              RHS.FinalStaticWrite->getArgOperand(0), ValueMap);
}

static bool haveEquivalentFactoryInstruction(
    const Instruction &LHS, const Instruction &RHS,
    const DenseMap<const Value *, const Value *> &ValueMap) {
  if (!LHS.isSameOperationAs(&RHS) || !hasSameNonDebugMetadata(LHS, RHS) ||
      LHS.getNumOperands() != RHS.getNumOperands()) {
    return false;
  }

  for (unsigned OperandIndex = 0; OperandIndex < LHS.getNumOperands();
       ++OperandIndex) {
    if (isStaticRefRead(LHS) && OperandIndex == 0) {
      if (!isa<GlobalVariable>(LHS.getOperand(OperandIndex)) ||
          !isa<GlobalVariable>(RHS.getOperand(OperandIndex)) ||
          LHS.getOperand(OperandIndex)->getType() !=
              RHS.getOperand(OperandIndex)->getType()) {
        return false;
      }
      continue;
    }

    if (!hasSameMappedOperand(LHS.getOperand(OperandIndex),
                              RHS.getOperand(OperandIndex), ValueMap)) {
      return false;
    }
  }

  return true;
}

static bool haveEquivalentFactories(const AnnotationFactoryCandidate &LHS,
                                    const AnnotationFactoryCandidate &RHS) {
  if (LHS.BodyInstCount != RHS.BodyInstCount ||
      LHS.StaticReads.size() != RHS.StaticReads.size() ||
      LHS.Fn->arg_size() != RHS.Fn->arg_size()) {
    return false;
  }

  DenseMap<const Value *, const Value *> ValueMap;
  auto LHSArgIt = LHS.Fn->arg_begin();
  auto RHSArgIt = RHS.Fn->arg_begin();
  while (LHSArgIt != LHS.Fn->arg_end() && RHSArgIt != RHS.Fn->arg_end()) {
    if (LHSArgIt->getType() != RHSArgIt->getType()) {
      return false;
    }
    ValueMap[&*LHSArgIt] = &*RHSArgIt;
    ++LHSArgIt;
    ++RHSArgIt;
  }

  auto LHSIt = LHS.BB->begin();
  auto RHSIt = RHS.BB->begin();

  while (LHSIt != LHS.BB->end() && RHSIt != RHS.BB->end()) {
    while (LHSIt != LHS.BB->end() && !isHelperInstruction(*LHSIt)) {
      ++LHSIt;
    }
    while (RHSIt != RHS.BB->end() && !isHelperInstruction(*RHSIt)) {
      ++RHSIt;
    }

    if (LHSIt == LHS.BB->end() || RHSIt == RHS.BB->end()) {
      return LHSIt == LHS.BB->end() && RHSIt == RHS.BB->end();
    }

    if (!haveEquivalentFactoryInstruction(*LHSIt, *RHSIt, ValueMap)) {
      return false;
    }

    ValueMap[&*LHSIt] = &*RHSIt;
    ++LHSIt;
    ++RHSIt;
  }

  return true;
}

static bool isProfitableToDedup(unsigned GroupSize, unsigned BodyInstCount) {
  if (GroupSize < 2 || BodyInstCount == 0) {
    return false;
  }

  // Before: K * (N body instructions + static write + ret)
  // After:  (N body instructions + static write + ret) + K * (helper call + ret)
  // The transform only runs when this coarse instruction-count model is
  // strictly positive, which filters out tiny helpers that usually grow code.
  return static_cast<uint64_t>(GroupSize - 1) * BodyInstCount > 2;
}

static std::string createHelperName(Module &M, StringRef BaseName) {
  std::string Name = (BaseName + AnnotationInitHelperSuffix).str();
  if (M.getNamedValue(Name) == nullptr) {
    return Name;
  }

  unsigned Suffix = 0;
  do {
    Name = (BaseName + AnnotationInitHelperSuffix + "." +
            std::to_string(++Suffix))
               .str();
  } while (M.getNamedValue(Name) != nullptr);

  return Name;
}

static Function &createSharedHelper(const AnnotationInitCandidate &Candidate) {
  Function &Original = *Candidate.Fn;
  Module &M = *Original.getParent();
  LLVMContext &Ctx = M.getContext();
  Type *StaticSlotTy = Candidate.FinalStaticWrite->getArgOperand(1)->getType();
  auto *HelperTy = FunctionType::get(Type::getVoidTy(Ctx), {StaticSlotTy}, false);

  auto *Helper =
      Function::Create(HelperTy, GlobalValue::InternalLinkage,
                       createHelperName(M, Original.getName()), &M);
  Helper->getArg(0)->setName("slot");
  Helper->setCallingConv(Original.getCallingConv());
  Helper->setAttributes(AttributeList::get(
      Ctx, AttributeList::FunctionIndex, Original.getAttributes().getFnAttrs()));
  if (Original.hasGC()) {
    Helper->setGC(Original.getGC());
  }
  if (Original.hasPersonalityFn()) {
    Helper->setPersonalityFn(Original.getPersonalityFn());
  }
  Helper->setUnnamedAddr(GlobalValue::UnnamedAddr::Local);
  Helper->addFnAttr(Attribute::NoInline);

  BasicBlock *Entry = BasicBlock::Create(Ctx, Candidate.BB->getName(), Helper);
  DenseMap<const Value *, Value *> ValueMap;
  ValueMap[Candidate.FinalStaticWrite->getArgOperand(1)] = Helper->getArg(0);

  for (Instruction &I : *Candidate.BB) {
    if (!isHelperInstruction(I)) {
      continue;
    }

    Instruction *Clone = I.clone();
    for (unsigned OperandIndex = 0; OperandIndex < Clone->getNumOperands();
         ++OperandIndex) {
      if (Value *Mapped = ValueMap.lookup(Clone->getOperand(OperandIndex))) {
        Clone->setOperand(OperandIndex, Mapped);
      }
    }

    Entry->getInstList().push_back(Clone);
    ValueMap[&I] = Clone;
  }

  ReturnInst::Create(Ctx, nullptr, Entry);
  return *Helper;
}

static Function &createSharedFactoryHelper(
    const AnnotationFactoryCandidate &Candidate) {
  Function &Original = *Candidate.Fn;
  Module &M = *Original.getParent();
  LLVMContext &Ctx = M.getContext();
  SmallVector<Type *, 8> ParamTypes;

  for (Argument &Arg : Original.args()) {
    ParamTypes.push_back(Arg.getType());
  }
  for (CallInst *StaticRead : Candidate.StaticReads) {
    ParamTypes.push_back(StaticRead->getArgOperand(0)->getType());
  }

  auto *HelperTy = FunctionType::get(Type::getVoidTy(Ctx), ParamTypes, false);
  auto *Helper =
      Function::Create(HelperTy, GlobalValue::InternalLinkage,
                       createHelperName(M, Original.getName()), &M);
  Helper->setCallingConv(Original.getCallingConv());
  SmallVector<AttributeSet, 8> ParamAttrs(ParamTypes.size());
  for (unsigned ParamIndex = 0; ParamIndex < Original.arg_size(); ++ParamIndex) {
    ParamAttrs[ParamIndex] =
        Original.getAttributes().getParamAttrs(ParamIndex);
  }
  Helper->setAttributes(AttributeList::get(
      Ctx, Original.getAttributes().getFnAttrs(),
      Original.getAttributes().getRetAttrs(), ParamAttrs));
  if (Original.hasGC()) {
    Helper->setGC(Original.getGC());
  }
  if (Original.hasPersonalityFn()) {
    Helper->setPersonalityFn(Original.getPersonalityFn());
  }
  Helper->setUnnamedAddr(GlobalValue::UnnamedAddr::Local);
  Helper->addFnAttr(Attribute::NoInline);

  DenseMap<const Value *, Value *> ValueMap;
  DenseMap<const Instruction *, Value *> SlotArgMap;
  auto HelperArgIt = Helper->arg_begin();

  for (Argument &OriginalArg : Original.args()) {
    HelperArgIt->setName(OriginalArg.getName());
    ValueMap[&OriginalArg] = &*HelperArgIt;
    ++HelperArgIt;
  }

  for (unsigned SlotIndex = 0; SlotIndex < Candidate.StaticReads.size();
       ++SlotIndex) {
    HelperArgIt->setName("slot" + std::to_string(SlotIndex));
    SlotArgMap[Candidate.StaticReads[SlotIndex]] = &*HelperArgIt;
    ++HelperArgIt;
  }

  BasicBlock *Entry = BasicBlock::Create(Ctx, Candidate.BB->getName(), Helper);
  for (Instruction &I : *Candidate.BB) {
    if (!isHelperInstruction(I)) {
      continue;
    }

    Instruction *Clone = I.clone();
    for (unsigned OperandIndex = 0; OperandIndex < Clone->getNumOperands();
         ++OperandIndex) {
      if (isStaticRefRead(I) && OperandIndex == 0) {
        Clone->setOperand(OperandIndex, SlotArgMap.lookup(&I));
        continue;
      }
      if (Value *Mapped = ValueMap.lookup(Clone->getOperand(OperandIndex))) {
        Clone->setOperand(OperandIndex, Mapped);
      }
    }

    Entry->getInstList().push_back(Clone);
    ValueMap[&I] = Clone;
  }

  ReturnInst::Create(Ctx, nullptr, Entry);
  return *Helper;
}

static void rewriteAsWrapper(const AnnotationInitCandidate &Candidate,
                             Function &Helper) {
  Function &Original = *Candidate.Fn;
  LLVMContext &Ctx = Original.getContext();
  Function *StaticWriteDecl = Candidate.FinalStaticWrite->getCalledFunction();
  Value *StaticSlot = Candidate.FinalStaticWrite->getArgOperand(1);
  DebugLoc DL = Candidate.FinalStaticWrite->getDebugLoc();

  Original.deleteBody();

  BasicBlock *Entry = BasicBlock::Create(Ctx, "bb3", &Original);
  IRBuilder<> Builder(Entry);
  (void)StaticWriteDecl;
  auto *HelperCall = Builder.CreateCall(&Helper, {StaticSlot});
  HelperCall->setDebugLoc(DL);
  auto *Ret = Builder.CreateRetVoid();
  Ret->setDebugLoc(DL);
}

static void rewriteFactoryAsWrapper(const AnnotationFactoryCandidate &Candidate,
                                    Function &Helper) {
  Function &Original = *Candidate.Fn;
  LLVMContext &Ctx = Original.getContext();
  SmallVector<Value *, 8> HelperArgs;
  SmallVector<Value *, 4> StaticSlots;
  DebugLoc DL;

  for (CallInst *StaticRead : Candidate.StaticReads) {
    StaticSlots.push_back(StaticRead->getArgOperand(0));
    if (!DL) {
      DL = StaticRead->getDebugLoc();
    }
  }

  Original.deleteBody();

  for (Argument &Arg : Original.args()) {
    HelperArgs.push_back(&Arg);
  }
  HelperArgs.append(StaticSlots.begin(), StaticSlots.end());

  BasicBlock *Entry = BasicBlock::Create(Ctx, "bb0", &Original);
  IRBuilder<> Builder(Entry);
  auto *HelperCall = Builder.CreateCall(&Helper, HelperArgs);
  HelperCall->setDebugLoc(DL);
  auto *Ret = Builder.CreateRetVoid();
  Ret->setDebugLoc(DL);
}

} // namespace

PreservedAnalyses CJAnnotationInitDedup::run(Module &M,
                                             ModuleAnalysisManager &) const {
  SmallVector<AnnotationInitCandidate, 16> Candidates;
  SmallVector<AnnotationFactoryCandidate, 16> FactoryCandidates;
  for (Function &F : M) {
    AnnotationInitCandidate Candidate;
    if (matchAnnotationInit(F, Candidate)) {
      Candidates.push_back(Candidate);
    }

    AnnotationFactoryCandidate FactoryCandidate;
    if (matchAnnotationFactory(F, FactoryCandidate)) {
      FactoryCandidates.push_back(std::move(FactoryCandidate));
    }
  }

  bool Changed = false;
  if (Candidates.size() >= 2) {
    SmallVector<SmallVector<unsigned, 4>, 8> Groups;
    for (unsigned CandidateIndex = 0; CandidateIndex < Candidates.size();
         ++CandidateIndex) {
      bool FoundGroup = false;
      for (SmallVector<unsigned, 4> &Group : Groups) {
        if (haveEquivalentBodies(Candidates[CandidateIndex],
                                 Candidates[Group.front()])) {
          Group.push_back(CandidateIndex);
          FoundGroup = true;
          break;
        }
      }

      if (!FoundGroup) {
        Groups.emplace_back();
        Groups.back().push_back(CandidateIndex);
      }
    }

    for (const SmallVector<unsigned, 4> &Group : Groups) {
      const auto &Representative = Candidates[Group.front()];
      if (!isProfitableToDedup(Group.size(), Representative.BodyInstCount)) {
        continue;
      }

      Function &Helper = createSharedHelper(Representative);
      for (unsigned CandidateIndex : Group) {
        rewriteAsWrapper(Candidates[CandidateIndex], Helper);
      }
      Changed = true;
    }
  }

  if (FactoryCandidates.size() >= 2) {
    SmallVector<SmallVector<unsigned, 4>, 8> Groups;
    for (unsigned CandidateIndex = 0; CandidateIndex < FactoryCandidates.size();
         ++CandidateIndex) {
      bool FoundGroup = false;
      for (SmallVector<unsigned, 4> &Group : Groups) {
        if (haveEquivalentFactories(FactoryCandidates[CandidateIndex],
                                    FactoryCandidates[Group.front()])) {
          Group.push_back(CandidateIndex);
          FoundGroup = true;
          break;
        }
      }

      if (!FoundGroup) {
        Groups.emplace_back();
        Groups.back().push_back(CandidateIndex);
      }
    }

    for (const SmallVector<unsigned, 4> &Group : Groups) {
      const auto &Representative = FactoryCandidates[Group.front()];
      if (!isProfitableToDedup(Group.size(), Representative.BodyInstCount)) {
        continue;
      }

      Function &Helper = createSharedFactoryHelper(Representative);
      for (unsigned CandidateIndex : Group) {
        rewriteFactoryAsWrapper(FactoryCandidates[CandidateIndex], Helper);
      }
      Changed = true;
    }
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
