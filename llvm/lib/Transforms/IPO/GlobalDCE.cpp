//===-- GlobalDCE.cpp - DCE unreachable internal functions ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This transform is designed to eliminate unreachable internal globals from the
// program.  It uses an aggressive algorithm, searching out globals that are
// known to be alive.  After it finds all of the globals which are needed, it
// deletes whatever is left over.  This allows it to delete recursive chunks of
// the program which are unreachable.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/GlobalDCE.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/TypeMetadataUtils.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Utils/CtorUtils.h"
#include "llvm/Transforms/Utils/GlobalStatus.h"

using namespace llvm;

#define DEBUG_TYPE "globaldce"

namespace llvm {
extern cl::opt<bool> CJPipeline;
} // namespace llvm

static cl::opt<bool>
    ClEnableVFE("enable-vfe", cl::Hidden, cl::init(true),
                cl::desc("Enable virtual function elimination"));

static cl::opt<bool>
    ClEnableCJVFE("enable-cangjie-vfe", cl::Hidden, cl::init(true),
                cl::desc("Enable cangjie virtual function elimination"));

static cl::opt<bool>
    ClEnableCJTIE("enable-cangjie-typeinfo-elimination", cl::Hidden,
                  cl::init(true),
                  cl::desc("Enable cangjie typeinfo elimination"));

STATISTIC(NumAliases  , "Number of global aliases removed");
STATISTIC(NumFunctions, "Number of functions removed");
STATISTIC(NumIFuncs,    "Number of indirect functions removed");
STATISTIC(NumVariables, "Number of global variables removed");
STATISTIC(NumVFuncs,    "Number of virtual functions removed");

namespace {
  class GlobalDCELegacyPass : public ModulePass {
  public:
    static char ID; // Pass identification, replacement for typeid
    GlobalDCELegacyPass() : ModulePass(ID) {
      initializeGlobalDCELegacyPassPass(*PassRegistry::getPassRegistry());
    }

    // run - Do the GlobalDCE pass on the specified module, optionally updating
    // the specified callgraph to reflect the changes.
    //
    bool runOnModule(Module &M) override {
      if (skipModule(M))
        return false;

      // We need a minimally functional dummy module analysis manager. It needs
      // to at least know about the possibility of proxying a function analysis
      // manager.
      FunctionAnalysisManager DummyFAM;
      ModuleAnalysisManager DummyMAM;
      DummyMAM.registerPass(
          [&] { return FunctionAnalysisManagerModuleProxy(DummyFAM); });

      auto PA = Impl.run(M, DummyMAM);
      return !PA.areAllPreserved();
    }

  private:
    GlobalDCEPass Impl;
  };
}

char GlobalDCELegacyPass::ID = 0;
INITIALIZE_PASS(GlobalDCELegacyPass, "globaldce",
                "Dead Global Elimination", false, false)

// Public interface to the GlobalDCEPass.
ModulePass *llvm::createGlobalDCEPass() {
  return new GlobalDCELegacyPass();
}

/// Returns true if F is effectively empty.
static bool isEmptyFunction(Function *F) {
  // Skip external functions.
  if (F->isDeclaration())
    return false;
  BasicBlock &Entry = F->getEntryBlock();
  for (auto &I : Entry) {
    if (I.isDebugOrPseudoInst())
      continue;
    if (auto *RI = dyn_cast<ReturnInst>(&I))
      return !RI->getReturnValue();
    break;
  }
  return false;
}

/// Compute the set of GlobalValue that depends from V.
/// The recursion stops as soon as a GlobalValue is met.
void GlobalDCEPass::ComputeDependencies(Value *V,
                                        SmallPtrSetImpl<GlobalValue *> &Deps) {
  if (auto *I = dyn_cast<Instruction>(V)) {
    Function *Parent = I->getParent()->getParent();
    Deps.insert(Parent);
  } else if (auto *GV = dyn_cast<GlobalValue>(V)) {
    Deps.insert(GV);
  } else if (auto *CE = dyn_cast<Constant>(V)) {
    // Avoid walking the whole tree of a big ConstantExprs multiple times.
    auto Where = ConstantDependenciesCache.find(CE);
    if (Where != ConstantDependenciesCache.end()) {
      auto const &K = Where->second;
      Deps.insert(K.begin(), K.end());
    } else {
      SmallPtrSetImpl<GlobalValue *> &LocalDeps = ConstantDependenciesCache[CE];
      for (User *CEUser : CE->users())
        ComputeDependencies(CEUser, LocalDeps);
      Deps.insert(LocalDeps.begin(), LocalDeps.end());
    }
  }
}

