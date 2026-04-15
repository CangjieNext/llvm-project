//===- CJRootMetadataMark.cpp ----------------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Marks the metadata globals that should be emitted into __CJ_METADATA after
// final static LTO when a root-bc keep set is provided.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJRootMetadataMark.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalIFunc.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Metadata.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/CJFillMetadata.h"

#include <fstream>

using namespace llvm;

static cl::opt<std::string>
    CJRootSymbolFile("cj-root-symbol-file", cl::Hidden, cl::init(""),
                     cl::desc("Symbol keep set file used by CJ root metadata marking"));
static cl::opt<std::string>
    CJRootBCFile("cj-root-bc-file", cl::Hidden, cl::init(""),
                 cl::desc("Root bc list file used by CJ root reflection pruning"));

namespace {
constexpr StringLiteral CJRootMetadataKeepMD = "cj.root.metadata.keep";
constexpr StringLiteral CJRootFunctionKeepMD = "cj.root.function.keep";

static void addKeepName(Module &M, NamedMDNode *KeepMD, StringSet<> &EmittedNames,
                        StringRef Name) {
  if (Name.empty() || !EmittedNames.insert(Name).second)
    return;
  auto *NameMD = MDString::get(M.getContext(), Name);
  KeepMD->addOperand(MDTuple::get(M.getContext(), NameMD));
}

static StringSet<> readRootSymbols() {
  StringSet<> Symbols;
  if (CJRootSymbolFile.empty())
    return Symbols;

  std::ifstream Input(CJRootSymbolFile);
  if (!Input.is_open())
    report_fatal_error(Twine("Failed to open cj-root-symbol-file: ") + CJRootSymbolFile);

  std::string Line;
  while (std::getline(Input, Line)) {
    auto CommentPos = Line.find('#');
    if (CommentPos != std::string::npos)
      Line.erase(CommentPos);
    auto Begin = Line.find_first_not_of(" \t\r\n");
    if (Begin == std::string::npos)
      continue;
    auto End = Line.find_last_not_of(" \t\r\n");
    Symbols.insert(Line.substr(Begin, End - Begin + 1));
  }
  return Symbols;
}

static std::string normalizeRootPackageEntry(std::string Line) {
  auto CommentPos = Line.find('#');
  if (CommentPos != std::string::npos)
    Line.erase(CommentPos);
  auto Begin = Line.find_first_not_of(" \t\r\n");
  if (Begin == std::string::npos)
    return {};
  auto End = Line.find_last_not_of(" \t\r\n");
  Line = Line.substr(Begin, End - Begin + 1);

  SmallString<256> Entry(Line);
  StringRef Ref(Entry);
  auto Slash = Ref.find_last_of("/\\");
  if (Slash != StringRef::npos)
    Ref = Ref.substr(Slash + 1);
  if (Ref.endswith(".bc"))
    Ref = Ref.drop_back(3);
  return Ref.str();
}

static StringSet<> readRootPackages() {
  StringSet<> Packages;
  if (CJRootBCFile.empty())
    return Packages;

  std::ifstream Input(CJRootBCFile);
  if (!Input.is_open())
    report_fatal_error(Twine("Failed to open cj-root-bc-file: ") + CJRootBCFile);

  std::string Line;
  while (std::getline(Input, Line)) {
    auto Pkg = normalizeRootPackageEntry(Line);
    if (!Pkg.empty())
      Packages.insert(Pkg);
  }
  return Packages;
}

static bool isRootSymbol(const GlobalValue &GV, const StringSet<> &RootSymbols) {
  return !GV.getName().empty() && RootSymbols.count(GV.getName());
}

static void collectReferencedGVs(Value *V, SmallPtrSetImpl<GlobalValue *> &KeepGVs,
                                 SmallPtrSetImpl<Constant *> &VisitedConsts,
                                 SmallVectorImpl<GlobalValue *> &Worklist) {
  if (!V)
    return;
  if (auto *GV = dyn_cast<GlobalValue>(V->stripPointerCasts())) {
    if (KeepGVs.insert(GV).second)
      Worklist.push_back(GV);
    return;
  }
  auto *C = dyn_cast<Constant>(V);
  if (!C || !VisitedConsts.insert(C).second)
    return;
  for (Value *Op : C->operands())
    collectReferencedGVs(Op, KeepGVs, VisitedConsts, Worklist);
}

static void seedRootGVs(Module &M, const StringSet<> &RootSymbols,
                        SmallPtrSetImpl<GlobalValue *> &KeepGVs,
                        SmallVectorImpl<GlobalValue *> &Worklist) {
  for (Function &F : M)
    if (isRootSymbol(F, RootSymbols) && KeepGVs.insert(&F).second)
      Worklist.push_back(&F);
  for (GlobalVariable &GV : M.globals())
    if (isRootSymbol(GV, RootSymbols) && KeepGVs.insert(&GV).second)
      Worklist.push_back(&GV);
  for (GlobalAlias &GA : M.aliases())
    if (isRootSymbol(GA, RootSymbols) && KeepGVs.insert(&GA).second)
      Worklist.push_back(&GA);
  for (GlobalIFunc &GIF : M.ifuncs())
    if (isRootSymbol(GIF, RootSymbols) && KeepGVs.insert(&GIF).second)
      Worklist.push_back(&GIF);
}

static void emitRootFunctionKeepNames(Module &M, const StringSet<> &RootSymbols) {
  if (auto *OldMD = M.getNamedMetadata(CJRootFunctionKeepMD))
    OldMD->eraseFromParent();
  auto *KeepMD = M.getOrInsertNamedMetadata(CJRootFunctionKeepMD);
  StringSet<> EmittedNames;
  for (Function &F : M) {
    if (!F.isDeclaration())
      addKeepName(M, KeepMD, EmittedNames, F.getName());
    else if (isRootSymbol(F, RootSymbols))
      addKeepName(M, KeepMD, EmittedNames, F.getName());
  }
  for (GlobalAlias &GA : M.aliases())
    if (isRootSymbol(GA, RootSymbols))
      addKeepName(M, KeepMD, EmittedNames, GA.getName());
  for (GlobalIFunc &GIF : M.ifuncs())
    if (isRootSymbol(GIF, RootSymbols))
      addKeepName(M, KeepMD, EmittedNames, GIF.getName());
}

static void collectRootClosure(Module &M, const StringSet<> &RootSymbols,
                               SmallPtrSetImpl<GlobalValue *> &KeepGVs) {
  SmallVector<GlobalValue *, 32> Worklist;
  SmallPtrSet<Constant *, 32> VisitedConsts;
  seedRootGVs(M, RootSymbols, KeepGVs, Worklist);

  while (!Worklist.empty()) {
    auto *GV = Worklist.pop_back_val();
    if (auto *F = dyn_cast<Function>(GV)) {
      for (Instruction &I : instructions(F))
        for (Value *Op : I.operands())
          collectReferencedGVs(Op, KeepGVs, VisitedConsts, Worklist);
      continue;
    }
    if (auto *GVar = dyn_cast<GlobalVariable>(GV)) {
      if (GVar->hasInitializer())
        collectReferencedGVs(GVar->getInitializer(), KeepGVs, VisitedConsts, Worklist);
      continue;
    }
    if (auto *GA = dyn_cast<GlobalAlias>(GV)) {
      collectReferencedGVs(GA->getAliasee(), KeepGVs, VisitedConsts, Worklist);
      continue;
    }
    if (auto *GIF = dyn_cast<GlobalIFunc>(GV))
      collectReferencedGVs(GIF->getResolver(), KeepGVs, VisitedConsts, Worklist);
  }
}

static void expandTypeMetadataDeps(Module &M, SmallPtrSetImpl<GlobalValue *> &KeepGVs) {
  DenseMap<Value *, SmallPtrSet<GlobalVariable *, 8>> ReverseDeps;
  for (GlobalVariable &GV : M.globals()) {
    if (GV.isCJMTable()) {
      if (GV.hasInitializer())
        ReverseDeps[GV.getInitializer()
                        ->getOperand(ExtensionDefFieldType::ET_TARGET_TYPE)
                        ->stripPointerCasts()]
            .insert(&GV);
      continue;
    }
    if (GV.isCJTypeExt()) {
      if (GV.hasInitializer())
        ReverseDeps[GV.getInitializer()->getOperand(0)->stripPointerCasts()].insert(&GV);
    }
  }

  SmallVector<GlobalValue *, 16> Worklist;
  for (GlobalValue *GV : KeepGVs)
    if (auto *GVar = dyn_cast<GlobalVariable>(GV))
      if (GVar->isCJTypeInfo() || GVar->isCJTypeTemplate())
        Worklist.push_back(GVar);

  while (!Worklist.empty()) {
    auto *GV = Worklist.pop_back_val();
    auto It = ReverseDeps.find(GV);
    if (It == ReverseDeps.end())
      continue;
    for (GlobalVariable *Dep : It->second) {
      if (KeepGVs.insert(Dep).second)
        Worklist.push_back(Dep);
    }
  }
}

static bool shouldExpandMetadataOperands(const GlobalVariable &GV) {
  if (GV.isCJMeta())
    return true;
  StringRef Name = GV.getName();
  return Name.endswith(".packageInfo") || Name.endswith(".reflectStr");
}

static void expandMetadataOperandDeps(Module &M,
                                      SmallPtrSetImpl<GlobalValue *> &KeepGVs) {
  SmallVector<GlobalValue *, 32> Worklist;
  SmallPtrSet<Constant *, 32> VisitedConsts;

  for (GlobalValue *GV : KeepGVs)
    if (auto *GVar = dyn_cast<GlobalVariable>(GV))
      if (shouldExpandMetadataOperands(*GVar))
        Worklist.push_back(GVar);

  while (!Worklist.empty()) {
    auto *GV = Worklist.pop_back_val();
    auto *GVar = dyn_cast<GlobalVariable>(GV);
    if (!GVar || !GVar->hasInitializer())
      continue;

    SmallVector<GlobalValue *, 16> NewlyAdded;
    collectReferencedGVs(GVar->getInitializer(), KeepGVs, VisitedConsts,
                         NewlyAdded);
    for (GlobalValue *NewGV : NewlyAdded) {
      auto *NewGVar = dyn_cast<GlobalVariable>(NewGV);
      if (NewGVar && shouldExpandMetadataOperands(*NewGVar))
        Worklist.push_back(NewGV);
    }
  }
}

static bool shouldKeepMetadataLikeGV(const GlobalValue &GV) {
  auto *GVar = dyn_cast<GlobalVariable>(&GV);
  if (!GVar)
    return false;
  return GVar->isCJMeta() || shouldExpandMetadataOperands(*GVar);
}

static void seedMetadataReferencedBySurvivingCode(
    Module &M, SmallPtrSetImpl<GlobalValue *> &KeepGVs) {
  SmallPtrSet<Constant *, 32> VisitedConsts;
  SmallVector<GlobalValue *, 16> Referenced;

  auto TryAddOperand = [&](Value *V) {
    Referenced.clear();
    collectReferencedGVs(V, KeepGVs, VisitedConsts, Referenced);
    for (GlobalValue *GV : Referenced) {
      if (!GV || !shouldKeepMetadataLikeGV(*GV))
        continue;
      KeepGVs.insert(GV);
    }
  };

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (Instruction &I : instructions(F))
      for (Value *Op : I.operands())
        TryAddOperand(Op);
  }

