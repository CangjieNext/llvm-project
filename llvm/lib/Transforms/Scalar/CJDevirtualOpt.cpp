//===- CJDevirtualOpt.cpp - -------------------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file provides interface to "CJ Devirtual Optimization" pass.
//
// This pass converts cj virtual call into direct call if possible.
//
//===----------------------------------------------------------------------===//
#include "llvm/Transforms/Scalar/CJDevirtualOpt.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Transforms/IPO/CJPartialEscapeAnalysis.h"
#include "llvm/Transforms/Scalar/CJFillMetadata.h"
#include "llvm/Transforms/Utils/CallGraphUpdater.h"

using namespace llvm;

namespace {

struct IntroTypeLoad {
  LoadInst *LI;
  SmallVector<LoadInst *, 4> FuncTableLoads;
  IntroTypeLoad(LoadInst *LI, SmallVector<LoadInst *, 4> FuncTableLoads)
      : LI(LI), FuncTableLoads(FuncTableLoads) {}
};

struct IntroTypeCall {
  CallInst *CI;
  SmallVector<LoadInst *, 4> FuncTableLoads;
  IntroTypeCall(CallInst *CI, SmallVector<LoadInst *, 4> FuncTableLoads)
      : CI(CI), FuncTableLoads(FuncTableLoads) {}
};

struct DevirtualCallData {
  SmallVector<CallInst *, 8> IsSubTypes;
  SmallVector<IntroTypeCall, 4> GetMtableFuncs;
  SmallVector<IntroTypeLoad, 4> IntroTypeLoads;
  SetVector<Function *> FuncPtrs;

  void getRelatedFuncTableLoad(Instruction *Inst,
                               SmallVector<LoadInst *, 4> &FTLIS) {
    SmallVector<Value *, 8> Worklist = {Inst};
    while (!Worklist.empty()) {
      auto *I = dyn_cast<Instruction>(Worklist.pop_back_val());
      assert(I != nullptr);
      switch (I->getOpcode()) {
      default:
        break;
      case Instruction::Call: {
        auto *CB = cast<CallBase>(I);
        if (!CB->getCalledFunction() ||
            CB->getCalledFunction()->getName() != "CJ_MCC_GetMTable")
          break;
        for (auto *UI : I->users())
          Worklist.push_back(UI);
        break;
      }
      case Instruction::Load: {
        if (I->hasMetadata(LLVMContext::MD_func_table) &&
            I->hasMetadata(LLVMContext::MD_intro_type)) {
          FTLIS.push_back(cast<LoadInst>(I));
          break;
        }
      }
        LLVM_FALLTHROUGH;
      case Instruction::GetElementPtr:
      case Instruction::BitCast: {
        for (auto *UI : I->users())
          Worklist.push_back(UI);
      } break;
      }
    }
    // Generally, each LI with IntroType must have a corresponding LI with
    // FuncTable, but in some optimization scenarios LI(FuncTable) may optimize.
    // Conservative treatment.
  }

  explicit DevirtualCallData(Function &F) {
    for (Instruction &I : instructions(F)) {
      // LI: virtual call.
      if (auto *LI = dyn_cast<LoadInst>(&I);
          LI && LI->hasMetadata(LLVMContext::MD_virtual_call)) {
        SmallVector<LoadInst *, 4> FTLIS;
        getRelatedFuncTableLoad(LI, FTLIS);
        if (FTLIS.empty())
          continue;
        IntroTypeLoads.push_back(IntroTypeLoad(LI, FTLIS));
        continue;
      }

      // CI: interface call.
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI || CI->getCalledFunction() == nullptr)
        continue;
      StringRef Name = CI->getCalledFunction()->getName();
      if (Name.equals("CJ_MCC_IsSubType")) {
        IsSubTypes.push_back(CI);
        continue;
      }
      if (Name.equals("CJ_MCC_GetMTable")) {
        SmallVector<LoadInst *, 4> FTLIS;
        getRelatedFuncTableLoad(CI, FTLIS);
        if (FTLIS.empty())
          continue;
        GetMtableFuncs.push_back(IntroTypeCall(CI, FTLIS));
      }
    }
  }
};

} // namespace

static void replaceIsSubType(CallBase *CI, bool Result) {
  CI->replaceAllUsesWith(
      ConstantInt::getBool(Type::getInt1Ty(CI->getContext()), Result));
  CI->eraseFromParent();
}

