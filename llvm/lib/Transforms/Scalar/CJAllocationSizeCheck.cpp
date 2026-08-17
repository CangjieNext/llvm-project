//===- CJAllocationSizeCheck.cpp - Cangjie Allocation Size Check *- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This pass rejects oversized allocations whose size computation would
// overflow uint64_t:
//
//   * A single alloca whose allocated type is not representable in uint64
//     bits (e.g. a huge VArray type). DataLayout::getTypeAllocSizeInBits wraps
//     silently on such a type, so its alloc size, GEP offsets and the stack
//     object formed from it are all untrustworthy.
//   * A global variable whose value type size is not representable in uint64
//     bits. It would be laid out with a wrong, smaller extent, so reads and
//     writes through it go out of bounds.
//
// Array counts are multiplied recursively in checked arithmetic. For struct
// types, each element is checked recursively (a member can itself be an
// overflowing array, e.g. a huge VArray member, which DataLayout's struct
// size would hide by wrapping silently) and the element sizes are summed in
// checked arithmetic; the struct's own size still comes from DataLayout, so
// the StructLayout algorithm is not duplicated.
//
// The Cangjie 2GB stack frame limit is not re-checked here: allocations
// whose type size is representable flow downstream, where the X86 and
// AArch64 backends enforce the limit on the final MachineFrameInfo stack
// size.
//
// Unlike CJIRVerifier, this pass is target independent (it does not skip
// ARM), it checks every function and global variable (oversized allocations
// are a resource limit, independent of whether a function uses the Cangjie
// GC, so the type-keeping function 0_for_keeping_some_types is covered too),
// and it reports a complete diagnostic with the demangled Cangjie name, in
// the style of the backend stack-size error. Global variables are named
// directly; function-local allocas are named by their enclosing function.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJAllocationSizeCheck.h"

#include "CangjieDemangle.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CheckedArithmetic.h"
#include "llvm/Transforms/Scalar/CJFillMetadata.h"

using namespace llvm;

