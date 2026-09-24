//===- CJTILocalAlias.cpp - local aliases for preemptible TypeInfo -------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Cangjie CodeGen emits every public type's TypeInfo global ("*.ti") and
// TypeTemplate global ("*.tt") with ExternalLinkage and default visibility,
// because other packages may reference them by name (cross-package generic
// instantiation, reflection). In a shared-library link, lld therefore treats
// these symbols as preemptible, and every pointer to them inside a constant
// initializer (.cjmetadata type-info fields arrays, super-type fields,
// extension records, ...) becomes an ABS64 dynamic relocation.
//
// Most of those references are *intra-module*: a library's own metadata
// pointing at its own ti (measured ~74% of all ABS64 on the ArkUI Cangjie
// wrapper libraries). For those, preemption is pure overhead: the runtime
// never interposes ti symbols, registers types by table scan, and compares
// type identity by UUID, not by pointer.
//
// This pass rewrites such intra-module constant references to go through a
// hidden alias of the same global:
//
//   @pkg:Foo.ti       = global %TypeInfo { ..., %TypeInfo* @pkg:Bar.ti, ... }
//   @pkg:Foo.ti$local = internal alias %TypeInfo, %TypeInfo* @pkg:Foo.ti
//
// A reference to a non-preemptible (internal) alias is resolved by lld into
// R_*_RELATIVE, which the RELR section packs at ~1 bit per entry instead of
// a 24-byte RELA entry.
//
// Correctness argument: an alias denotes the *same address* as its aliasee,
// so every relocated pointer value at runtime is bit-identical to the
// pre-pass behavior. The dynamic symbol table is unchanged: the original
// "*.ti" global keeps ExternalLinkage/default visibility and stays exported
// Only uses through Constants are rewritten. Note this also covers
// instruction operands wrapped in a ConstantExpr (cjc passes ti to runtime
// calls like GCNew as `i8* bitcast (%TypeInfo* @X.ti to i8*)`); those code
// references then bind locally too, letting llc use direct adrp+add instead
// of a GOT load (measured: -687 GOT entries/GLOB_DAT relocs in
// libohos.arkui.component.common.so). Direct GlobalVariable operands of
// instructions are not Constants and keep referencing the exported symbol.
//
// Runs once per linked unit, alongside the other Cangjie size passes:
// non-LTO via PassBuilder's addCangjiePasses (skipped under --cangjie-lto
// pre-opt), full LTO via LTOBackend's addCangjiePasses.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "cj-ti-local-alias"

#include "llvm/Transforms/Scalar/CJTILocalAlias.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/ADT/Triple.h"

using namespace llvm;

STATISTIC(NumAliases, "Number of local ti/tt aliases created");
STATISTIC(NumUsesRewritten, "Number of constant uses redirected to aliases");

// Escape hatch: --cj-ti-local-alias=false disables the pass.
static cl::opt<bool> EnableCJTILocalAlias("cj-ti-local-alias", cl::init(true),
                                          cl::Hidden);
// Also alias weak/LinkOnceODR ti (static generic instances). Note: this drops
// the runtime interposition dedup of identical weak ti across DSOs
// (first-loaded-wins); type identity stays UUID-based, so this is
// semantically safe but turns the runtime's pointer-equality fast paths into
// UUID slow paths.
static cl::opt<bool> EnableWeakTILocalAlias("cj-ti-local-alias-weak",
                                            cl::init(true), cl::Hidden);

// Name suffixes of Cangjie type metadata globals emitted by CodeGen
// (CGType::GetNameOfTypeInfoGV / GetNameOfTypeTemplate).
static bool isTIMetadataName(StringRef name) {
  return name.endswith(".ti") || name.endswith(".tt");
}

// A global is worth aliasing iff references to it would otherwise become
// preemptible dynamic relocations in a -shared link: it must be defined in
// this module, exported with default visibility, and not already dso_local.
// Weak/LinkOnceODR definitions (static generic instances) are included when
// --cj-ti-local-alias-weak is on; an alias keeps the local definition alive,
// which is harmless in a single-package dylib link.
static bool isAliasableTIMetadata(const GlobalVariable &gv) {
  if (gv.isDeclarationForLinker() || !gv.hasInitializer())
    return false;
  const auto linkage = gv.getLinkage();
  const bool linkageOk = linkage == GlobalValue::ExternalLinkage ||
                         (EnableWeakTILocalAlias &&
                          (linkage == GlobalValue::WeakODRLinkage ||
                           linkage == GlobalValue::LinkOnceODRLinkage));
  if (!linkageOk)
    return false;
  if (gv.isDSOLocal())
    return false;
  return isTIMetadataName(gv.getName());
}

PreservedAnalyses CJTILocalAlias::run(Module &M, ModuleAnalysisManager &) const {
  if (!EnableCJTILocalAlias)
    return PreservedAnalyses::all();
  // The ABS64 -> RELATIVE conversion is an ELF/lld mechanism; COFF and Mach-O
  // have different alias and relocation semantics, so restrict to ELF.
  if (!Triple(M.getTargetTriple()).isOSBinFormatELF())
    return PreservedAnalyses::all();

  bool changed = false;
  for (GlobalVariable &gv : M.globals()) {
    if (!isAliasableTIMetadata(gv))
      continue;
    // Skip globals with no constant users: nothing to rewrite.
    if (llvm::none_of(gv.users(),
                      [](const User *u) { return isa<Constant>(u); }))
      continue;

    auto *alias =
        GlobalAlias::create(gv.getValueType(), gv.getAddressSpace(),
                            GlobalValue::InternalLinkage,
                            gv.getName() + "$local", &gv, &M);
    ++NumAliases;

    // Rewrite only uses inside constant initializers. Note that gv.users()
    // sees a nested reference through its outermost ConstantExpr/aggregate;
    // replaceUsesWithIf lets RAUW rebuild those constants (via
    // Constant::handleOperandChange), so nesting depth is not a concern.
    // The alias itself is a Constant user of gv (its aliasee operand) and
    // must be excluded, otherwise it would be repointed to itself.
    unsigned rewritten = 0;
    gv.replaceUsesWithIf(alias, [&](Use &u) {
      auto *c = dyn_cast<Constant>(u.getUser());
      if (!c || c == alias)
        return false;
      ++rewritten;
      return true;
    });
    NumUsesRewritten += rewritten;
    if (rewritten == 0) {
      // All constant uses were the alias itself (cannot happen with the
      // exclusion above) or disappeared; drop the pointless alias.
      alias->eraseFromParent();
      --NumAliases;
      continue;
    }
    changed = true;
  }

  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