  for (GlobalVariable &GV : M.globals()) {
    if (!GV.hasInitializer() || GV.isCJMeta())
      continue;
    TryAddOperand(GV.getInitializer());
  }
}

static bool shouldKeepMetadataEntry(const GlobalValue *GV,
                                    const SmallPtrSetImpl<GlobalValue *> &KeepGVs) {
  return GV && KeepGVs.count(const_cast<GlobalValue *>(GV));
}

static StringRef getPackageNameFromMetadataGV(const GlobalVariable &GV) {
  StringRef Name = GV.getName();
  auto Colon = Name.find(':');
  if (Colon == StringRef::npos)
    return {};
  return Name.substr(0, Colon);
}

static bool shouldStripReflectionForGV(const GlobalVariable &GV,
                                       const StringSet<> &RootPackages) {
  auto Pkg = getPackageNameFromMetadataGV(GV);
  return !Pkg.empty() && !RootPackages.count(Pkg);
}

static bool rewriteTypeInfoReflection(GlobalVariable &GV) {
  if (!GV.hasInitializer())
    return false;
  auto *Init = dyn_cast<ConstantStruct>(GV.getInitializer());
  if (!Init)
    return false;

  SmallVector<Constant *, CIT_MAX> Ops;
  for (unsigned I = 0; I < Init->getNumOperands(); ++I)
    Ops.push_back(cast<Constant>(Init->getOperand(I)));

  bool Changed = false;
  if (auto *OldFlag = dyn_cast<ConstantInt>(Ops[CIT_FLAG])) {
    uint64_t NewFlag = OldFlag->getZExtValue() & ~static_cast<uint64_t>(TF_REFLECTION);
    if (NewFlag != OldFlag->getZExtValue()) {
      Ops[CIT_FLAG] = ConstantInt::get(OldFlag->getType(), NewFlag);
      Changed = true;
    }
  }
  auto *NullReflect = Constant::getNullValue(Ops[CIT_REFLECTION]->getType());
  if (Ops[CIT_REFLECTION] != NullReflect) {
    Ops[CIT_REFLECTION] = NullReflect;
    Changed = true;
  }
  if (!Changed)
    return false;

  GV.setInitializer(ConstantStruct::get(cast<StructType>(Init->getType()), Ops));
  return true;
}

