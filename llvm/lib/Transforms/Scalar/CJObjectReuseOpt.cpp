//===- CJObjectReuseOpt.cpp - -----------------------------------------*- C++
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
// This file provides interface to "Cangjie Object Reuse Optimization" pass.
//
// This pass will optimize the scene where intra-function heap objects are
// non-overlappping live range and no escaping. Reuse the allocated memory
// of dead object to reduce the frequency of memory management overhead.
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJObjectReuseOpt.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/SafepointIRVerifier.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/CJFillMetadata.h"
#include "llvm/Transforms/Utils/CJSimpleGraphColoring.h"

#include <set>

using namespace llvm;

#define DEBUG_TYPE "CJObjectReuseOpt"

void CJObjectReuseOpt::clearMap() {
  ReuseHeapVarMap.clear();
  ReuseStackVarMap.clear();
  HeapCJVarToLiveInterval.clear();
  StackCJVarToLiveInterval.clear();
  CJVarToMemEquivalenceVars.clear();
}

/*
  The pass reuses the same bit size of object memory which are
  not escape, lifetime disjoint and the same addrspace.

  Pseudo Code:
  IF sizeof(ClassA) == sizeof(ClassB) &&
     (%Addr0 NOT Escape) && (%Addr1 NOT Escape) &&
     (The AddrSpace of %Addr0 == The AddrSpace of %Addr1) &&
     (The CJLifetime of %Addr0) ∩ (The CJLifetime of %Addr1) == ∅ &&
     dominate(%Addr0, %Addr1)
  THEN During The CJLifetime of %Addr1:
    1. %Addr0 replace %Addr1
    2. %MemEquivalence_of_Addr0 replace The uses of %MemEquivalence_of_Addr0
       (Same Type replace directly, otherwise try to add BitCast Instruction)

  ============== Before CJObjectReuseOpt Pass ===============:

  %Addr0 = AddrSpaceCast %MemPtr, %MemPtrWithAddrSpaceInfo
  Call Ctor<init> %Addr0,  %ClassA_TypeInfo
  %MemEquivalence_Addr0 = BitCast %Addr0, %OtherPointerType_C
  ...
  Call Ctor<init> %Addr1,  %ClassB_TypeInfo
  %MemEquivalence_Addr1 = BitCast %Addr1, %OtherPointerType_C
  Call Foo %MemEquivalence_Addr1, %OtherParam (NOT Escape)

  ============== After CJObjectReuseOpt Pass ===============:

  %Addr0 = AddrSpaceCast %MemPtr, %MemPtrWithAddrSpaceInfo
  Call Ctor<init> %Addr0,  %ClassA_TypeInfo
  %MemEquivalence_Addr0 = BitCast %Addr0, %OtherPointerType_C
  ...
  Call Ctor<init> %Addr0,  %ClassB_TypeInfo
  %MemEquivalence_Addr1 = BitCast %Addr0, %OtherPointerType_C
  Call Foo %MemEquivalence_Addr0, %OtherParam (NOT Escape)

  Note:
  %MemEquivalence_Addr1 has empty uses and
  %MemEquivalence_Addr1 = BitCast %Addr0, %OtherPointerType_C
  will be remove by the DCE pass comes after CJObjectReuseOpt Pass
*/

bool CJObjectReuseOpt::runImpl(Function &F,
                               FunctionVarLifeTimeResult &LifeTimeResult,
                               AliasAnalysis &AA, DominatorTree &DT,
                               LoopInfo &LI) {
  clearMap();
  CandidateReuseMap BitSizeToNoEscapeVals;
  SmallPtrSet<const Value *, 32> EphValues;
  EarliestEscapeInfo EI(DT, LI, EphValues);
  BatchAAResults BatchAA(AA, &EI);
#ifndef NDEBUG
  LLVM_DEBUG(dbgs() << "CJObjectReuseOpt start in : " << F.getName().str()
                    << "\n");
#endif
  collectNoEscapeValue(F, BitSizeToNoEscapeVals);
  adjustForReuseByLiveInterval(F, BitSizeToNoEscapeVals, LifeTimeResult,
                               BatchAA);
  bool Changed = transformForMemReuse(DT, LifeTimeResult);
#ifndef NDEBUG
  if (Changed) {
    LLVM_DEBUG(dbgs() << "CJObjectReuseOpt changed in : " << F.getName().str()
                      << "\n");
  }
  LLVM_DEBUG(dbgs() << "CJObjectReuseOpt end in : " << F.getName().str()
                    << "\n");
#endif
  return Changed;
}

