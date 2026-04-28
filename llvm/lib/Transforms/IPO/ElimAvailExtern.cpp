//===- ElimAvailExtern.cpp - DCE unreachable internal functions -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This transform is designed to eliminate available external global
// definitions from the program, turning them into declarations.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/ElimAvailExtern.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Utils/GlobalStatus.h"

using namespace llvm;

#define DEBUG_TYPE "elim-avail-extern"

STATISTIC(NumFunctions, "Number of functions removed");
STATISTIC(NumVariables, "Number of global variables removed");

static bool isLTOPostLink(const Module &M) {
  auto *LTOPostLinkMD =
      cast_or_null<ConstantAsMetadata>(M.getModuleFlag("LTOPostLink"));
  return LTOPostLinkMD &&
         cast<ConstantInt>(LTOPostLinkMD->getValue())->getZExtValue() != 0;
}

static void collectReferencedGVAndAliasees(
    const Value *V, DenseSet<const GlobalValue *> &ReferencedGVs) {
  const Value *Cur = V->stripPointerCasts();
  const auto *GV = dyn_cast<GlobalValue>(Cur);
  while (GV && ReferencedGVs.insert(GV).second) {
    const auto *GA = dyn_cast<GlobalAlias>(GV);
    if (!GA)
      break;
    Cur = GA->getAliasee()->stripPointerCasts();
    GV = dyn_cast<GlobalValue>(Cur);
  }
}

static DenseSet<const GlobalValue *>
collectCJStaticGenericTIReferencedGVs(const Module &M) {
  DenseSet<const GlobalValue *> ReferencedGVs;
  for (const GlobalVariable &GV : M.globals()) {
    if (!GV.isCJStaticGenericTI() || !GV.hasInitializer())
      continue;

    const Constant *Init = GV.getInitializer();
    for (unsigned I = 0, E = Init->getNumOperands(); I != E; ++I)
      collectReferencedGVAndAliasees(Init->getOperand(I), ReferencedGVs);
  }
  return ReferencedGVs;
}

static bool eliminateAvailableExternally(Module &M) {
  bool Changed = false;
  DenseSet<const GlobalValue *> ProtectedGVs;
  if (isLTOPostLink(M))
    ProtectedGVs = collectCJStaticGenericTIReferencedGVs(M);

  // Drop initializers of available externally global variables.
  for (GlobalVariable &GV : M.globals()) {
    if (!GV.hasAvailableExternallyLinkage())
      continue;
    // Keep globals referenced by CJStaticGenericTI unchanged in LTO post-link.
    if (ProtectedGVs.count(&GV))
      continue;
    if (GV.hasInitializer()) {
      Constant *Init = GV.getInitializer();
      GV.setInitializer(nullptr);
      if (isSafeToDestroyConstant(Init))
        Init->destroyConstant();
    }
    GV.removeDeadConstantUsers();
    GV.setLinkage(GlobalValue::ExternalLinkage);
    NumVariables++;
    Changed = true;
  }

  // Drop the bodies of available externally functions.
  for (Function &F : M) {
    if (!F.hasAvailableExternallyLinkage())
      continue;
    if (!F.isDeclaration())
      // This will set the linkage to external
      F.deleteBody();
    F.removeDeadConstantUsers();
    NumFunctions++;
    Changed = true;
  }

  return Changed;
}

PreservedAnalyses
EliminateAvailableExternallyPass::run(Module &M, ModuleAnalysisManager &) {
  if (!eliminateAvailableExternally(M))
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

namespace {

struct EliminateAvailableExternallyLegacyPass : public ModulePass {
  static char ID; // Pass identification, replacement for typeid

  EliminateAvailableExternallyLegacyPass() : ModulePass(ID) {
    initializeEliminateAvailableExternallyLegacyPassPass(
        *PassRegistry::getPassRegistry());
  }

  // run - Do the EliminateAvailableExternally pass on the specified module,
  // optionally updating the specified callgraph to reflect the changes.
  bool runOnModule(Module &M) override {
    if (skipModule(M))
      return false;
    return eliminateAvailableExternally(M);
  }
};

} // end anonymous namespace

char EliminateAvailableExternallyLegacyPass::ID = 0;

INITIALIZE_PASS(EliminateAvailableExternallyLegacyPass, "elim-avail-extern",
                "Eliminate Available Externally Globals", false, false)

ModulePass *llvm::createEliminateAvailableExternallyPass() {
  return new EliminateAvailableExternallyLegacyPass();
}