static bool rewriteTypeTemplateReflection(GlobalVariable &GV) {
  if (!GV.hasInitializer())
    return false;
  auto *Init = dyn_cast<ConstantStruct>(GV.getInitializer());
  if (!Init)
    return false;

  SmallVector<Constant *, TT_MAX> Ops;
  for (unsigned I = 0; I < Init->getNumOperands(); ++I)
    Ops.push_back(cast<Constant>(Init->getOperand(I)));

  bool Changed = false;
  if (auto *OldFlag = dyn_cast<ConstantInt>(Ops[TT_FLAG])) {
    uint64_t NewFlag = OldFlag->getZExtValue() & ~static_cast<uint64_t>(TF_REFLECTION);
    if (NewFlag != OldFlag->getZExtValue()) {
      Ops[TT_FLAG] = ConstantInt::get(OldFlag->getType(), NewFlag);
      Changed = true;
    }
  }
  auto *NullReflect = Constant::getNullValue(Ops[TT_REFLECTION]->getType());
  if (Ops[TT_REFLECTION] != NullReflect) {
    Ops[TT_REFLECTION] = NullReflect;
    Changed = true;
  }
  if (!Changed)
    return false;

  GV.setInitializer(ConstantStruct::get(cast<StructType>(Init->getType()), Ops));
  return true;
}
} // namespace

