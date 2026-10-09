//===- CJSpecificOpt.cpp ----------------------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This pass mainly implements the specified optimization of cangjie.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJSpecificOpt.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/SafepointIRVerifier.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

#define DEBUG_TYPE "cj-specific-opt"

STATISTIC(NumLocalAliases, "Number of local ti/tt aliases created");
STATISTIC(NumUsesRewritten, "Number of constant uses redirected to aliases");

static cl::opt<bool>
    DisableDeadAllocaElimination("disable-dead-alloca-elimination", cl::Hidden,
                                 cl::init(false));

namespace llvm {
extern cl::opt<bool> CangjieJIT;
extern cl::opt<unsigned> CangjieStackCheckSize;
} // namespace llvm

static bool hasLoopInCangjieFunc(Function &F) {
  DominatorTree DT;
  DT.recalculate(F);
  LoopInfoBase<llvm::BasicBlock, llvm::Loop> loopInfo;
  loopInfo.releaseMemory();
  loopInfo.analyze(DT);
  return !loopInfo.empty();
}

static bool markfastCall(Function &F) {
  // The function cannot be fastcall if it contains a loop.
  if (hasLoopInCangjieFunc(F)) {
    return false;
  }
  for (Instruction &I : instructions(F)) {
    const auto *CI = dyn_cast<CallBase>(&I);
    if (!CI)
      continue;

    Function *Callee = CI->getCalledFunction();
    if (!Callee) // Has indirect call
      return false;

    if (Callee->isCangjieIntrinsic() || Callee->hasFnAttribute("cj-runtime") ||
        Callee->hasFnAttribute("gc-leaf-function")) {
      continue;
    } else {
      return false;
    }
  }
  // mark cj_fast_call attribute
  F.addFnAttr(Attribute::get(F.getContext(), "cj_fast_call"));
  return true;
}

static bool deadAllocaElimination(Function &F) {
  bool Changed = false;
  if (DisableDeadAllocaElimination)
    return Changed;

  SetVector<Instruction *> WorkList;
  for (Instruction &I : F.getEntryBlock()) {
    auto AI = dyn_cast<AllocaInst>(&I);
    if (!AI || AI->getNumUses() > 1)
      continue;

    if (AI->use_empty()) {
      WorkList.insert(AI);
      continue;
    }

    auto *Cast = dyn_cast<BitCastInst>(AI->use_begin()->getUser());
    if (!Cast || Cast->getNumUses() > 1)
      continue;

    if (Cast->use_empty()) {
      WorkList.insert(Cast);
      continue;
    }

    // If the use of alloca is only memset, eliminate it.
    auto *II = dyn_cast<IntrinsicInst>(Cast->use_begin()->getUser());
    if (II && II->getIntrinsicID() == Intrinsic::cj_memset) {
      WorkList.insert(II);
      WorkList.insert(Cast);
      WorkList.insert(AI);
    }
  }

  if (WorkList.size()) {
    for (auto I : WorkList) {
      I->eraseFromParent();
    }
    Changed = true;
  }

  return Changed;
}

static void lowerCJMemset(Function &F, CallInst *CI) {
  FunctionType *FT = CI->getFunctionType();
  Type *ParamTypes[] = {
      FT->getParamType(0), // dst
      FT->getParamType(2)  // len
  };
  Function *NewFn =
      Intrinsic::getDeclaration(F.getParent(), Intrinsic::memset, ParamTypes);
  CI->setCalledFunction(NewFn);
}