void GlobalDCEPass::UpdateGVDependencies(GlobalValue &GV) {
  SmallPtrSet<GlobalValue *, 8> Deps;
  for (User *User : GV.users())
    ComputeDependencies(User, Deps);
  Deps.erase(&GV); // Remove self-reference.
  for (GlobalValue *GVU : Deps) {
    // If this is a dep from a vtable to a virtual function, and we have
    // complete information about all virtual call sites which could call
    // though this vtable, then skip it, because the call site information will
    // be more precise.
    if (VFESafeVTables.count(GVU) && isa<Function>(&GV)) {
      LLVM_DEBUG(dbgs() << "Ignoring dep " << GVU->getName() << " -> "
                        << GV.getName() << "\n");
      continue;
    }
    if (CJPipeline &&
        CangjieDCE::maybeFakeLiveOfTypeMeta(dyn_cast<GlobalVariable>(GVU),
                                            dyn_cast<GlobalVariable>(&GV)))
      continue;
    GVDependencies[GVU].insert(&GV);
  }
}

/// Mark Global value as Live
void GlobalDCEPass::MarkLive(GlobalValue &GV,
                             SmallVectorImpl<GlobalValue *> *Updates) {
  auto const Ret = AliveGlobals.insert(&GV);
  if (!Ret.second)
    return;

  if (Updates)
    Updates->push_back(&GV);
  if (Comdat *C = GV.getComdat()) {
    for (auto &&CM : make_range(ComdatMembers.equal_range(C))) {
      MarkLive(*CM.second, Updates); // Recursion depth is only two because only
                                     // globals in the same comdat are visited.
    }
  }
}

void GlobalDCEPass::ScanVTables(Module &M) {
  SmallVector<MDNode *, 2> Types;
  LLVM_DEBUG(dbgs() << "Building type info -> vtable map\n");

  auto *LTOPostLinkMD =
      cast_or_null<ConstantAsMetadata>(M.getModuleFlag("LTOPostLink"));
  bool LTOPostLink =
      LTOPostLinkMD &&
      (cast<ConstantInt>(LTOPostLinkMD->getValue())->getZExtValue() != 0);

  for (GlobalVariable &GV : M.globals()) {
    Types.clear();
    GV.getMetadata(LLVMContext::MD_type, Types);
    if (GV.isDeclaration() || Types.empty())
      continue;

    // Use the typeid metadata on the vtable to build a mapping from typeids to
    // the list of (GV, offset) pairs which are the possible vtables for that
    // typeid.
    for (MDNode *Type : Types) {
      Metadata *TypeID = Type->getOperand(1).get();

      uint64_t Offset =
          cast<ConstantInt>(
              cast<ConstantAsMetadata>(Type->getOperand(0))->getValue())
              ->getZExtValue();

      TypeIdMap[TypeID].insert(std::make_pair(&GV, Offset));
    }

    // If the type corresponding to the vtable is private to this translation
    // unit, we know that we can see all virtual functions which might use it,
    // so VFE is safe.
    if (auto GO = dyn_cast<GlobalObject>(&GV)) {
      GlobalObject::VCallVisibility TypeVis = GO->getVCallVisibility();
      if (TypeVis == GlobalObject::VCallVisibilityTranslationUnit ||
          (LTOPostLink &&
           TypeVis == GlobalObject::VCallVisibilityLinkageUnit)) {
        LLVM_DEBUG(dbgs() << GV.getName() << " is safe for VFE\n");
        VFESafeVTables.insert(&GV);
      }
    }
  }
}

void GlobalDCEPass::ScanVTableLoad(Function *Caller, Metadata *TypeId,
                                   uint64_t CallOffset) {
  for (auto &VTableInfo : TypeIdMap[TypeId]) {
    GlobalVariable *VTable = VTableInfo.first;
    uint64_t VTableOffset = VTableInfo.second;

    Constant *Ptr =
        getPointerAtOffset(VTable->getInitializer(), VTableOffset + CallOffset,
                           *Caller->getParent(), VTable);
    if (!Ptr) {
      LLVM_DEBUG(dbgs() << "can't find pointer in vtable!\n");
      VFESafeVTables.erase(VTable);
      continue;
    }

    auto Callee = dyn_cast<Function>(Ptr->stripPointerCasts());
    if (!Callee) {
      LLVM_DEBUG(dbgs() << "vtable entry is not function pointer!\n");
      VFESafeVTables.erase(VTable);
      continue;
    }

    LLVM_DEBUG(dbgs() << "vfunc dep " << Caller->getName() << " -> "
                      << Callee->getName() << "\n");
    GVDependencies[Caller].insert(Callee);
  }
}