PreservedAnalyses CJObjectReuseOpt::run(Function &F,
                                        FunctionAnalysisManager &FAM) {
  auto &LifeTimeResult = FAM.getResult<CJFunctionVarLifeTime>(F);
  auto &AA = FAM.getResult<AAManager>(F);
  auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  auto &LI = FAM.getResult<LoopAnalysis>(F);
  if (runImpl(F, LifeTimeResult, AA, DT, LI)) {
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

bool CJObjectReuseOpt::isHeapZoneValue(Value *Val) {
  static SmallDenseMap<Value *, bool, 4> CacheAnalysis;
  if (CacheAnalysis.count(Val)) {
    return CacheAnalysis[Val];
  }
  assert(Val->getType()->isPointerTy() &&
         "illegal val in CJObjectReuseOpt::isHeapZoneValue.");
  Value *Addr = Val;
  // Check whether the origin def comes from addrspace(0)
  // Currently we only process addrspacecast scene, more scene can be
  // complemented False positive is fine for it only misses optimazition
  while (auto *Inst = dyn_cast<AddrSpaceCastInst>(Addr)) {
    unsigned AddrSpace = Inst->getSrcAddressSpace();
    if (AddrSpace == 0) {
      CacheAnalysis[Val] = false;
      return false;
    }
    Addr = Inst->getPointerOperand();
  }
  CacheAnalysis[Val] = true;
  return true;
}

inline static bool isPointerCastFamily(Value *Val) {
  auto *Instr = dyn_cast<Instruction>(Val);
  if (Instr == nullptr) {
    return false;
  }
  if (Instr->getOpcode() == Instruction::AddrSpaceCast ||
      Instr->getOpcode() == Instruction::BitCast ||
      Instr->getOpcode() == Instruction::PtrToInt ||
      Instr->getOpcode() == Instruction::IntToPtr) {
    return true;
  }
  return false;
}

inline static bool isCJMallocObj(Value *V) {
  if (auto *CB = dyn_cast<CallBase>(V)) {
    return CB->getIntrinsicID() == Intrinsic::cj_malloc_object;
  }
  return false;
}

inline static bool isTypeHasPadding(Type *AddrElemType, const DataLayout &DL) {
  if (auto *ST = dyn_cast<StructType>(AddrElemType)) {
    auto *SL = DL.getStructLayout(ST);
    if (SL->hasPadding()) {
      return true;
    }
    uint64_t EndOffset = SL->getSizeInBytes();
    uint64_t Offset = 0;
    while (Offset < EndOffset) {
      unsigned Index = SL->getElementContainingOffset(Offset);
      Type *ElementTy = ST->getElementType(Index);
      uint64_t ElementSize = DL.getTypeAllocSize(ElementTy).getFixedSize();
      if (isTypeHasPadding(ElementTy, DL)) {
        return true;
      }
      Offset += ElementSize;
    }
  } else if (auto *AT = dyn_cast<ArrayType>(AddrElemType)) {
    Type *ElementTy = AT->getElementType();
    return isTypeHasPadding(ElementTy, DL);
  } else if (auto *VT = dyn_cast<VectorType>(AddrElemType)) {
    Type *ElementTy = VT->getElementType();
    return isTypeHasPadding(ElementTy, DL);
  }

  return false;
}

static bool isDefChainConsistOfCastOrPHI(Value *V) {
  if (auto *Instr = dyn_cast<Instruction>(V)) {
    // Only process the source is the cj.malloc.object or alloc now
    while (Instr && !(isa<AllocaInst>(Instr) || isCJMallocObj(Instr))) {
      if (const Argument *Arg = dyn_cast<Argument>(Instr)) {
        return false;
      }
      if (isPointerCastFamily(Instr)) {
        Instr = dyn_cast<Instruction>(Instr->getOperand(0));
      } else if (auto *PHI = dyn_cast<PHINode>(Instr);
                 PHI && PHI->getNumIncomingValues() == 1) {
        Instr = dyn_cast<Instruction>(PHI->getIncomingValue(0));
      } else {
        return false;
      }
    }
    return (isa<AllocaInst>(Instr) || isCJMallocObj(Instr));
  }
  return false;
}

// Collect the same bit size of object memory which are not escape by Ctor
// instruction
void CJObjectReuseOpt::collectNoEscapeValue(
    Function &F, CandidateReuseMap &BitSizeToNoEscapeVals) {
  constexpr StringRef CONSTRUCTOR_KEY_WORD = "<init>";
  constexpr unsigned BYTE_BIT_WIDTH = 8;
  for (auto &I : instructions(F)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      Function *Callee = CB->getCalledFunction();
      if (Callee && Callee->getName().contains(CONSTRUCTOR_KEY_WORD)) {
        Value *Addr = CB->getArgOperand(0);
        if (!isDefChainConsistOfCastOrPHI(Addr)) {
          continue;
        }
        auto *GV =
            dyn_cast<GlobalVariable>(CB->getArgOperand(CB->arg_size() - 1));
        if (!GV || !GV->hasInitializer() ||
            !GV->hasAttribute("NotModifiableClass")) {
          continue;
        }
        auto *ClassInfo = GV->getInitializer();
        if (ClassInfo->getNumOperands() <= ClassInfoFieldType::CIT_SIZE) {
          continue;
        }
        // The size field of a generic class (e.g. Array<T>) is a
        // ConstantExpr rather than a ConstantInt, so only accept a plain
        // integer constant here. getUniqueInteger() would assert otherwise.
        auto *SizeInt = dyn_cast<ConstantInt>(
            ClassInfo->getOperand(ClassInfoFieldType::CIT_SIZE));
        if (!SizeInt) {
          continue;
        }
        uint64_t BitSize = SizeInt->getZExtValue() * BYTE_BIT_WIDTH;
        if (BitSize == 0) {
          continue;
        }

        if (!isHeapZoneValue(Addr) || !PointerMayBeCaptured(Addr, true, true)) {
          BitSizeToNoEscapeVals[BitSize].push_back(CB);
#ifndef NDEBUG
          LLVM_DEBUG(dbgs() << "BitSize: " << std::to_string(BitSize)
                            << ", no escape CB:" << *CB << "\n");
#endif
        }
      }
    }
  }
}

void CJObjectReuseOpt::adjustForReuseByLiveInterval(
    Function &F, CandidateReuseMap &BitSizeToNoEscapeVals,
    FunctionVarLifeTimeResult &LifeTimeResult, BatchAAResults &AARes) {
  CandidateReuseMap HeapVars;
  CandidateReuseMap StackVar;

  for (auto &[BitSize, VarArr] : BitSizeToNoEscapeVals) {
    for (auto *CB : VarArr) {
      if (isHeapZoneValue(CB->getArgOperand(0))) {
        HeapVars[BitSize].push_back(CB);
      } else {
        StackVar[BitSize].push_back(CB);
      }
    }
  }

  // Note in SSA form, one CJ var can map to multiple SSA vars
  // The CJVarLifeTime = ∪ {The SSA Var lifetime of memEquivalence}
  auto CalculateLiveIntervalForCJVar =
      [this, &F, &LifeTimeResult,
       &AARes](CandidateReuseMap &InputMap) -> CJVarToLiveIntervalMap {
    CJVarToLiveIntervalMap CJVarToLiveInterval;
    for (auto &[BitSize, VarArr] : InputMap) {
      std::set<CallBase *> SkipCB;
      for (auto *CJVar : VarArr) {
        bool AddCJVarToLiveIntervalMap = true;
        FunctionVarLifeTimeResult::LiveInterval CJVarLiveInterval =
            LifeTimeResult.SSAVarToLiveInterval[CJVar->getArgOperand(0)];
        MemoryLocation CJVarLoc = MemoryLocation(
            CJVar->getArgOperand(0), LocationSize::precise(BitSize));
        for (auto &Instr : instructions(F)) {
          if (!Instr.getType()->isPointerTy()) {
            continue;
          }
          if (&Instr != CJVar) {
            AliasResult AliasRes =
                AARes.alias(CJVarLoc, MemoryLocation::getAfter(&Instr));
            // MayAlias means we couldn't decide whether to replace mem or not,
            // PartialAlias means we should replace mem partly, currently not
            // support.
            if (AliasRes == AliasResult::MayAlias ||
                AliasRes == AliasResult::PartialAlias) {
              AddCJVarToLiveIntervalMap = false;
              SkipCB.insert(CJVar);
              break;
            }
            if (AliasRes == AliasResult::MustAlias &&
                LifeTimeResult.SSAVarToLiveInterval[&Instr].second >=
                    LifeTimeResult.InstrsToIndex[CJVar]) {
              this->CJVarToMemEquivalenceVars[CJVar].insert(&Instr);
#ifndef NDEBUG
        LLVM_DEBUG({
          dbgs() << "CJ Var: " << *CJVar << "\n";
          dbgs() << "MemEquivalenceVar: " << Instr << "\n"
                 << "MemEquivalenceVar LiveInterval = {"
                 << std::to_string(LifeTimeResult.SSAVarToLiveInterval[&Instr].first) << ", "
                 << std::to_string(LifeTimeResult.SSAVarToLiveInterval[&Instr].second) << "}"
                 << "\n";
        });
#endif
              CJVarLiveInterval =
                  FunctionVarLifeTimeResult::combineLifeInterval(
                      CJVarLiveInterval,
                      LifeTimeResult.SSAVarToLiveInterval[&Instr]);
            }
          }
        }
        if (!AddCJVarToLiveIntervalMap) {
          continue;
        }
        CJVarLiveInterval.first = LifeTimeResult.InstrsToIndex[CJVar];
        CJVarToLiveInterval[CJVar] = CJVarLiveInterval;
#ifndef NDEBUG
        LLVM_DEBUG({
          dbgs() << "CJ Var: " << *CJVar << "\n";
          dbgs() << "CJVarLiveInterval = {"
                 << std::to_string(CJVarLiveInterval.first) << ", "
                 << std::to_string(CJVarLiveInterval.second) << "}"
                 << "\n";
        });
#endif
      }
      // remove skip CB from candidateReuseMap
      for (auto *CB : SkipCB) {
        VarArr.erase(std::remove(VarArr.begin(), VarArr.end(), CB),
                     VarArr.end());
      }
    }
    return CJVarToLiveInterval;
  };

  HeapCJVarToLiveInterval = CalculateLiveIntervalForCJVar(HeapVars);
  StackCJVarToLiveInterval = CalculateLiveIntervalForCJVar(StackVar);

  auto SplitNonOverlappingCJVar =
      [](CandidateReuseMap &InputMap, ReuseMultiMap &OutputMap,
         CJVarToLiveIntervalMap &CJVarToLiveInterval) {
        // Split the CJ var in the fixed bitsize VarArr by disjoint live
        // interval
        for (auto &[BitSize, VarArr] : InputMap) {
          bool AllCJVarsHaveLiveInterval = true;
          for (auto *CB : VarArr) {
            if (CJVarToLiveInterval.count(CB) == 0) {
              AllCJVarsHaveLiveInterval = false;
              break;
            }
          }
          if (!AllCJVarsHaveLiveInterval) {
            continue;
          }
          // Use graph color algorithm to collect CJVars which can be reused
          InstrLiveIntervalConflictGraph ILICG =
              InstrLiveIntervalConflictGraph(VarArr, CJVarToLiveInterval);
          SimpleGraphColoring<InstrLiveIntervalConflictGraph *> GraphColoring(
              &ILICG);
          GraphColoring.color();
          std::vector<SmallVector<CallBase *, 4>> Temp(
              GraphColoring.getColorNum(), SmallVector<CallBase *, 4>{});
          for (auto &[Node, Color] : GraphColoring.getColorMapRes()) {
            Temp[Color - 1].push_back(dyn_cast<CallBase>(Node->getInstr()));
          }
          for (auto &ReuseGroup : Temp) {
            if (ReuseGroup.size() > 1) {
              OutputMap.insert({BitSize, ReuseGroup});
            }
#ifndef NDEBUG
            LLVM_DEBUG({
              dbgs() << "SameColorGroup BitSize: " << std::to_string(BitSize)
                     << "\n";
              for (auto *CB : ReuseGroup) {
                dbgs() << *CB << "\n";
              }
            });
#endif
          }
        }
      };

  SplitNonOverlappingCJVar(HeapVars, ReuseHeapVarMap, HeapCJVarToLiveInterval);
  SplitNonOverlappingCJVar(StackVar, ReuseStackVarMap,
                           StackCJVarToLiveInterval);
}

bool CJObjectReuseOpt::transformForMemReuse(
    DominatorTree &DT, FunctionVarLifeTimeResult &LifeTimeResult) {
  bool Changed = false;

  auto ReplaceUseBetweenCJValLifeTime =
      [&LifeTimeResult](Value *Replacee, Value *Replacer, CallBase *CJVar,
                        CJVarToLiveIntervalMap &CJVarLifeTimeMap) {
        Value *FinalReplacer = Replacer;
        if (Replacer->getType() != Replacee->getType()) {
          IRBuilder<> IRB(CJVar);
          bool SameAddrSpace = getUnderlyingObject(Replacer)
                                   ->getType()
                                   ->getPointerAddressSpace() ==
                               Replacee->getType()->getPointerAddressSpace();
          if (!SameAddrSpace) {
            FinalReplacer =
                IRB.CreateBitCast(getUnderlyingObject(Replacer),
                                  getUnderlyingObject(Replacee)->getType());
            FinalReplacer =
                IRB.CreateAddrSpaceCast(FinalReplacer, Replacee->getType());
          } else {
            FinalReplacer = IRB.CreateBitCast(getUnderlyingObject(Replacer),
                                              Replacee->getType());
          }
        }
        Replacee->replaceUsesWithIf(
            FinalReplacer,
            [&LifeTimeResult, &CJVarLifeTimeMap, &CJVar
#ifndef NDEBUG
             ,
             &FinalReplacer
#endif
        ](Use &U) -> bool {
              auto *Instr = dyn_cast<Instruction>(U.getUser());
              bool DoReplace = LifeTimeResult.InstrsToIndex[Instr] >=
                                   CJVarLifeTimeMap[CJVar].first &&
                               LifeTimeResult.InstrsToIndex[Instr] <=
                                   CJVarLifeTimeMap[CJVar].second;
#ifndef NDEBUG
              if (DoReplace) {
                LLVM_DEBUG({
                  dbgs() << "FinalReplacer : "
                         << *dyn_cast<Instruction>(FinalReplacer) << "\n";
                  dbgs() << "BeReplaced : " << *Instr << "\n";
                  dbgs() << "=================================" << "\n";
                });
              }
#endif
              return DoReplace;
            });
      };

  auto ReplaceValueForReuse =
      [this, &Changed, &DT, &LifeTimeResult, &ReplaceUseBetweenCJValLifeTime](
          ReuseMultiMap &InputMap, CJVarToLiveIntervalMap &CJVarLifeTimeMap) {
        for (auto &[BitSize, VarArr] : InputMap) {
          std::sort(VarArr.begin(), VarArr.end(),
                    [&LifeTimeResult](CallBase *LHS, CallBase *RHS) {
                      return LifeTimeResult.InstrsToIndex[LHS] <
                             LifeTimeResult.InstrsToIndex[RHS];
                    });
          std::set<size_t> HaveBeenReplacedIndex;
          for (size_t ReplacerIndex = 0; ReplacerIndex < VarArr.size() - 1;
               ++ReplacerIndex) {
            if (HaveBeenReplacedIndex.count(ReplacerIndex)) {
              continue;
            }
            auto *ReplacerCB = VarArr[ReplacerIndex];
            std::unordered_map<Type *, Value *> ReplacerTypeToValue;
            auto &ReplacerMemEquivalence =
                this->CJVarToMemEquivalenceVars[ReplacerCB];
            for (auto *E : ReplacerMemEquivalence) {
              if (ReplacerTypeToValue.count(E->getType())) {
                continue;
              }
              ReplacerTypeToValue[E->getType()] = E;
            }
            for (size_t BeReplacedIndex = ReplacerIndex + 1;
                 BeReplacedIndex < VarArr.size(); ++BeReplacedIndex) {
              auto *BeReplacedCB = VarArr[BeReplacedIndex];
              if (HaveBeenReplacedIndex.count(ReplacerIndex)) {
                continue;
              }
              Value *ReuseAddr = ReplacerCB->getArgOperand(0);
              Value *BeReplacedAddr = BeReplacedCB->getArgOperand(0);
              // when addr type isn't the same and one of them has padding,
              // the replacement is not allowed for SROA pass may generates
              // undef behavior instruction caused by using padding data.
              const auto &DL = dyn_cast<Instruction>(ReuseAddr)->getModule()->getDataLayout();
              auto ReplaceElemType = getUnderlyingObject(ReuseAddr)
                                         ->getType()
                                         ->getNonOpaquePointerElementType();
              auto BeReplaceElemType = getUnderlyingObject(BeReplacedAddr)
                                           ->getType()
                                           ->getNonOpaquePointerElementType();
              if (ReplaceElemType != BeReplaceElemType &&
                  (isTypeHasPadding(ReplaceElemType, DL) ||
                   isTypeHasPadding(BeReplaceElemType, DL))) {
                continue;
              }
              if (ReuseAddr->getType()->getPointerAddressSpace() !=
                  BeReplacedAddr->getType()->getPointerAddressSpace()) {
                continue;
              }
              if (!DT.dominates(ReplacerCB->getParent(),
                                BeReplacedCB->getParent())) {
                continue;
              }
#ifndef NDEBUG
              LLVM_DEBUG({
                dbgs() << "ReplacerCB : " << *ReplacerCB << "\n";
                dbgs() << "BeReplacedCB : " << *BeReplacedCB << "\n";
                dbgs() << "=================================" << "\n";
              });
#endif
              ReplaceUseBetweenCJValLifeTime(BeReplacedAddr, ReuseAddr,
                                             BeReplacedCB, CJVarLifeTimeMap);
              auto &BeReplacedMemEquivalence =
                  this->CJVarToMemEquivalenceVars[BeReplacedCB];
              for (auto *V : BeReplacedMemEquivalence) {
                if (V == BeReplacedAddr) {
                  continue;
                }
                Value *ReplacerMemEquivalenceAddr = ReuseAddr;
                if (ReplacerTypeToValue.count(V->getType())) {
                  ReplacerMemEquivalenceAddr =
                      ReplacerTypeToValue[V->getType()];
                }
                ReplaceUseBetweenCJValLifeTime(V, ReplacerMemEquivalenceAddr,
                                               BeReplacedCB, CJVarLifeTimeMap);
              }
              HaveBeenReplacedIndex.insert(BeReplacedIndex);
              Changed = true;
            }
          }
        }
      };

  ReplaceValueForReuse(ReuseHeapVarMap, HeapCJVarToLiveInterval);
  ReplaceValueForReuse(ReuseStackVarMap, StackCJVarToLiveInterval);

  return Changed;
}