//===- CJAnnotationOutline.cpp --------------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This pass outlines the bodies of Cangjie annotation init and annotation
// factory functions that are identical except for the static slots they touch:
// one representative body is kept in a shared helper taking the slots as
// parameters, and every original function becomes a tiny thunk forwarding its
// own slot.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJAnnotationOutline.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
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
};

struct AnnotationFactoryCandidate {
  Function *Fn = nullptr;
  BasicBlock *BB = nullptr;
  SmallVector<CallInst *> StaticReads;
};

static bool isAnnotationInitName(StringRef Name) {
  return Name.startswith(AnnotationInitPrefix) &&
         Name.endswith(AnnotationInitSuffix);
}

static bool isAnnotationFactoryName(StringRef Name) {
  return Name.startswith(AnnotationFactoryPrefix);
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

// Match the per-instance annotation initializers emitted by the frontend
// (see TranslateAnnotation.cpp): a single-block, argument-less internal init
// whose last statement writes the static slot of one annotation instance, i.e.
//   define internal void @_CAO..K<idx>iiHv() {
//     <body>; gcwrite.static.ref(%obj, @_CAO..K<idx>E); ret void
//   }
// <idx> is the base62-mangled annotation index (MangleUtils.h) and the trailing
// iiHv marks the initializer, so this name check needs no extra state. Body
// contents are not inspected here; haveEquivalentBodies compares them later on.
static bool matchAnnotationInit(Function &F,
                                AnnotationInitCandidate &Candidate) {
  if (!F.hasCangjieGC() || F.isDeclaration() || !F.hasInternalLinkage() ||
      !F.arg_empty() || !F.getReturnType()->isVoidTy() ||
      !isAnnotationInitName(F.getName()) ||
      F.getName().endswith(AnnotationInitHelperSuffix) || F.size() != 1) {
    return false;
  }

  BasicBlock &BB = F.getEntryBlock();
  auto *Ret = dyn_cast<ReturnInst>(BB.getTerminator());
  if (Ret == nullptr) {
    return false;
  }

  Instruction *Last = Ret->getPrevNonDebugInstruction();
  if (Last == nullptr || !isStaticRefWrite(*Last)) {
    return false;
  }

  auto *StaticWrite = cast<CallInst>(Last);
  if (!isa<GlobalVariable>(StaticWrite->getArgOperand(1))) {
    return false;
  }

  Candidate = {&F, &BB, StaticWrite};
  return true;
}

// Match the annotation factory of a declaration (_CAF..Hv): it reads the static
// slots (_CAO..K<i>E) of its annotation instances into the annotation array.
static bool matchAnnotationFactory(Function &F,
                                   AnnotationFactoryCandidate &Candidate) {
  if (!F.hasCangjieGC() || F.isDeclaration() ||
      !F.getReturnType()->isVoidTy() || !isAnnotationFactoryName(F.getName()) ||
      F.getName().endswith(AnnotationInitHelperSuffix) || F.size() != 1) {
    return false;
  }

  BasicBlock &BB = F.getEntryBlock();
  auto *Ret = dyn_cast<ReturnInst>(BB.getTerminator());
  if (Ret == nullptr) {
    return false;
  }

  SmallVector<CallInst *> StaticReads;
  for (Instruction &I : BB) {
    if (!isHelperInstruction(I)) {
      continue;
    }
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

  Candidate = {&F, &BB, std::move(StaticReads)};
  return true;
}

static bool hasSameNonDebugMetadata(const Instruction &LHS,
                                    const Instruction &RHS) {
  SmallVector<std::pair<unsigned, MDNode *>> LHSMetadata;
  SmallVector<std::pair<unsigned, MDNode *>> RHSMetadata;
  LHS.getAllMetadataOtherThanDebugLoc(LHSMetadata);
  RHS.getAllMetadataOtherThanDebugLoc(RHSMetadata);
  return LHSMetadata == RHSMetadata;
}

// Operands correspond if they are literally the same value (constants are
// uniqued per context, so identical constants share a pointer and different
// ones never do), or if ValueMap already pairs them up (the static slot, or an
// instruction compared earlier).
static bool
hasSameMappedOperand(const Value *LHS, const Value *RHS,
                     const DenseMap<const Value *, const Value *> &ValueMap) {
  if (LHS == RHS) {
    return true;
  }

  auto It = ValueMap.find(LHS);
  return It != ValueMap.end() && It->second == RHS;
}

// Instructions match if they are the same operation (opcode, type and special
// state: cmp predicate, GEP source element type, call conv and attributes, see
// Instruction::isSameOperationAs), carry the same non-debug metadata, and every
// operand corresponds. isSameOperationAs already guarantees equal operand
// counts, so iterating over LHS' count below is safe.
static bool
hasSameOperation(const Instruction &LHS, const Instruction &RHS,
                 const DenseMap<const Value *, const Value *> &ValueMap) {
  if (!LHS.isSameOperationAs(&RHS) || !hasSameNonDebugMetadata(LHS, RHS)) {
    return false;
  }

  for (unsigned OperandIndex = 0; OperandIndex < LHS.getNumOperands();
       ++OperandIndex) {
    if (!hasSameMappedOperand(LHS.getOperand(OperandIndex),
                              RHS.getOperand(OperandIndex), ValueMap)) {
      return false;
    }
  }
  return true;
}

// Two annotation inits are equivalent when their blocks match instruction by
// instruction, skipping debug/pseudo instructions. The static slot is the only
// operand allowed to differ: it is pre-mapped into ValueMap, because it becomes
// a parameter of the shared helper. Everything else must match, including the
// object written by the final gcwrite.static.ref (argOperand(0)); otherwise the
// helper would write one init's object into another init's slot.
static bool haveEquivalentBodies(const AnnotationInitCandidate &LHS,
                                 const AnnotationInitCandidate &RHS) {
  DenseMap<const Value *, const Value *> ValueMap;
  Value *LSlot = LHS.FinalStaticWrite->getArgOperand(1);
  Value *RSlot = RHS.FinalStaticWrite->getArgOperand(1);
  ValueMap[LSlot] = RSlot;

  auto LHSIt = LHS.BB->begin();
  auto RHSIt = RHS.BB->begin();
  while (true) {
    while (LHSIt != LHS.BB->end() && LHSIt->isDebugOrPseudoInst()) {
      ++LHSIt;
    }
    while (RHSIt != RHS.BB->end() && RHSIt->isDebugOrPseudoInst()) {
      ++RHSIt;
    }

    if (LHSIt == LHS.BB->end() || RHSIt == RHS.BB->end()) {
      return LHSIt == LHS.BB->end() && RHSIt == RHS.BB->end();
    }

    if (!hasSameOperation(*LHSIt, *RHSIt, ValueMap)) {
      return false;
    }

    // Remember "this instruction corresponds to that one" for later operands.
    ValueMap[&*LHSIt] = &*RHSIt;
    ++LHSIt;
    ++RHSIt;
  }
}

// Like hasSameOperation, but a static read's slot is allowed to differ. The
// slot is operand 0 there; its type is already compared by isSameOperationAs,
// and matchAnnotationFactory guarantees both operands are globals.
static bool haveEquivalentFactoryInstruction(
    const Instruction &LHS, const Instruction &RHS,
    const DenseMap<const Value *, const Value *> &ValueMap) {
  if (!LHS.isSameOperationAs(&RHS) || !hasSameNonDebugMetadata(LHS, RHS)) {
    return false;
  }

  for (unsigned OperandIndex = 0; OperandIndex < LHS.getNumOperands();
       ++OperandIndex) {
    if (isStaticRefRead(LHS) && OperandIndex == 0) {
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
  if (LHS.StaticReads.size() != RHS.StaticReads.size() ||
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
  while (true) {
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
}

// Outlining replaces GroupSize copies of the block with one copy plus a
// call/ret wrapper per member: GroupSize * BlockSize becomes BlockSize + 2 *
// GroupSize.
static bool isProfitableToOutline(uint64_t GroupSize, uint64_t BlockSize) {
  if (GroupSize < 2) {
    return false;
  }

  return (GroupSize - 1) * BlockSize > 2 * GroupSize;
}

// Instructions that cloneHelperBody actually copies: debug and pseudo
// instructions are skipped, so they must not make a body look worth outlining.
static uint64_t countClonedInstructions(const BasicBlock &BB) {
  uint64_t Count = 0;
  for (const Instruction &I : BB) {
    Count += isHelperInstruction(I) ? 1 : 0;
  }
  return Count;
}

static std::string createHelperName(Module &M, StringRef BaseName) {
  std::string Name = (BaseName + AnnotationInitHelperSuffix).str();
  if (M.getNamedValue(Name) == nullptr) {
    return Name;
  }

  unsigned Suffix = 0;
  do {
    Name =
        (BaseName + AnnotationInitHelperSuffix + "." + std::to_string(++Suffix))
            .str();
  } while (M.getNamedValue(Name) != nullptr);

  return Name;
}

// Creates the empty shared helper: same name scheme, calling convention,
// attributes, GC and personality as the original. An inherited alwaysinline is
// dropped because it would conflict with the noinline added afterwards.
static Function *createHelperFunction(Function &Original,
                                      FunctionType *HelperTy,
                                      AttributeList Attrs) {
  Module &M = *Original.getParent();
  auto *Helper = Function::Create(HelperTy, GlobalValue::InternalLinkage,
                                  createHelperName(M, Original.getName()), &M);
  Helper->setCallingConv(Original.getCallingConv());
  Helper->setAttributes(Attrs);
  if (Original.hasGC()) {
    Helper->setGC(Original.getGC());
  }
  if (Original.hasPersonalityFn()) {
    Helper->setPersonalityFn(Original.getPersonalityFn());
  }
  Helper->setUnnamedAddr(GlobalValue::UnnamedAddr::Local);
  Helper->removeFnAttr(Attribute::AlwaysInline);
  Helper->addFnAttr(Attribute::NoInline);
  return Helper;
}

// Clones the representative body into Helper, remapping operands through VMap
// on the way; every clone is added to the map so later ones can reference it.
// The helper has no DISubprogram of its own, so inherited debug locations would
// attribute its instructions to the representative; drop them instead.
static void cloneHelperBody(BasicBlock *Source, ValueToValueMapTy &VMap,
                            Function *Helper) {
  LLVMContext &Ctx = Helper->getContext();
  BasicBlock *Entry = BasicBlock::Create(Ctx, Source->getName(), Helper);
  for (Instruction &I : *Source) {
    if (!isHelperInstruction(I)) {
      continue;
    }

    Instruction *Clone = I.clone();
    RemapInstruction(Clone, VMap, RF_NoModuleLevelChanges);
    Clone->setDebugLoc(DebugLoc());
    Entry->getInstList().push_back(Clone);
    VMap[&I] = Clone;
  }

  ReturnInst::Create(Ctx, nullptr, Entry);
}

// Builds the body a group shares, with its final static write parameterised:
//     define internal void @_CAO..foo_iiHv_imp(i8 addrspace(1)** %slot) {
//       ...the representative's body...
//       call void @llvm.cj.gcwrite.static.ref(%obj, %slot)
//       ret void
//     }
static Function &createSharedHelper(const AnnotationInitCandidate &Candidate) {
  Function &Original = *Candidate.Fn;
  LLVMContext &Ctx = Original.getContext();
  Type *StaticSlotTy = Candidate.FinalStaticWrite->getArgOperand(1)->getType();
  auto *HelperTy =
      FunctionType::get(Type::getVoidTy(Ctx), {StaticSlotTy}, false);

  auto *Helper = createHelperFunction(
      Original, HelperTy,
      AttributeList::get(Ctx, AttributeList::FunctionIndex,
                         Original.getAttributes().getFnAttrs()));
  Helper->getArg(0)->setName("slot");

  // Seeding the slot is what swaps it for the argument; each clone is then
  // added to the map so the next ones can be remapped against it.
  ValueToValueMapTy VMap;
  VMap[Candidate.FinalStaticWrite->getArgOperand(1)] = Helper->getArg(0);
  cloneHelperBody(Candidate.BB, VMap, Helper);
  return *Helper;
}

// Same as createSharedHelper, for factories: the helper keeps the factory's own
// arguments and gains one %slotN per static read, which the reads are rewritten
// to use:
//     define void @_CAF..fooE_imp(%Arr* %dst, i8 addrspace(1)** %slot0, ...) {
//       %v = call i8 addrspace(1)* @llvm.cj.gcread.static.ref(%slot0)
//       ...
//       ret void
//     }
static Function &
createSharedFactoryHelper(const AnnotationFactoryCandidate &Candidate) {
  Function &Original = *Candidate.Fn;
  LLVMContext &Ctx = Original.getContext();
  SmallVector<Type *> ParamTypes;
  for (Argument &Arg : Original.args()) {
    ParamTypes.push_back(Arg.getType());
  }
  for (CallInst *StaticRead : Candidate.StaticReads) {
    ParamTypes.push_back(StaticRead->getArgOperand(0)->getType());
  }

  auto *HelperTy = FunctionType::get(Type::getVoidTy(Ctx), ParamTypes, false);
  SmallVector<AttributeSet> ParamAttrs(ParamTypes.size());
  for (unsigned ParamIndex = 0; ParamIndex < Original.arg_size();
       ++ParamIndex) {
    ParamAttrs[ParamIndex] = Original.getAttributes().getParamAttrs(ParamIndex);
  }

  auto *Helper = createHelperFunction(
      Original, HelperTy,
      AttributeList::get(Ctx, Original.getAttributes().getFnAttrs(),
                         Original.getAttributes().getRetAttrs(), ParamAttrs));

  // The helper takes the factory's own arguments first, then one %slotN per
  // static read; a read is remapped by its slot global (operand 0).
  ValueToValueMapTy VMap;
  auto HelperArgIt = Helper->arg_begin();
  for (Argument &OriginalArg : Original.args()) {
    HelperArgIt->setName(OriginalArg.getName());
    VMap[&OriginalArg] = &*HelperArgIt;
    ++HelperArgIt;
  }
  for (unsigned SlotIndex = 0; SlotIndex < Candidate.StaticReads.size();
       ++SlotIndex) {
    HelperArgIt->setName("slot" + std::to_string(SlotIndex));
    VMap[Candidate.StaticReads[SlotIndex]->getArgOperand(0)] = &*HelperArgIt;
    ++HelperArgIt;
  }

  cloneHelperBody(Candidate.BB, VMap, Helper);
  return *Helper;
}

// Replace this member's body with a call to the group's shared helper.
//
//   before:
//     define internal void @_CAO..foo_iiHv() {
//       %obj = call i8 addrspace(1)* @llvm.cj.malloc.object(
//                i8* bitcast (%TypeInfo* @"pkg:Anno.ti" to i8*), i32 24)
//       ...                          ; payload gep/bitcast, stores of the field
//       values call void @llvm.cj.gcwrite.static.ref(
//                i8 addrspace(1)* %obj, i8 addrspace(1)** @_CAO..fooE)   ; <-
//                only difference
//       ret void
//     }
//
//   after:
//     define internal void @_CAO..foo_iiHv() {
//       call void @_CAO..foo_iiHv_imp(i8 addrspace(1)** @_CAO..fooE)
//       ret void
//     }
static void rewriteAsWrapper(const AnnotationInitCandidate &Candidate,
                             Function &Helper) {
  Function &Original = *Candidate.Fn;
  LLVMContext &Ctx = Original.getContext();
  // @_CAO..fooE
  auto *GV = cast<GlobalVariable>(Candidate.FinalStaticWrite->getArgOperand(1));
  DebugLoc DL = Candidate.FinalStaticWrite->getDebugLoc();

  // deleteBody() also resets the linkage; keep the original one.
  auto Linkage = Original.getLinkage();
  Original.deleteBody();
  Original.setLinkage(Linkage);

  BasicBlock *Entry = BasicBlock::Create(Ctx, "bb3", &Original);
  IRBuilder<> Builder(Entry);
  // call void @_CAO..foo_iiHv_imp(i8 addrspace(1)** @_CAO..fooE)
  auto *HelperCall = Builder.CreateCall(&Helper, {GV});
  HelperCall->setCallingConv(Helper.getCallingConv());
  // Keep the callee's declared parameter attributes (e.g. nocapture) on the
  // call.
  HelperCall->setAttributes(Helper.getAttributes().removeFnAttributes(Ctx));
  HelperCall->setDebugLoc(DL);
  auto *Ret = Builder.CreateRetVoid();
  Ret->setDebugLoc(DL);
}

// Same as rewriteAsWrapper, for factories: every static ref read by the factory
// becomes an extra helper argument, appended after the original arguments.
//
//   before:  define void @_CAF..fooE(%Arr* sret(%Arr) %dst) { ...;
//   gcread.static.ref(@_CAO..aE); ... } after:   define void
//   @_CAF..fooE_imp(%Arr* %dst, i8 addrspace(1)** %slot0, ...) { ... }
//            define void @_CAF..fooE(%Arr* %dst) { call @_CAF..fooE_imp(%dst,
//            @_CAO..aE, ...); ret void }
static void rewriteFactoryAsWrapper(const AnnotationFactoryCandidate &Candidate,
                                    Function &Helper) {
  Function &Original = *Candidate.Fn;
  LLVMContext &Ctx = Original.getContext();
  SmallVector<Value *> HelperArgs;
  SmallVector<GlobalVariable *> GVs;
  DebugLoc DL;

  for (CallInst *StaticRead : Candidate.StaticReads) {
    GVs.push_back(cast<GlobalVariable>(StaticRead->getArgOperand(0)));
    if (!DL) {
      DL = StaticRead->getDebugLoc();
    }
  }

  // Same as rewriteAsWrapper: keep the original linkage.
  auto Linkage = Original.getLinkage();
  Original.deleteBody();
  Original.setLinkage(Linkage);

  // call void @_CAF..fooE_imp(%Arr* %dst, i8 addrspace(1)** @_CAO..aE,
  //                             i8 addrspace(1)** @_CAO..bE)
  for (Argument &Arg : Original.args()) {
    HelperArgs.push_back(&Arg);
  }
  HelperArgs.append(GVs.begin(), GVs.end());

  BasicBlock *Entry = BasicBlock::Create(Ctx, "bb0", &Original);
  IRBuilder<> Builder(Entry);
  auto *HelperCall = Builder.CreateCall(&Helper, HelperArgs);
  HelperCall->setCallingConv(Helper.getCallingConv());
  // Keep the callee's declared parameter attributes (e.g. sret) on the call.
  HelperCall->setAttributes(Helper.getAttributes().removeFnAttributes(Ctx));
  HelperCall->setDebugLoc(DL);
  auto *Ret = Builder.CreateRetVoid();
  Ret->setDebugLoc(DL);
}

} // namespace

// Collects every function matching either outline-able shape in one pass over
// the module.
static void collectCandidates(
    Module &M, SmallVectorImpl<AnnotationInitCandidate> &InitCandidates,
    SmallVectorImpl<AnnotationFactoryCandidate> &FactoryCandidates) {
  for (Function &F : M) {
    AnnotationInitCandidate InitCandidate;
    if (matchAnnotationInit(F, InitCandidate)) {
      InitCandidates.push_back(InitCandidate);
    }

    AnnotationFactoryCandidate FactoryCandidate;
    if (matchAnnotationFactory(F, FactoryCandidate)) {
      FactoryCandidates.push_back(std::move(FactoryCandidate));
    }
  }
}

// Outlines one candidate kind: partition it into groups of equivalent bodies,
// then replace each profitable group with one shared helper plus a thin wrapper
// per member. Only the equivalence test, the helper constructor and the
// rewriter differ between the two kinds, so the caller passes them in. Bodies
// are equivalent iff they are identical except for the static slot, so the
// relation is transitive and comparing against the representative suffices.
template <typename CandidateT, typename EquivalentFn, typename CreateHelperFn,
          typename RewriteFn>
static bool groupAndOutline(const SmallVector<CandidateT> &Candidates,
                            EquivalentFn Equivalent,
                            CreateHelperFn CreateHelper, RewriteFn Rewrite) {
  SmallVector<SmallVector<unsigned>> Groups;
  for (unsigned CandidateIndex = 0; CandidateIndex < Candidates.size();
       ++CandidateIndex) {
    bool FoundGroup = false;
    for (auto &Group : Groups) {
      if (Equivalent(Candidates[CandidateIndex], Candidates[Group.front()])) {
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

  bool Changed = false;
  for (const auto &Group : Groups) {
    const CandidateT &Representative = Candidates[Group.front()];
    if (!isProfitableToOutline(Group.size(),
                               countClonedInstructions(*Representative.BB))) {
      continue;
    }

    // The representative defines the helper's body and slot type; every member
    // shares it (checked by Equivalent).
    Function &Helper = CreateHelper(Representative);
    for (unsigned CandidateIndex : Group) {
      Rewrite(Candidates[CandidateIndex], Helper);
    }
    Changed = true;
  }

  return Changed;
}

PreservedAnalyses CJAnnotationOutline::run(Module &M,
                                           ModuleAnalysisManager &) const {
  SmallVector<AnnotationInitCandidate> Candidates;
  SmallVector<AnnotationFactoryCandidate> FactoryCandidates;
  collectCandidates(M, Candidates, FactoryCandidates);

  bool Changed = groupAndOutline(Candidates, haveEquivalentBodies,
                                 createSharedHelper, rewriteAsWrapper);
  Changed |=
      groupAndOutline(FactoryCandidates, haveEquivalentFactories,
                      createSharedFactoryHelper, rewriteFactoryAsWrapper);

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