static void lowerCJPow(Function &F, CallInst *CI) {
  // get pow funcType
  const auto *FT = CI->getCalledFunction()->getFunctionType();
  Type *ParamTypes[2] = {
      FT->getParamType(0), // base
      FT->getParamType(1)  // exponent
  };

  /**
   * only support 3 types
   * double pow(double, double)
   * float pow(float, float)
   * float pow(float, int32)
   */
  assert(((ParamTypes[0]->isDoubleTy() && ParamTypes[1]->isDoubleTy()) ||
          (ParamTypes[0]->isFloatTy() &&
           (ParamTypes[1]->isFloatTy() ||
            ParamTypes[1]->isIntegerTy(32)))) && // 32BitWidth
         "Unsupported Pow FuncParamType");

  std::string CJPowFuncStr;
  if (ParamTypes[1]->isDoubleTy()) {
    CJPowFuncStr = "CJ_CORE_CPow";
  } else if (ParamTypes[1]->isFloatTy()) {
    CJPowFuncStr = "CJ_CORE_CPowf";
  } else if (ParamTypes[1]->isIntegerTy(32)) { // 32BitWidth
    CJPowFuncStr = "CJ_CORE_FastPowerFloatInt32";
  } else {
    return;
  }

  // insert new CJ pow function
  auto *CJPowFT = FunctionType::get(ParamTypes[0], ParamTypes, false);
  Function *CJPowFunc = cast<Function>(
      F.getParent()->getOrInsertFunction(CJPowFuncStr, CJPowFT).getCallee());
  CJPowFunc->addFnAttr(Attribute::get(F.getContext(), "gc-leaf-function"));
  CI->setCalledFunction(CJPowFunc);
}

static void replaceAddrpaceCastUsed(AddrSpaceCastInst *ACI, IntrinsicInst *II) {
  Value *Ptr = ACI->getPointerOperand();
  IRBuilder<> Builder(II);
  switch (II->getIntrinsicID()) {
  default:
    assert(false && "Unsupported intrinsic type");
    break;
  case Intrinsic::memcpy: {
    MemCpyInst *MemCpy = cast<MemCpyInst>(II);
    if (ACI == MemCpy->getRawSource()) {
      Builder.CreateMemCpy(MemCpy->getRawDest(), MemCpy->getDestAlign(), Ptr,
                           MemCpy->getSourceAlign(), MemCpy->getLength(),
                           MemCpy->isVolatile());
    } else {
      Builder.CreateMemCpy(Ptr, MemCpy->getDestAlign(), MemCpy->getRawSource(),
                           MemCpy->getSourceAlign(), MemCpy->getLength(),
                           MemCpy->isVolatile());
    }
    break;
  }
  case Intrinsic::memmove: {
    MemMoveInst *MemMove = cast<MemMoveInst>(II);
    if (ACI == MemMove->getRawSource()) {
      Builder.CreateMemMove(MemMove->getRawDest(), MemMove->getDestAlign(), Ptr,
                            MemMove->getSourceAlign(), MemMove->getLength(),
                            MemMove->isVolatile());
    } else {
      Builder.CreateMemMove(Ptr, MemMove->getDestAlign(),
                            MemMove->getRawSource(), MemMove->getSourceAlign(),
                            MemMove->getLength(), MemMove->isVolatile());
    }
    break;
  }
  case Intrinsic::memset: {
    MemSetInst *MemSet = cast<MemSetInst>(II);
    Builder.CreateMemSet(Ptr, MemSet->getValue(), MemSet->getLength(),
                         MaybeAlign(MemSet->getDestAlignment()),
                         MemSet->isVolatile());
    break;
  }
  }

  II->eraseFromParent();
  return;
}