static std::string getOriginTypeName(GlobalVariable *GV) {
  if (!GV->hasAttribute("CFileKlass")) {
    std::string Name = GV->getName().str();
    size_t Pos = Name.rfind(".ti");
    if (Pos == std::string::npos) {
      report_fatal_error("Incorrect type info name!");
    }
    return Name.substr(0, Pos);
  }
  TypeInfo TI(GV);
  if (GlobalVariable *TT = TI.getTypeTemplate()) {
    std::string Name = TT->getName().str();
    size_t Pos = Name.rfind(".tt");
    if (Pos == std::string::npos) {
      report_fatal_error("Incorrect type template name!");
    }
    return Name.substr(0, Pos) + "<T>";
  } else {
    std::string Name = GV->getName().str();
    size_t Pos = Name.rfind(".ti");
    if (Pos == std::string::npos) {
      report_fatal_error("Incorrect type info name!");
    }
    return Name.substr(0, Pos);
  }
}

// Find the extension-def. Return global value, if the Any extension def
// the Interface. Otherwise, return nullptr.
static GlobalVariable *getExtensionDef(GlobalVariable *Any,
                                       GlobalVariable *Interface) {
  if (!Any->hasInitializer() || !Interface->hasInitializer())
    return nullptr;

  std::string TypeStr1 = getOriginTypeName(Any);
  std::string TypeStr2 = getOriginTypeName(Interface);
  std::string ExtensionDefName = TypeStr1 + "_ed_" + TypeStr2;
  Module *M = Any->getParent();
  return M->getGlobalVariable(ExtensionDefName, true);
}

static bool isSubType(GlobalVariable *Klass0, GlobalVariable *Klass1) {
  if (Klass0 == nullptr)
    return false;

  if (Klass0 == Klass1)
    return true;

  TypeInfo K0(Klass0);
  TypeInfo K1(Klass1);
  switch (K0.Kind) {
  case TK_NOTHING:
    return true;
  case TK_TUPLE: {
    if (!K1.isTuple())
      return false;

    unsigned K0FieldNum = K0.getFieldNum();
    unsigned K1FieldNum = K1.getFieldNum();
    if (K0FieldNum != K1FieldNum)
      return false;

    for (unsigned i = 0; i < K0FieldNum; ++i) {
      if (!K0.getField(i)->hasInitializer() ||
          !K1.getField(i)->hasInitializer()) {
        return false;
      }
      if (!isSubType(K0.getField(i), K1.getField(i))) {
        return false;
      }
    }
    return true;
  }
  case TK_FUNC: {
    report_fatal_error("Should not reach here.");
    return false;
  }
  case TK_CFUNC: {
    if (K1.Kind != TK_CFUNC)
      return false;

    unsigned TypeArgNum = K0.getTypeArgNum();
    if (TypeArgNum != K1.getTypeArgNum())
      return false;

    assert(TypeArgNum > 0 &&
           "The number of the function type arg should be more than 0.");

    // Note: the first of TypeArgs is the return type.
    constexpr unsigned ReturnTypeIdx = 0;
    if (!isSubType(K0.getTypeArg(ReturnTypeIdx), K1.getTypeArg(ReturnTypeIdx)))
      return false;

    for (unsigned i = ReturnTypeIdx + 1; i < TypeArgNum; ++i) {
      if (!isSubType(K1.getTypeArg(i), K0.getTypeArg(i))) {
        return false;
      }
    }
    return true;
  }
  case TK_RAWARRAY:
    // The array type has no subtype relationship.
    return false;
  case TK_CLASS:
  case TK_TEMP_ENUM: {
    if (!K1.isClass())
      return false;

    GlobalVariable *SuperKlass = K0.getSuperOrComponent();
    while (SuperKlass != nullptr) {
      if (SuperKlass == Klass1)
        return true;

      if (!SuperKlass->hasInitializer())
        return false;

      TypeInfo Super(SuperKlass);
      SuperKlass = Super.getSuperOrComponent();
    }
    return false;
  }
  default:
    if (K0.isInterface() || K1.isInterface())
      report_fatal_error("The interface type is processed in subtype.!");

    return false;
  }
  return false;
}

// Reture true if K0 is subtype of K1, otherwise, return false.
static bool isFunctionSubType(TypeInfo &K0, TypeInfo &K1) {
  // The function type information is stored in the `SuperClass`
  // of the closure instance. Therefore, having SuperClass and
  // SuperClass->IsFunc are prerequisites for the instance to be
  // of the function type.
  auto Super = K0.getSuperOrComponent();
  if (!Super)
    return false;

  TypeInfo SuperTI(Super);
  if (!SuperTI.isFunction())
    return false;

  // Now, `SuperTI` and `K1` both are Closure type.
  // Get function type from Closure type, i.e., typeArgs[0]:
  K0.resetTypeInfo(SuperTI.getTypeArg(0));
  K1.resetTypeInfo(K1.getTypeArg(0));
  // In addition, it is necessary to have the same number of TypeArgNum.
  unsigned TypeArgNum = K0.getTypeArgNum();
  if (TypeArgNum != K1.getTypeArgNum())
    return false;

  return isSubType(K0.GV, K1.GV);
}