namespace {

/// Returns the allocation size of \p Ty in bits, or None if it cannot be
/// represented by uint64_t without overflow (e.g. a huge VArray type).
/// Unlike DataLayout::getTypeAllocSizeInBits, which wraps silently, array
/// counts are multiplied recursively in checked arithmetic. Other types use
/// DataLayout's size.
///
/// Identified struct types form a DAG: the same type can be an element of
/// many structs, and many allocas share one type, so naive recursion would
/// revisit shared nodes exponentially often (a doubling chain
/// %S1={%S0,%S0}, %S2={%S1,%S1}, ... costs 2^depth visits). \p Memo caches
/// every computed size, including overflow results, keeping the check linear
/// in the number of distinct types. Recursive types cannot reach the memo
/// half-populated: a struct can only contain itself through a pointer, which
/// terminates the recursion.
static Optional<uint64_t>
getAllocSizeInBitsChecked(Type *Ty, const DataLayout &DL,
                          DenseMap<Type *, Optional<uint64_t>> &Memo) {
  if (auto It = Memo.find(Ty); It != Memo.end())
    return It->second;
  Optional<uint64_t> Result;
  if (auto *ATy = dyn_cast<ArrayType>(Ty)) {
    if (Optional<uint64_t> ElementSize =
            getAllocSizeInBitsChecked(ATy->getElementType(), DL, Memo))
      Result = checkedMulUnsigned(ATy->getNumElements(), *ElementSize);
    // else: element overflowed, Result stays None.
  } else if (auto *STy = dyn_cast<StructType>(Ty)) {
    // A struct member can itself be an overflowing array (e.g. a huge VArray
    // member): DataLayout's struct size would wrap silently and hide the
    // member's overflow. Check every element recursively and sum the element
    // sizes in checked arithmetic; padding is bounded by alignment and cannot
    // rescue an overflow. The struct's own size still comes from DataLayout.
    uint64_t Sum = 0;
    bool Overflow = false;
    for (Type *Element : STy->elements()) {
      Optional<uint64_t> ElementSize =
          getAllocSizeInBitsChecked(Element, DL, Memo);
      if (!ElementSize) {
        Overflow = true;
        break;
      }
      auto NewSum = checkedAddUnsigned(Sum, *ElementSize);
      if (!NewSum) {
        Overflow = true;
        break;
      }
      Sum = *NewSum;
    }
    if (!Overflow)
      Result = DL.getTypeAllocSizeInBits(Ty).getKnownMinSize();
  } else {
    // getKnownMinSize tolerates scalable types, which cannot overflow: their
    // extent scales at runtime, so the known minimum stays representable.
    Result = DL.getTypeAllocSizeInBits(Ty).getKnownMinSize();
  }
  Memo[Ty] = Result;
  return Result;
}

/// Returns the demangled Cangjie name (e.g. "default::main") of a mangled
/// symbol, or the raw name when demangling yields nothing. Applies to both
/// function and global-variable symbols.
static std::string getDemangledName(StringRef MangledName) {
  auto D = Cangjie::Demangle(MangledName.str());
  std::string DemangledName = D.GetPkgName() +
                              std::string(D.GetPkgName().empty() ? "" : "::") +
                              D.GetFullName();
  if (!DemangledName.empty())
    return DemangledName;
  return MangledName.str();
}

/// Reports a complete, demangled diagnostic for a function-local stack
/// allocation and aborts compilation, in the style of the backend stack-size
/// error.
[[noreturn]] static void reportOversizedInFunction(const Twine &Message,
                                                   const Function &F) {
  std::string Name = getDemangledName(F.getName());
  report_fatal_error(Twine(Message) + " in function " + Name +
                         ".\nCompilation stopped! Please check the "
                         "implementation of " +
                         Name + "!",
                     false);
}

/// Reports a complete, demangled diagnostic for an oversized global variable
/// and aborts compilation. A global variable has no enclosing function, so
/// the diagnostic names the variable itself.
[[noreturn]] static void reportOversizedInGlobal(const Twine &Message,
                                                 const GlobalVariable &GV) {
  std::string Name = getDemangledName(GV.getName());
  report_fatal_error(Twine(Message) + " in global variable " + Name +
                         ".\nCompilation stopped! Please check the "
                         "definition of " +
                         Name + "!",
                     false);
}

/// Checks a single function for oversized stack allocations and aborts on any
/// overflow in the size computation.
static void checkAllocationSize(Function &F, const DataLayout &DL,
                                DenseMap<Type *, Optional<uint64_t>> &Memo) {
  for (Instruction &I : instructions(F)) {
    auto *AI = dyn_cast<AllocaInst>(&I);
    if (!AI)
      continue;
    // Reject any alloca whose allocated type is not representable in uint64
    // bits, including dynamic allocas whose runtime count is unknown.
    if (!getAllocSizeInBitsChecked(AI->getAllocatedType(), DL, Memo))
      reportOversizedInFunction("The allocation type size exceeds the maximum "
                                "representable size",
                                F);
  }
}

/// Checks a single global variable for an oversized value type and aborts on
/// any overflow in the size computation. A global whose value type size wraps
/// uint64 bits would be laid out with a wrong, smaller extent (e.g. the
/// assembler emits only the wrapped byte count), so reads and writes through
/// it go out of bounds.
static void checkGlobalVariable(GlobalVariable &GV, const DataLayout &DL,
                                DenseMap<Type *, Optional<uint64_t>> &Memo) {
  if (!getAllocSizeInBitsChecked(GV.getValueType(), DL, Memo))
    reportOversizedInGlobal("The value type size exceeds the maximum "
                            "representable size",
                            GV);
}

} // namespace

PreservedAnalyses CJAllocationSizeCheck::run(Module &M,
                                             ModuleAnalysisManager &) const {
  if (hasRunCangjieOpt(M))
    return PreservedAnalyses::all();
  const DataLayout &DL = M.getDataLayout();
  DenseMap<Type *, Optional<uint64_t>> Memo;
  for (GlobalVariable &GV : M.globals()) {
    if (!GV.hasInitializer())
      continue;
    checkGlobalVariable(GV, DL, Memo);
  }
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    checkAllocationSize(F, DL, Memo);
  }
  return PreservedAnalyses::all();
}