// IR after inline:
// \code
// %x = bitcast %record* %r to i8*
// %y = addrspacecast i8* %x to i8 addrspace(1)*
// call @memcpy.p0i8.p0i8.i64(i8* %x, i8* %src, i64 16, i1 false)
// call @memcpy.p0i8.p1i8.i64(i8* %z, i8 addrspace(1)* %y, i64 16, i1 false)
// \
// transforms into:
// \code
// %x = bitcast %record* %r to i8*
// %y = addrspacecast i8* %x to i8 addrspace(1)*
// call @memcpy.p0i8.p0i8.i64(i8* %x, i8* %src, i64 16, i1 false)
// call @memcpy.p0i8.p0i8.i64(i8* %z, i8* %x, i64 16, i1 false)
// \
// Remove the `addrspacecast inst`, if addrspacecast is not used elsewhere
// which is helpful for computeLiveInValues.
// The addrspacecast may also be used for memset and memmove.
static bool replaceInvalidAddrpaceCast(Function &F) {
  bool Changed = false;
  SmallVector<Instruction *, 8> WorkList;
  for (auto &I : instructions(F)) {
    if (I.getMetadata(LLVMContext::MD_tbaa)) {
      I.setMetadata(LLVMContext::MD_tbaa, nullptr);
    }
    if (I.getMetadata(LLVMContext::MD_tbaa_struct)) {
      I.setMetadata(LLVMContext::MD_tbaa_struct, nullptr);
    }
    auto *ACI = dyn_cast<AddrSpaceCastInst>(&I);
    if (!ACI) {
      continue;
    }
    auto *PT = dyn_cast<PointerType>(ACI->getPointerOperand()->getType());
    if (PT->getAddressSpace() != 0 || !PT->getElementType()->isIntegerTy(8)) {
      continue;
    }
    WorkList.push_back(ACI);
  }

  for (Instruction *I : WorkList) {
    auto *ACI = cast<AddrSpaceCastInst>(I);
    for (auto UI = ACI->use_begin(), UE = ACI->use_end(); UI != UE;) {
      auto *II = dyn_cast<IntrinsicInst>((UI++)->getUser());
      if (!II)
        continue;

      // Replace memory inst operand.
      if (II->getIntrinsicID() == Intrinsic::memcpy ||
          II->getIntrinsicID() == Intrinsic::memset ||
          II->getIntrinsicID() == Intrinsic::memmove) {
        replaceAddrpaceCastUsed(ACI, II);
        Changed = true;
      }
    }

    if (ACI->user_empty()) {
      ACI->eraseFromParent();
      Changed = true;
    }
  }

  return Changed;
}

static bool insertStackCheck(Function &F) {
  if (CangjieJIT || F.hasFnAttribute("gc-leaf-function"))
    return false;

  uint64_t ApproximateSize = 0;
  bool HasCJCall = false;
  const DataLayout &DL = F.getParent()->getDataLayout();
  for (auto &I : F.getEntryBlock()) {
    if (auto *AI = dyn_cast<AllocaInst>(&I)) {
      // The stack ptr type must be 8-byte aligned.
      if (AI->getAlign().value() % 8) {
        AI->setAlignment(Align(8));
      }
      // 8-byte aligned size.
      ApproximateSize +=
          (DL.getTypeSizeInBits(AI->getAllocatedType()) / 8 + 7) & ~7;
    }
  }
  for (auto &I : instructions(F)) {
    auto *CB = dyn_cast<CallBase>(&I);
    if (!CB || isa<IntrinsicInst>(CB))
      continue;
    if (!CB->hasFnAttr("cj-runtime")) {
      HasCJCall = true;
      break;
    }
  }
  if (!HasCJCall && ApproximateSize < CangjieStackCheckSize)
    return false;

  Instruction *InsertBefore = &F.getEntryBlock().front();
  const char *StackCheckStr = "CJ_MCC_StackCheck";
  Module *M = InsertBefore->getModule();
  Function *Func = M->getFunction(StackCheckStr);
  if (Func == nullptr) {
    FunctionType *FuncType =
        FunctionType::get(Type::getVoidTy(M->getContext()), false);
    Func = cast<Function>(
        M->getOrInsertFunction(StackCheckStr, FuncType).getCallee());
    Func->addFnAttr("cj-stack-check");
    Func->setUnnamedAddr(GlobalValue::UnnamedAddr::Local);
  }
  auto *CI = CallInst::Create(Func->getFunctionType(), Func, "", InsertBefore);
  CI->setCallingConv(CallingConv::CangjieGC);
  return true;
}