void GlobalDCEPass::ScanTypeCheckedLoadIntrinsics(Module &M) {
  LLVM_DEBUG(dbgs() << "Scanning type.checked.load intrinsics\n");
  Function *TypeCheckedLoadFunc =
      M.getFunction(Intrinsic::getName(Intrinsic::type_checked_load));

  if (!TypeCheckedLoadFunc)
    return;

  for (auto U : TypeCheckedLoadFunc->users()) {
    auto CI = dyn_cast<CallInst>(U);
    if (!CI)
      continue;

    auto *Offset = dyn_cast<ConstantInt>(CI->getArgOperand(1));
    Value *TypeIdValue = CI->getArgOperand(2);
    auto *TypeId = cast<MetadataAsValue>(TypeIdValue)->getMetadata();

    if (Offset) {
      ScanVTableLoad(CI->getFunction(), TypeId, Offset->getZExtValue());
    } else {
      // type.checked.load with a non-constant offset, so assume every entry in
      // every matching vtable is used.
      for (auto &VTableInfo : TypeIdMap[TypeId]) {
        VFESafeVTables.erase(VTableInfo.first);
      }
    }
  }
}

void GlobalDCEPass::AddVirtualFunctionDependencies(Module &M) {
  if (!ClEnableVFE)
    return;

  // If the Virtual Function Elim module flag is present and set to zero, then
  // the vcall_visibility metadata was inserted for another optimization (WPD)
  // and we may not have type checked loads on all accesses to the vtable.
  // Don't attempt VFE in that case.
  auto *Val = mdconst::dyn_extract_or_null<ConstantInt>(
      M.getModuleFlag("Virtual Function Elim"));
  if (!Val || Val->getZExtValue() == 0)
    return;

  ScanVTables(M);

  if (VFESafeVTables.empty())
    return;

  ScanTypeCheckedLoadIntrinsics(M);

  LLVM_DEBUG(
    dbgs() << "VFE safe vtables:\n";
    for (auto *VTable : VFESafeVTables)
      dbgs() << "  " << VTable->getName() << "\n";
  );
}