static bool tryInferSubType(DevirtualCallData &CallData) {
  bool Changed = false;
  if (CallData.IsSubTypes.empty())
    return Changed;

  for (auto CI : CallData.IsSubTypes) {
    GlobalVariable *Klass0 = findTypeInfoGV(CI->getArgOperand(0));
    GlobalVariable *Klass1 = findTypeInfoGV(CI->getArgOperand(1));

    if (Klass0 == nullptr || Klass1 == nullptr)
      continue;

    if (Klass0 == Klass1) {
      replaceIsSubType(CI, true);
      Changed = true;
      continue;
    }

    if (!Klass0->hasInitializer() || !Klass1->hasInitializer())
      continue;

    TypeInfo K0(Klass0);
    TypeInfo K1(Klass1);
    bool Result = true;
    if (K0.isInterface() || K1.isInterface()) {
      // If it find the extensionDef of klass, the subType is true.
      // Otherwise, the subtype is unknown.
      if (getExtensionDef(Klass0, Klass1) == nullptr)
        continue;
    } else if (K1.isFunction()) {
      Result = isFunctionSubType(K0, K1);
    } else {
      Result = isSubType(Klass0, Klass1);
    }

    replaceIsSubType(CI, Result);
    Changed = true;
  }
  return Changed;
}

// For virtual call:
//  %func_ptr = load i8*, i8** %9, !FuncTable
// For interface call:
//  %func_ptr = call i8** @CJ_MCC_GetMTable(i8* %TI1, i8* %TI2, i64 i)
// And then:
//  %func = bitcast i8* %func_ptr to %func_type
//  %ret = call %func_type @func(...)
// ====>
// %ret = call %func_type @specific_callee(...)
static void replaceIVCall(Instruction *I, ExtensionDefData &Data,
                          uint64_t FuncIdx) {
  Function *TableFunc = Data.getFuncByIndex(FuncIdx);
  IRBuilder<> IRB(I);
  auto *BI = IRB.CreateBitCast(TableFunc, I->getType());
  I->replaceAllUsesWith(BI);
  assert(I->use_empty() && "CJ_MCC_GetMTable user is not empty");
  I->eraseFromParent();
}