static bool insertResetFPState(Function &F, unsigned OptLevel) {
  if (!Triple(F.getParent()->getTargetTriple()).isAArch64() ||
      OptLevel < 2) { // 2: O2
    return false;
  }
  bool Changed = false;

  for (BasicBlock &BB : F) {
    if (BB.getName().startswith("fp.convert.exception")) {
      IRBuilder<> Builder(BB.getFirstNonPHI());
      // insert call void @llvm.cj.reset.fp.state()
      Function *ResetFpStateFunc = Intrinsic::getDeclaration(
          F.getParent(), Intrinsic::cj_reset_fp_state);
      Builder.CreateCall(ResetFpStateFunc, {});

      Changed = true;
    }
  }
  return Changed;
}

static bool runOnFunction(Function &F, unsigned OptLevel) {
  bool Changed = false;
  if (F.isDeclaration() || F.empty() || !F.hasCangjieGC() || F.hasOptNone()) {
    return Changed;
  }

  for (auto I = inst_begin(F); I != inst_end(F);) {
    IntrinsicInst *CI = dyn_cast<IntrinsicInst>(&*I++);
    if (!CI || CI->getCalledFunction() == nullptr)
      continue;

    switch (CI->getIntrinsicID()) {
    default:
      break;
    case Intrinsic::cj_memset:
      lowerCJMemset(F, CI);
      Changed = true;
      break;
    case Intrinsic::pow:
    case Intrinsic::powi:
      lowerCJPow(F, CI);
      Changed = true;
      break;
    }
  }
  if (CangjieJIT)
    return Changed;

  Changed |= markfastCall(F);
  Changed |= deadAllocaElimination(F);
  Changed |= replaceInvalidAddrpaceCast(F);
  const Triple TT(F.getParent()->getTargetTriple());
  Changed |= insertStackCheck(F);
  Changed |= insertResetFPState(F, OptLevel);
  return Changed;
}

// Cangjie CodeGen emits every public type's TypeInfo global ("*.ti") and
// TypeTemplate global ("*.tt") with ExternalLinkage and default visibility,
// because other packages may reference them by name (cross-package generic
// instantiation, reflection). In a shared-library link, lld therefore treats
// these symbols as preemptible, and every pointer to them inside a constant
// initializer (.cjmetadata type-info fields arrays, super-type fields,
// extension records, ...) becomes an ABS64 dynamic relocation; code taking
// their address (cjc passes ti to runtime calls like GCNew as a bitcast
// ConstantExpr) goes through the GOT.
//
// Most of those references are *intra-module*: a library's own metadata
// pointing at its own ti. For those, preemption is pure overhead: the
// runtime never interposes ti symbols and compares type identity by UUID,
// not by pointer. This function rewrites such intra-module constant
// references to go through an internal alias of the same global:
//
//   @pkg:Foo.ti     = global %TypeInfo { ..., %TypeInfo* @pkg:Bar.ti, ... }
//   @alias_pkg:Foo.ti = internal alias %TypeInfo, %TypeInfo* @pkg:Foo.ti
//
// A reference to a non-preemptible (internal) alias is resolved by lld into
// R_*_RELATIVE (RELR-packable) instead of a 24-byte ABS64/GLOB_DAT entry,
// and code references bind locally (direct adrp+add / lea instead of a GOT
// load). The original "*.ti" global keeps ExternalLinkage and stays
// exported; only intra-module references stop participating in preemption.
//
// Escape hatch: on ELF, --cj-local-alias=false falls back to the legacy
// generateAliasForStaticGI path (StaticGenericTI array operands only),
// i.e. exactly the behavior before this feature.
static cl::opt<bool>
    EnableCJLocalAlias("cj-local-alias", cl::init(true), cl::Hidden,
                       cl::desc("Rewrite intra-module ti/tt references via "
                                "local aliases. When disabled on ELF, fall "
                                "back to the legacy StaticGenericTI-only "
                                "aliasing."));

// Name suffixes of Cangjie type metadata globals emitted by CodeGen
// (CGType::GetNameOfTypeInfoGV / GetNameOfTypeTemplate).
static bool isTIMetadataName(StringRef name) {
  return name.endswith(".ti") || name.endswith(".tt");
}