PreservedAnalyses GlobalDCEPass::run(Module &M, ModuleAnalysisManager &MAM) {
  bool Changed = false;
  CangjieDCE CJDCE(*this);

  // The algorithm first computes the set L of global variables that are
  // trivially live.  Then it walks the initialization of these variables to
  // compute the globals used to initialize them, which effectively builds a
  // directed graph where nodes are global variables, and an edge from A to B
  // means B is used to initialize A.  Finally, it propagates the liveness
  // information through the graph starting from the nodes in L. Nodes note
  // marked as alive are discarded.

  // Remove empty functions from the global ctors list.
  Changed |= optimizeGlobalCtorsList(
      M, [](uint32_t, Function *F) { return isEmptyFunction(F); });

  // Collect the set of members for each comdat.
  for (Function &F : M)
    if (Comdat *C = F.getComdat())
      ComdatMembers.insert(std::make_pair(C, &F));
  for (GlobalVariable &GV : M.globals())
    if (Comdat *C = GV.getComdat())
      ComdatMembers.insert(std::make_pair(C, &GV));
  for (GlobalAlias &GA : M.aliases())
    if (Comdat *C = GA.getComdat())
      ComdatMembers.insert(std::make_pair(C, &GA));

  // Add dependencies between virtual call sites and the virtual functions they
  // might call, if we have that information.
  AddVirtualFunctionDependencies(M);

  if (CJPipeline && ClEnableCJVFE)
    CJDCE.addCangjieVirtualFunctionDependencies(M);
  if (CJPipeline && ClEnableCJTIE)
    CJDCE.initTIE(M);

  // Loop over the module, adding globals which are obviously necessary.
  for (GlobalObject &GO : M.global_objects()) {
    GO.removeDeadConstantUsers();
    // Functions with external linkage are needed if they have a body.
    // Externally visible & appending globals are needed, if they have an
    // initializer.
    if (!GO.isDeclaration())
      if (!GO.isDiscardableIfUnused())
        MarkLive(GO);

    UpdateGVDependencies(GO);
  }

  // Compute direct dependencies of aliases.
  for (GlobalAlias &GA : M.aliases()) {
    GA.removeDeadConstantUsers();
    // Externally visible aliases are needed.
    if (!GA.isDiscardableIfUnused())
      MarkLive(GA);

    UpdateGVDependencies(GA);
  }

  // Compute direct dependencies of ifuncs.
  for (GlobalIFunc &GIF : M.ifuncs()) {
    GIF.removeDeadConstantUsers();
    // Externally visible ifuncs are needed.
    if (!GIF.isDiscardableIfUnused())
      MarkLive(GIF);

    UpdateGVDependencies(GIF);
  }

  // Propagate liveness from collected Global Values through the computed
  // dependencies.
  SmallVector<GlobalValue *, 8> NewLiveGVs{AliveGlobals.begin(),
                                           AliveGlobals.end()};
  while (!NewLiveGVs.empty()) {
    GlobalValue *LGV = NewLiveGVs.pop_back_val();
    for (auto *GVD : GVDependencies[LGV])
      MarkLive(*GVD, &NewLiveGVs);
  }

  if (CJPipeline && ClEnableCJTIE)
    CJDCE.updateLiveExtensions();

  // Now that all globals which are needed are in the AliveGlobals set, we loop
  // through the program, deleting those which are not alive.
  //

  // The first pass is to drop initializers of global variables which are dead.
  std::vector<GlobalVariable *> DeadGlobalVars; // Keep track of dead globals
  for (GlobalVariable &GV : M.globals()) {
    // Cangjie native GV needs to be processed at the backend..
    if (GV.hasAttribute("cj-native")) {
      continue;
    }

    if (!AliveGlobals.count(&GV)) {
      DeadGlobalVars.push_back(&GV); // Keep track of dead globals
      if (GV.hasInitializer()) {
        Constant *Init = GV.getInitializer();
        GV.setInitializer(nullptr);
        if (isSafeToDestroyConstant(Init))
          Init->destroyConstant();
      }
    }
  }
  // The second pass drops the bodies of functions which are dead...
  std::vector<Function *> DeadFunctions;
  for (Function &F : M) {
    // Cangjie runtime function needs to be processed at the backend..
    if (F.hasFnAttribute("cj-runtime"))
      continue;

    if (!AliveGlobals.count(&F)) {
      DeadFunctions.push_back(&F); // Keep track of dead globals
      if (!F.isDeclaration())
        F.deleteBody();
    }
  }
  // The third pass drops targets of aliases which are dead...
  std::vector<GlobalAlias*> DeadAliases;
  for (GlobalAlias &GA : M.aliases())
    if (!AliveGlobals.count(&GA)) {
      DeadAliases.push_back(&GA);
      GA.setAliasee(nullptr);
    }

  // The fourth pass drops targets of ifuncs which are dead...
  std::vector<GlobalIFunc*> DeadIFuncs;
  for (GlobalIFunc &GIF : M.ifuncs())
    if (!AliveGlobals.count(&GIF)) {
      DeadIFuncs.push_back(&GIF);
      GIF.setResolver(nullptr);
    }

  // Now that all interferences have been dropped, delete the actual objects
  // themselves.
  auto EraseUnusedGlobalValue = [&](GlobalValue *GV) {
    GV->removeDeadConstantUsers();
    GV->eraseFromParent();
    Changed = true;
  };

  NumFunctions += DeadFunctions.size();
  for (Function *F : DeadFunctions) {
    if (!F->use_empty()) {
      // Virtual functions might still be referenced by one or more vtables,
      // but if we've proven them to be unused then it's safe to replace the
      // virtual function pointers with null, allowing us to remove the
      // function itself.
      ++NumVFuncs;

      // Detect vfuncs that are referenced as "relative pointers" which are used
      // in Swift vtables, i.e. entries in the form of:
      //
      //   i32 trunc (i64 sub (i64 ptrtoint @f, i64 ptrtoint ...)) to i32)
      //
      // In this case, replace the whole "sub" expression with constant 0 to
      // avoid leaving a weird sub(0, symbol) expression behind.
      replaceRelativePointerUsersWithZero(F);

      F->replaceNonMetadataUsesWith(ConstantPointerNull::get(F->getType()));
    }
    EraseUnusedGlobalValue(F);
  }

  NumVariables += DeadGlobalVars.size();
  for (GlobalVariable *GV : DeadGlobalVars) {
    if (CangjieDCE::isCangjieType(GV) ||
        CangjieDCE::isMetaAssociatedWithType(GV))
      GV->replaceNonMetadataUsesWith(ConstantPointerNull::get(GV->getType()));
    EraseUnusedGlobalValue(GV);
  }

  NumAliases += DeadAliases.size();
  for (GlobalAlias *GA : DeadAliases)
    EraseUnusedGlobalValue(GA);

  NumIFuncs += DeadIFuncs.size();
  for (GlobalIFunc *GIF : DeadIFuncs)
    EraseUnusedGlobalValue(GIF);

  if (CJPipeline && ClEnableCJTIE) {
    CJDCE.rewriteExtensions(M);
    CJDCE.rewriteLLVMUsed(M);
  }

  // Make sure that all memory is released
  AliveGlobals.clear();
  ConstantDependenciesCache.clear();
  GVDependencies.clear();
  ComdatMembers.clear();
  TypeIdMap.clear();
  VFESafeVTables.clear();

  if (Changed)
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

CangjieDCE::GenericFuncInfo *
CangjieDCE::insertInstance(const SmallVectorImpl<GlobalVariable *> &Path,
                           GlobalVariable *FT = nullptr) {
  GenericFuncInfo *CurRoot = this->Root;
  for (GlobalVariable *GV : Path) {
    auto It = CurRoot->Next.find(GV);
    if (It != CurRoot->Next.end()) {
      CurRoot = It->second;
      continue;
    }
    auto *GFI = new GenericFuncInfo;
#ifndef NDEBUG
    GFI->Ty = GV;
#endif
    GFI->Prev = CurRoot;
    CurRoot->Next[GV] = GFI;
    CurRoot = GFI;
  }
  CurRoot->IsValid = true;
  if (FT)
    CurRoot->FuncTables.insert(FT);
  return CurRoot;
}

CangjieDCE::GenericFuncInfo *
CangjieDCE::findTargetInstance(const SmallVectorImpl<GlobalVariable *> &Path) {
  GenericFuncInfo *Root = this->Root;
  for (GlobalVariable *GV : Path) {
    auto It = Root->Next.find(GV);
    if (It == Root->Next.end())
      return nullptr;
    Root = It->second;
  }
  return Root->IsValid ? Root : nullptr;
}

CangjieDCE::GenericFuncInfo *CangjieDCE::resolveVFEMeta(Metadata *MD,
                                                        Module &M) {
  SmallVector<GlobalVariable *, 4> Path;
  MDNode *N;
  while ((N = dyn_cast_or_null<MDNode>(MD))) {
    auto *GV = M.getNamedGlobal(
        dyn_cast<MDString>(N->getOperand(0).get())->getString());
    // Some vcall may encounter situations where the target types does not appear
    // in the current module. In such cases, it can be ensured that the virtual
    // function being called is definitely not defined in the current module.
    if (!GV)
      return nullptr;
    Path.push_back(GV);
    // Layout of MD:
    //  {1.tt, [offset]}
    //  {1.tt, 2.ti, [offset]}
    //  {1.tt, !2, [offset]}, !2 = new MD
    MD = N->getNumOperands() >= 2 ? N->getOperand(1)
                                  : static_cast<Metadata *>(nullptr);
  }
  if (auto *MDS = dyn_cast_or_null<MDString>(MD))
    Path.push_back(M.getNamedGlobal(MDS->getString()));
  return insertInstance(Path);
}

// Obtain the function table FT that achieves P from C, return {C, P, FT}.
std::tuple<GlobalVariable *, GlobalVariable *, GlobalVariable *>
CangjieDCE::scanVTable(GlobalVariable &GV) {
  assert(GV.hasInitializer());
  assert(GV.getType()->getNonOpaquePointerElementType()->getStructName() ==
         "ExtensionDef");
  Constant *C = GV.getInitializer();
  GlobalVariable *FT = dyn_cast<GlobalVariable>(
      C->getOperand(ExtensionDefFieldType::ET_FUNC_TABLE)->stripPointerCasts());
  if (!FT)
    return {nullptr, nullptr, nullptr};
  GlobalVariable *TI =
      cast<GlobalVariable>(C->getOperand(ExtensionDefFieldType::ET_TARGET_TYPE)
                               ->stripPointerCasts());
  assert(GV.hasMetadata("inheritedType"));
  GlobalVariable *IF = GV.getParent()->getNamedGlobal(
      dyn_cast<MDString>(GV.getMetadata("inheritedType")->getOperand(0))
          ->getString());
  return {TI, IF, FT};
}

void CangjieDCE::updateDependencies(
    Function *Caller,
    DenseMap<GenericFuncInfo *, SmallSet<uint64_t, 4>> &Relation) {
  auto Mark = [Caller, this](SmallPtrSetImpl<GlobalVariable *> &FTs,
                             uint64_t Offset) {
    for (auto *FT : FTs) {
      auto *C = FT->getInitializer();
      if (C->getNumOperands() <= Offset)
        continue;
      Function *Callee = dyn_cast_or_null<Function>(
          C->getOperand(Offset)->stripPointerCasts());
      if (!Callee)
        continue;
      assert(Callee != nullptr);
      this->DCE.GVDependencies[Caller].insert(Callee);
    }
  };
  for (auto &[GFI, Offsets] : Relation) {
    for (auto I : Offsets) {
      auto *CurGFI = GFI;
      // 1. Mark all child nodes.
      SmallVector<GenericFuncInfo *, 8> Worklist = {CurGFI};
      while (!Worklist.empty()) {
        auto *Child = Worklist.pop_back_val();
        for (auto [_, N] : Child->Next) {
          Mark(N->FuncTables, I);
          Worklist.push_back(N);
        }
      }
      // 2. Recursively mark all parent nodes.
      while (CurGFI != Root) {
        auto &FTs = CurGFI->FuncTables;
        CurGFI = CurGFI->Prev;
        Mark(FTs, I);
      }
    }
  }
}

void CangjieDCE::addCangjieVirtualFunctionDependencies(Module &M) {
  // For cangjie, it has the following rules:
  //  - If A implements I, then apart from extend, the visibility of I must be
  //  greater than or equal to that of A.
  //  - If A extends I, then the visibility of I may be less than or equal to
  //  that of A. In this case, external packages cannot call I's methods through
  //  A.
  // Therefore, for VFE, it is only necessary to be concerned with the
  // visibility of the implemented type. As long as I is internal, optimization
  // can be performed.
  for (auto &GV : M.globals()) {
    // If typeArgs >= 2, skip it.
    if (!GV.isCJMTable() || !GV.hasMetadata("inheritedType"))
      continue;
    if (!GV.hasInitializer())
      report_fatal_error("ExtensionDef must be initialized.");
    auto [TIC, TII, FT] = scanVTable(GV);
    if (!TIC || !TII || !FT)
      continue;
    if (GlobalValue::isLocalLinkage(TII->getLinkage())) {
      auto *GFI = resolveVFEMeta(GV.getMetadata("inheritedType"), M);
      assert(GFI);
      GFI->FuncTables.insert(FT);
      DCE.VFESafeVTables.insert(FT);
    }
  }

  for (Function &F : M) {
    // Value: offsets
    DenseMap<GenericFuncInfo *, SmallSet<uint64_t, 4>> RelatedGV;
    for (auto &I : instructions(F)) {
      // There is an overlap between MD_obj_type and MD_intro_type, which needs
      // to be considered for merging in the future.
      auto *TypeMD = I.getMetadata(LLVMContext::MD_obj_type);
      if (!TypeMD)
        continue;
      assert(isa<LoadInst>(&I));
      auto *OffsetMD = I.getMetadata(LLVMContext::MD_func_table);
      if (!OffsetMD)
        report_fatal_error("objType present without FuncTable");

      // Merged format: operand(0) is MDNode (vs MDString for single-entry).
      // Each operand pair (TypeMD[i], OffsetMD[i]) represents one (obj_type, offset).
      if (TypeMD->getNumOperands() > 0 && isa<MDNode>(TypeMD->getOperand(0))) {
        if (TypeMD->getNumOperands() != OffsetMD->getNumOperands())
          report_fatal_error(
              "Merged objType and FuncTable have different operand counts");
        for (unsigned i = 0; i < TypeMD->getNumOperands(); i++) {
          auto *SubType = dyn_cast<MDNode>(TypeMD->getOperand(i));
          auto *SubOff = dyn_cast<MDNode>(OffsetMD->getOperand(i));
          if (!SubType || !SubOff || SubOff->getNumOperands() == 0)
            report_fatal_error("Malformed merged objType or FuncTable");
          auto *GFI = resolveVFEMeta(SubType, M);
          if (!GFI)
            continue;
          auto Offset =
              mdconst::extract<ConstantInt>(SubOff->getOperand(0))
                  ->getZExtValue();
          RelatedGV[GFI].insert(Offset);
        }
      } else {
        // Single-entry format.
        auto *GFI = resolveVFEMeta(TypeMD, M);
        if (!GFI)
          continue;
        auto Offset =
            mdconst::extract<ConstantInt>(OffsetMD->getOperand(0))
                ->getZExtValue();
        RelatedGV[GFI].insert(Offset);
      }
    }
    updateDependencies(&F, RelatedGV);
  }
}

CangjieDCE::~CangjieDCE() {
  SmallVector<GenericFuncInfo *> Worklist = {Root};
  SmallVector<GenericFuncInfo *> Stack;
  while (!Worklist.empty()) {
    auto *GFI = Worklist.pop_back_val();
    Stack.push_back(GFI);
    for (auto [_, N] : GFI->Next)
      Worklist.push_back(N);
  }
  while (!Stack.empty())
    delete Stack.pop_back_val();
}

bool CangjieDCE::maybeFakeLiveOfTypeMeta(GlobalVariable *GVU,
                                         GlobalVariable *GV) {
  if (!GV || !GVU)
    return false;
  // All metadata about typeinfo should be regarded as a whole.
  if (GVU->isCJInnerTypeExtensions()) {
    assert(GV->isCJMTable());
    return true;
  }
  if (GVU->isCJStaticGenericTI()) {
    assert(GV->isCJTypeInfo());
    return true;
  }
  if (GV->isCJTypeExt()) {
    assert(GVU->getName().startswith("llvm.used"));
    return true;
  }
  // For the extended function, if both target type and interface are public,
  // mtable needs to remain alive even though neither target type nor interface
  // is in use.
  if (GVU->isCJOuterTypeExtensions()) {
    auto IsInternalType = [](GlobalVariable *Ty) {
      if (Ty->isCJTypeTemplate())
        return GlobalVariable::isInternalLinkage(Ty->getLinkage());
      auto *TT = Ty->getInitializer()
                     ->getOperand(ClassInfoFieldType::CIT_GENERIC_FROM)
                     ->stripPointerCasts();
      if (isa<ConstantPointerNull>(TT))
        return GlobalVariable::isInternalLinkage(Ty->getLinkage());
      return GlobalVariable::isInternalLinkage(
          cast<GlobalVariable>(TT)->getLinkage());
    };
    bool IsTypeInternal = false, IsIFInternal = false;
    auto *Ty = cast<GlobalVariable>(
        GV->getInitializer()
            ->getOperand(ExtensionDefFieldType::ET_TARGET_TYPE)
            ->stripPointerCasts());
    if (Ty->hasInitializer())
      IsTypeInternal = IsInternalType(Ty);
    auto *IFN = GV->getInitializer()
                    ->getOperand(ExtensionDefFieldType::ET_INTERFACE_FN)
                    ->stripPointerCasts();
    if (auto *IF = dyn_cast<GlobalVariable>(IFN))
      IsIFInternal = IF->hasInitializer() && IsInternalType(IF);
    else if (auto *F =
                 dyn_cast<Function>(IFN)) // TODO check whether tt is internal
      IsIFInternal = false;
    return IsTypeInternal || IsIFInternal;
  }
  return false;
}

void CangjieDCE::initTIE(Module &M) {
  for (GlobalVariable &GV : M.globals())
    if (int Idx = getReverseDepsIndex(&GV); Idx != -1) {
      ReverseDeps[GV.getInitializer()->getOperand(Idx)->stripPointerCasts()]
          .insert(&GV);
    }
}

void CangjieDCE::updateLiveExtensions() {
  auto NeedToUpdate = [this](GlobalValue *G) {
    auto *GV = dyn_cast<GlobalVariable>(G);
    if (GV && ReverseDeps.count(GV)) {
      // If typeinfo is alive, then the associated metadata are also alive. Some
      // typeinfo or typetemplate are declaration and do not have attribute.
      assert(isCangjieType(GV) || GV->isDeclaration());
      return true;
    }
    return false;
  };
  auto MarkRelatedMeta = [this, &NeedToUpdate](
                             SmallPtrSetImpl<GlobalVariable *> &LiveSet,
                             SmallVectorImpl<GlobalVariable *> &Updates) {
    // Note that in scenarios where the type and target type are the same,
    // filtering is required to avoid duplicate marking, which could lead to an
    // infinite loop.
    for (auto *GV : LiveSet)
      DCE.MarkLive(*GV);
    SmallVector<GlobalValue *, 8> NewLiveGVs{LiveSet.begin(), LiveSet.end()};
    while (!NewLiveGVs.empty()) {
      GlobalValue *LGV = NewLiveGVs.pop_back_val();
      for (auto *GVD : DCE.GVDependencies[LGV]) {
        DCE.MarkLive(*GVD, &NewLiveGVs);
        if (NeedToUpdate(GVD)) // Recursively handle typeinfo.
          Updates.push_back(cast<GlobalVariable>(GVD));
      }
    }
  };

  SmallVector<GlobalVariable *, 8> LiveTypes;
  for (GlobalValue *G : DCE.AliveGlobals) {
    if (NeedToUpdate(G))
      LiveTypes.push_back(cast<GlobalVariable>(G));
  }
  SmallPtrSet<GlobalVariable *, 8> Visited;
  while (!LiveTypes.empty()) {
    auto *Ty = LiveTypes.pop_back_val();
    if (!Visited.insert(Ty).second)
      continue;
    assert(ReverseDeps.count(Ty));
    MarkRelatedMeta(ReverseDeps[Ty], LiveTypes);
  }
}

void CangjieDCE::rewriteExtensions(Module &M) {
  SmallPtrSet<GlobalVariable *, 4> Candidates;
  // There might also be InnerTypeExtensions. Considering the runtime's parsing
  // method, not rewriting this part does not affect correctness; it only
  // slightly impacts the optimization effect on code size.
  for (GlobalVariable &GV : M.globals())
    if (GV.isCJStaticGenericTI() || GV.isCJOuterTypeExtensions())
      Candidates.insert(&GV);
  for (auto *GV : Candidates) {
    if (!GV->hasInitializer())
      report_fatal_error("Must have initializer");
    auto *C = GV->getInitializer();
    std::vector<Constant *> LiveGVs;
    Type *ATy = nullptr;
    // Create NewGV
    for (unsigned I = 0, E = C->getNumOperands(); I != E; ++I) {
      if (isa<ConstantPointerNull>(C->getOperand(I)))
        continue;
      ATy = cast<GlobalVariable>(C->getOperand(I))->getType();
      LiveGVs.push_back(cast<GlobalVariable>(C->getOperand(I)));
    }
    if (LiveGVs.empty() || LiveGVs.size() == C->getNumOperands())
      continue;
    ArrayType *AT = ArrayType::get(ATy, LiveGVs.size());
    Constant *NewC = ConstantArray::get(AT, LiveGVs);
    GlobalVariable *NewGV = cast<GlobalVariable>(
        M.getOrInsertGlobal(GV->getName().str() + ".new", AT));
    NewGV->setLinkage(GlobalVariable::PrivateLinkage);
    NewGV->setInitializer(NewC);
    NewGV->copyAttributesFrom(GV);
    NewGV->copyMetadata(GV, /*Offset=*/0);
    // Replace GV
    if (GV->getNumUses() != 1) // 1: llvm.used
      report_fatal_error("User number should be one");
    auto *Expr = dyn_cast<ConstantExpr>(GV->use_begin()->getUser());
    Expr->handleOperandChange(GV, NewGV);
    GV->eraseFromParent();
  }
}

void CangjieDCE::rewriteLLVMUsed(Module &M) {
  GlobalVariable *GV = M.getNamedGlobal("llvm.used");
  if (!GV)
    return;
  auto *C = GV->getInitializer();
  std::vector<Constant *> LiveGVs;
  for (unsigned I = 0, E = C->getNumOperands(); I != E; ++I) {
    if (isa<ConstantPointerNull>(C->getOperand(I)))
      continue;
    LiveGVs.push_back(cast<Constant>(C->getOperand(I)));
  }
  if (LiveGVs.empty() || LiveGVs.size() == C->getNumOperands())
    return;
  auto *AT = ArrayType::get(Type::getInt8PtrTy(M.getContext()), LiveGVs.size());
  auto *NewC = ConstantArray::get(AT, LiveGVs);
  GV->setName("llvm.used.old");
  GlobalVariable *NewGV =
      cast<GlobalVariable>(M.getOrInsertGlobal("llvm.used", AT));
  NewGV->setLinkage(GlobalVariable::AppendingLinkage);
  NewGV->setInitializer(NewC);
  NewGV->copyAttributesFrom(GV);
  NewGV->copyMetadata(GV, /*Offset=*/0);
  GV->eraseFromParent();
}