static bool devirtual(DevirtualCallData &CallData) {
  bool Changed = false;
  auto DevirtualImpl = [](GlobalVariable *Klass0, GlobalVariable *Klass1,
                          Instruction *I, uint64_t FuncIndex) {
    if (Klass0 == nullptr || Klass1 == nullptr)
      return false;
    GlobalVariable *ED = getExtensionDef(Klass0, Klass1);
    if (ED == nullptr || !ED->hasInitializer())
      return false;
    ExtensionDefData Data(ED);
    replaceIVCall(I, Data, FuncIndex);
    return true;
  };
  auto ResolveFTLI =
      [&DevirtualImpl](
          ArrayRef<LoadInst *> FTLIS, GlobalVariable *Klass0,
          SmallVector<LoadInst *, 4> &UnresolvedLoad,
          const std::function<GlobalVariable *(LoadInst *)> &GetKlass1)
      -> bool {
    bool Changed = false;
    for (auto *FTLI : FTLIS) {
      MDNode *MDFT = FTLI->getMetadata(LLVMContext::MD_func_table);
      assert(MDFT != nullptr && "metadata IntroType must have value");
      // Skip merged metadata format (produced when instructions with different
      // VFE metadata are combined). Merged format has MDNode operands instead
      // of ConstantAsMetadata.
      if (isa<MDNode>(MDFT->getOperand(0))) {
        UnresolvedLoad.push_back(FTLI);
        continue;
      }
      uint64_t FuncIndex =
          mdconst::extract<ConstantInt>(MDFT->getOperand(0))->getZExtValue();
      bool Resolved = DevirtualImpl(Klass0, GetKlass1(FTLI), FTLI, FuncIndex);
      if (!Resolved) {
        UnresolvedLoad.push_back(FTLI);
      } else {
        Changed = true;
      }
    }
    return Changed;
  };
  if (CallData.GetMtableFuncs.empty() && CallData.IntroTypeLoads.empty())
    return Changed;

  SmallVector<IntroTypeCall, 8> UnresolvedMtableFuncs;
  SmallVector<IntroTypeLoad, 8> UnresolvedIntroTypeLoads;
  for (auto ITC : CallData.GetMtableFuncs) {
    UnresolvedMtableFuncs.push_back(ITC);
  }
  for (auto ITL : CallData.IntroTypeLoads) {
    UnresolvedIntroTypeLoads.push_back(ITL);
  }
  bool OnceResolved = true;
  uint32_t MTableIndex = 0;
  uint32_t LoadIndex = 0;
  while (OnceResolved) {
    OnceResolved = false;
    uint32_t ResolvedMSize = UnresolvedMtableFuncs.size();
    uint32_t ResolvedLSize = UnresolvedIntroTypeLoads.size();
    for (; MTableIndex < ResolvedMSize; MTableIndex++) {
      auto &ITC = UnresolvedMtableFuncs[MTableIndex];
      CallInst *CI = ITC.CI;
      GlobalVariable *Klass0 = findTypeInfoGV(CI->getArgOperand(0));
      GlobalVariable *Klass1 = findTypeInfoGV(CI->getArgOperand(1));
      SmallVector<LoadInst *, 4> UnresolvedLoad;
      OnceResolved |= ResolveFTLI(
          ITC.FuncTableLoads, Klass0, UnresolvedLoad,
          [&Klass1](LoadInst *) -> GlobalVariable * { return Klass1; });
      if (!UnresolvedLoad.empty())
        UnresolvedMtableFuncs.push_back(IntroTypeCall(CI, UnresolvedLoad));
    }
    for (; LoadIndex < ResolvedLSize; LoadIndex++) {
      auto &ITL = UnresolvedIntroTypeLoads[LoadIndex];
      auto *TILI = ITL.LI;
      auto &FTLIS = ITL.FuncTableLoads;
      // If the load pointer is phi or select, no processing is performed
      // currently and can be optimized later.
      if (!isa<GEPOperator>(TILI->getPointerOperand()->stripPointerCasts()))
        continue;
      Value *TI =
          cast<GEPOperator>(TILI->getPointerOperand()->stripPointerCasts())
              ->getPointerOperand();
      // Klass0 is get from TILI (the first load).
      GlobalVariable *Klass0 = findTypeInfoGV(TI);
      SmallVector<LoadInst *, 4> UnresolvedLoad;
      OnceResolved |=
          ResolveFTLI(FTLIS, Klass0, UnresolvedLoad, [](LoadInst *FTLI) {
            MDNode *MDTI = FTLI->getMetadata(LLVMContext::MD_intro_type);
            if (MDTI == nullptr)
              report_fatal_error("metadata IntroType must have value");
            assert(MDTI != nullptr && "");
            const Twine &GVName =
                dyn_cast<MDString>(MDTI->getOperand(0))->getString() +
                Twine(".ti");
            // Klass1 is get from FTLI (the last load).
            // This is necessary because TI may be instantiated from TT.
            GlobalVariable *Klass1 =
                FTLI->getModule()->getGlobalVariable(GVName.str(), true);
            return Klass1;
          });
      if (!UnresolvedLoad.empty()) {
        UnresolvedIntroTypeLoads.push_back(IntroTypeLoad(TILI, UnresolvedLoad));
      }
    }
    Changed |= OnceResolved;
  }
  return Changed;
}

PreservedAnalyses CJDevirtualOpt::run(LazyCallGraph::SCC &C,
                                      CGSCCAnalysisManager &AM,
                                      LazyCallGraph &CG,
                                      CGSCCUpdateResult &UR) const {
  FunctionAnalysisManager &FAM =
      AM.getResult<FunctionAnalysisManagerCGSCCProxy>(C, CG).getManager();
  auto LookupLoopInfo = [&FAM](Function &F) -> LoopInfo & {
    return FAM.getResult<LoopAnalysis>(F);
  };
  bool Changed = false;
  CallGraphUpdater CGUpdater;
  CGUpdater.initialize(CG, C, AM, UR);
  for (LazyCallGraph::Node &N : C) {
    Function &F = N.getFunction();
    DevirtualCallData CallData(F);

    Changed |= tryInferSubType(CallData);
    if (devirtual(CallData)) {
      Changed = true;
      for (auto Func : CallData.FuncPtrs) {
        if (!Func->hasEscapeAnalysis() && !Func->isDeclaration()) {
          Changed |= escapeAnalysisFuncImpl(Func, LookupLoopInfo);
        }
      }
      CGUpdater.reanalyzeFunction(F);
    }
  }
  if (Changed) {
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}