// A global is worth aliasing iff references to it would otherwise become
// preemptible dynamic relocations in a -shared link: it must be defined in
// this module, exported with default visibility, and not already dso_local.
// Weak/LinkOnceODR definitions (static generic instances) are included: an
// alias keeps the local definition alive, which is harmless in a
// single-package dylib link. This drops the runtime interposition dedup of
// identical weak ti across DSOs (first-loaded-wins); type identity stays
// UUID-based, so this is semantically safe.
static bool isAliasableTIMetadata(const GlobalVariable &gv) {
  if (gv.isDeclarationForLinker() || !gv.hasInitializer())
    return false;
  if (gv.isDSOLocal())
    return false;
  // Hidden/protected symbols are already non-preemptible; aliasing them
  // would be harmless but pointless.
  if (!gv.hasDefaultVisibility())
    return false;
  const auto linkage = gv.getLinkage();
  const bool linkageOk = linkage == GlobalValue::ExternalLinkage ||
                         linkage == GlobalValue::WeakODRLinkage ||
                         linkage == GlobalValue::LinkOnceODRLinkage;
  if (!linkageOk)
    return false;
  return isTIMetadataName(gv.getName());
}

// Historical implementation retained for non-ELF (Mach-O) platforms, where
// the CJLocalAlias-style rewriting is neither needed nor applicable (see the
// ELF-only note below). Consolidating both paths onto
// generateLocalAliasForTIMetadata is left to a follow-up change.
static bool generateAliasForStaticGI(Module &M) {
  bool Changed = false;
  for (GlobalVariable &GV : M.globals()) {

    if (GV.isCJStaticGenericTI()) {
      assert(GV.hasInitializer() && "StaticGenericTI has no Initializer!");

      Constant *C = GV.getInitializer();

      SmallVector<Constant *, 16> NewOperands;
      for (unsigned Idx = 0; Idx < C->getNumOperands(); Idx++) {
        GlobalVariable *TI = cast<GlobalVariable>(C->getOperand(Idx));

        // Skip ExternalLinkage symbols to preserve the original declaration in
        // ThinLTO where non-prevailing linkonce_odr symbols are downgraded by
        // the 'ElimAvailExtern' pass.
        if (TI->hasExternalLinkage()) {
          NewOperands.push_back(TI);
          continue;
        }

        // For FullLTO static-library symbols, convert
        // AvailableExternallyLinkage to InternalLinkage so alias generation
        // can proceed.
        if (TI->hasAvailableExternallyLinkage()) {
          TI->setLinkage(GlobalValue::InternalLinkage);
        }

        std::string AliasName = "alias_" + TI->getName().str();
        auto *Alias = GlobalAlias::create(
            TI->getValueType(), TI->getType()->getPointerAddressSpace(),
            GlobalValue::PrivateLinkage, AliasName, TI, &M);

        NewOperands.push_back(Alias);
        Changed = true;
      }

      ArrayType *ArrayTy = cast<ArrayType>(C->getType());
      Constant *NewInit = ConstantArray::get(ArrayTy, NewOperands);
      GV.setInitializer(NewInit);
    }
  }
  return Changed;
}