PreservedAnalyses CJRootReflectionPrunePass::run(Module &M,
                                                 ModuleAnalysisManager &) const {
  if (CJRootBCFile.empty())
    return PreservedAnalyses::all();

  auto RootPackages = readRootPackages();
  if (RootPackages.empty())
    return PreservedAnalyses::all();

  bool Changed = false;
  for (GlobalVariable &GV : M.globals()) {
    if (!shouldStripReflectionForGV(GV, RootPackages))
      continue;
    if (GV.isCJTypeInfo())
      Changed |= rewriteTypeInfoReflection(GV);
    else if (GV.isCJTypeTemplate())
      Changed |= rewriteTypeTemplateReflection(GV);
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses CJRootMetadataMarkPass::run(Module &M, ModuleAnalysisManager &) const {
  if (CJRootSymbolFile.empty())
    return PreservedAnalyses::all();

  auto RootSymbols = readRootSymbols();
  if (RootSymbols.empty())
    return PreservedAnalyses::all();

  SmallPtrSet<GlobalValue *, 32> KeepGVs;
  collectRootClosure(M, RootSymbols, KeepGVs);
  seedMetadataReferencedBySurvivingCode(M, KeepGVs);
  expandTypeMetadataDeps(M, KeepGVs);
  expandMetadataOperandDeps(M, KeepGVs);
  emitRootFunctionKeepNames(M, RootSymbols);

  if (auto *OldMD = M.getNamedMetadata(CJRootMetadataKeepMD))
    OldMD->eraseFromParent();
  auto *KeepMD = M.getOrInsertNamedMetadata(CJRootMetadataKeepMD);

  StringSet<> EmittedNames;
  for (GlobalValue *GV : KeepGVs) {
    auto *GVar = dyn_cast<GlobalVariable>(GV);
    if (!GVar || !GVar->isCJMeta() || GVar->getName().empty())
      continue;
    addKeepName(M, KeepMD, EmittedNames, GVar->getName());
  }

  for (GlobalVariable &GV : M.globals()) {
    if (GV.isCJStaticGenericTI()) {
      if (!GV.hasInitializer())
        continue;
      for (Value *Op : GV.getInitializer()->operands()) {
        auto *RefGV = dyn_cast<GlobalValue>(cast<Constant>(Op)->stripPointerCasts());
        if (!shouldKeepMetadataEntry(RefGV, KeepGVs))
          continue;
        addKeepName(M, KeepMD, EmittedNames, RefGV->getName());
      }
      continue;
    }
    if (GV.isCJOuterTypeExtensions()) {
      if (!GV.hasInitializer())
        continue;
      for (Value *Op : GV.getInitializer()->operands()) {
        auto *RefGV = dyn_cast<GlobalValue>(cast<Constant>(Op)->stripPointerCasts());
        if (!shouldKeepMetadataEntry(RefGV, KeepGVs))
          continue;
        addKeepName(M, KeepMD, EmittedNames, RefGV->getName());
      }
    }
  }

  return PreservedAnalyses::none();
}