static bool generateLocalAliasForTIMetadata(Module &M) {
  bool changed = false;
  for (GlobalVariable &gv : M.globals()) {
    if (!isTIMetadataName(gv.getName()))
      continue;
    // FullLTO: a ti whose prevailing definition lives in a static library is
    // marked available_externally here. ElimAvailExtern keeps the body (it
    // is referenced by the StaticGenericTI array), but the symbol would not
    // be emitted; upgrade to internal so the local definition is kept and
    // binds locally. No alias is needed once the symbol itself is internal.
    // When this function is gated off (--cj-local-alias=false), the legacy
    // generateAliasForStaticGI path performs the same upgrade for the
    // StaticGenericTI array operands.
    if (gv.hasAvailableExternallyLinkage() && gv.hasInitializer()) {
      gv.setLinkage(GlobalValue::InternalLinkage);
      changed = true;
      continue;
    }
    if (!isAliasableTIMetadata(gv))
      continue;
    // Every remaining user is rewritten below (the alias is bit-identical
    // to its aliasee), except GlobalAlias users: the new alias would
    // otherwise be repointed to itself, and an old alias from a previous
    // run would form an alias chain. If only aliases are left (e.g. a
    // second run of this pass), there is nothing to do.
    if (llvm::none_of(gv.users(),
                      [](const User *u) { return !isa<GlobalAlias>(u); }))
      continue;

    auto *alias = GlobalAlias::create(gv.getValueType(), gv.getAddressSpace(),
                                      GlobalValue::InternalLinkage,
                                      "alias_" + gv.getName(), &gv, &M);
    ++NumLocalAliases;

    // Note that gv.users() sees a nested constant reference through its
    // outermost ConstantExpr/aggregate; replaceUsesWithIf lets RAUW rebuild
    // those constants (via Constant::handleOperandChange), so nesting depth
    // is not a concern.
    unsigned rewritten = 0;
    gv.replaceUsesWithIf(alias, [&rewritten](Use &u) {
      if (isa<GlobalAlias>(u.getUser()))
        return false;
      ++rewritten;
      return true;
    });
    NumUsesRewritten += rewritten;
    changed = true;
  }
  return changed;
}

static bool processCangjieIR(Module &M, unsigned OptLevel) {
  bool Changed = false;

  // Lower @llvm.cj.vfe.info intrinsic calls to their first argument (identity).
  // This intrinsic is used to carry VFE metadata through optimization passes
  // and must be removed before codegen.
  if (Function *VFEInfoFn = M.getFunction("llvm.cj.vfe.info")) {
    SmallVector<CallInst *, 16> ToRemove;
    for (User *U : VFEInfoFn->users()) {
      if (auto *CI = dyn_cast<CallInst>(U))
        ToRemove.push_back(CI);
    }
    for (auto *CI : ToRemove) {
      CI->replaceAllUsesWith(CI->getArgOperand(0));
      CI->eraseFromParent();
    }
    if (!ToRemove.empty())
      Changed = true;
  }

  llvm::Triple T(M.getTargetTriple());
  if (T.isOSBinFormatELF() && EnableCJLocalAlias) {
    Changed |= generateLocalAliasForTIMetadata(M);
  } else if (!T.isOSWindows()) {
    // Legacy path: on non-ELF platforms this is the only implementation;
    // on ELF it serves as the --cj-local-alias=false escape hatch (exactly
    // the pre-feature behavior).
    Changed |= generateAliasForStaticGI(M);
  }

  for (Function &F : M) {
    Changed |= runOnFunction(F, OptLevel);
  }

  // llvm.used is not needed after opt, clear it. Otherwise it would cause
  // an error when the linker tried to preserve the symbol due to
  // the `/include:` directive.
  GlobalVariable *UsedGV = M.getNamedGlobal("llvm.used");
  if (UsedGV) {
    UsedGV->eraseFromParent();
    Changed = true;
  }
  return Changed;
}

PreservedAnalyses CJSpecificOpt::run(Module &M, ModuleAnalysisManager &) const {
  if (processCangjieIR(M, OptLevel)) {
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

namespace {
class CangjieSpecificOptLegacyPass : public ModulePass {
public:
  static char ID;
  unsigned OptLevel;

  explicit CangjieSpecificOptLegacyPass(unsigned OptLevel = 0)
      : ModulePass(ID), OptLevel(OptLevel) {
    initializeCangjieSpecificOptLegacyPassPass(
        *PassRegistry::getPassRegistry());
  }
  ~CangjieSpecificOptLegacyPass() = default;

  bool runOnModule(Module &M) override { return processCangjieIR(M, OptLevel); }
};
} // namespace

char CangjieSpecificOptLegacyPass::ID = 0;

ModulePass *llvm::createCangjieSpecificOptLegacyPass(unsigned OptLevel) {
  return new CangjieSpecificOptLegacyPass(OptLevel);
}

INITIALIZE_PASS(CangjieSpecificOptLegacyPass, "cj-specific-opt",
                "Cangjie Specific Optimize", false, false)
