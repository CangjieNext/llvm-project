//===- CJAllocationSizeCheck.cpp - Cangjie Allocation Size Check *- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This pass rejects oversized stack allocations whose size computation would
// overflow uint64_t:
//
//   * A single alloca whose allocated type is not representable in uint64
//     bits (e.g. a huge VArray type). DataLayout::getTypeAllocSizeInBits wraps
//     silently on such a type, so its alloc size, GEP offsets and the stack
//     object formed from it are all untrustworthy.
//   * A static frame whose accumulated size wraps uint64 bits. Each term is
//     the allocated type size times the constant element count (a product the
//     backend forms unchecked in FunctionLoweringInfo), and the running sum
//     is accumulated in checked arithmetic.
//
// A frame whose size fits uint64 bits stays below 2^61 bytes, far below the
// int64 byte offsets that PrologEpilogInserter accumulates, so no downstream
// computation can wrap. The Cangjie 2GB stack frame limit itself is not
// re-checked here: frames that do not overflow flow downstream, where the
// X86 and AArch64 backends enforce the limit on the final MachineFrameInfo
// stack size, accounting for spill slots and padding that IR-level summing
// cannot see.
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

#include <climits>

using namespace llvm;

namespace {

/// Returns the allocation size of \p Ty in bits, or None if it cannot be
/// represented by uint64_t without overflow (e.g. a huge VArray type).
/// Unlike DataLayout::getTypeAllocSizeInBits, which wraps silently, array
/// counts are multiplied and struct members are summed in checked arithmetic.
/// Struct results are memoized in \p Cache so that nested identified structs
/// cost linear rather than exponential time.
static Optional<TypeSize>
getAllocSizeInBitsChecked(Type *Ty, const DataLayout &DL,
                          DenseMap<StructType *, Optional<TypeSize>> &Cache);

static Optional<TypeSize>
getArrayAllocSizeChecked(ArrayType *ATy, const DataLayout &DL,
                         DenseMap<StructType *, Optional<TypeSize>> &Cache) {
  auto ElementSize =
      getAllocSizeInBitsChecked(ATy->getElementType(), DL, Cache);
  if (!ElementSize)
    return None;
  auto ArraySize =
      checkedMulUnsigned(ATy->getNumElements(), ElementSize->getKnownMinSize());
  if (!ArraySize)
    return None;
  return TypeSize(*ArraySize, ElementSize->isScalable());
}

static Optional<TypeSize> computeStructAllocSizeChecked(
    StructType *STy, const DataLayout &DL,
    DenseMap<StructType *, Optional<TypeSize>> &Cache) {
  // All quantities in this helper are in bits.
  uint64_t StructSize = 0;
  uint64_t StructAlignment = CHAR_BIT;
  for (Type *ElementTy : STy->elements()) {
    auto ElementSize = getAllocSizeInBitsChecked(ElementTy, DL, Cache);
    if (!ElementSize || ElementSize->isScalable())
      return None;
    // Packed members are one-byte aligned.
    uint64_t Alignment = CHAR_BIT;
    if (!STy->isPacked()) {
      // DataLayout reports alignment in bytes, so scale it to bits. Align
      // values are bounded well below 2^61 bytes, so this cannot overflow.
      Alignment = DL.getABITypeAlign(ElementTy).value() * CHAR_BIT;
      StructAlignment = std::max(StructAlignment, Alignment);
    }
    auto AlignedSize =
        checkedAddUnsigned(StructSize, -StructSize & (Alignment - 1));
    if (!AlignedSize)
      return None;
    auto NewSize =
        checkedAddUnsigned(*AlignedSize, ElementSize->getFixedSize());
    if (!NewSize)
      return None;
    StructSize = *NewSize;
  }
  auto PaddedSize =
      checkedAddUnsigned(StructSize, -StructSize & (StructAlignment - 1));
  if (!PaddedSize)
    return None;
  return TypeSize::Fixed(*PaddedSize);
}

static Optional<TypeSize>
getStructAllocSizeChecked(StructType *STy, const DataLayout &DL,
                          DenseMap<StructType *, Optional<TypeSize>> &Cache) {
  auto It = Cache.find(STy);
  if (It == Cache.end())
    It = Cache.insert({STy, computeStructAllocSizeChecked(STy, DL, Cache)})
             .first;
  return It->second;
}

static Optional<TypeSize>
getAllocSizeInBitsChecked(Type *Ty, const DataLayout &DL,
                          DenseMap<StructType *, Optional<TypeSize>> &Cache) {
  if (auto *ATy = dyn_cast<ArrayType>(Ty))
    return getArrayAllocSizeChecked(ATy, DL, Cache);
  if (auto *STy = dyn_cast<StructType>(Ty))
    return getStructAllocSizeChecked(STy, DL, Cache);
  return DL.getTypeAllocSizeInBits(Ty);
}

/// Returns the demangled Cangjie name (e.g. "default::main") of a mangled
/// symbol, or the raw name when demangling yields nothing. Applies to both
/// function and global-variable symbols.
static std::string getDemangledName(StringRef MangledName) {
  auto D = Cangjie::Demangle(MangledName.str());
  std::string DemangledName =
      D.GetPkgName() + std::string(D.GetPkgName().empty() ? "" : "::") +
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
static bool checkAllocationSize(Function &F, const DataLayout &DL,
                                DenseMap<StructType *, Optional<TypeSize>> &Cache) {
  uint64_t FrameBits = 0;
  for (Instruction &I : instructions(F)) {
    auto *AI = dyn_cast<AllocaInst>(&I);
    if (!AI)
      continue;
    // Reject any alloca whose allocated type is not representable in uint64
    // bits, including dynamic allocas whose runtime count is unknown.
    auto AllocBits =
        getAllocSizeInBitsChecked(AI->getAllocatedType(), DL, Cache);
    if (!AllocBits)
      reportOversizedInFunction("The allocation type size exceeds the maximum "
                                "representable size", F);
    // Non-static allocas do not contribute to the static frame size, and
    // scalable allocas are rejected by the generic IR verifier; neither is
    // summed here.
    if (!AI->isStaticAlloca() || AllocBits->isScalable())
      continue;
    // getLimitedValue saturates counts wider than 64 bits, so they overflow
    // the checked multiply for any nonzero type size.
    Optional<uint64_t> NewFrameBits;
    if (auto AllocSizeBits = checkedMulUnsigned(
            AllocBits->getFixedSize(),
            cast<ConstantInt>(AI->getArraySize())->getLimitedValue()))
      NewFrameBits = checkedAddUnsigned(FrameBits, *AllocSizeBits);
    if (!NewFrameBits)
      reportOversizedInFunction("The accumulated stack frame size exceeds the "
                                "maximum representable size", F);
    FrameBits = *NewFrameBits;
  }
  return false;
}

/// Checks a single global variable for an oversized value type and aborts on
/// any overflow in the size computation. A global whose value type size wraps
/// uint64 bits would be laid out with a wrong, smaller extent (e.g. the
/// assembler emits only the wrapped byte count), so reads and writes through
/// it go out of bounds.
static void checkGlobalVariable(
    GlobalVariable &GV, const DataLayout &DL,
    DenseMap<StructType *, Optional<TypeSize>> &Cache) {
  auto ValueBits = getAllocSizeInBitsChecked(GV.getValueType(), DL, Cache);
  if (!ValueBits)
    reportOversizedInGlobal("The value type size exceeds the maximum "
                            "representable size", GV);
}

} // namespace

PreservedAnalyses CJAllocationSizeCheck::run(Module &M,
                                             ModuleAnalysisManager &) const {
  if (hasRunCangjieOpt(M))
    return PreservedAnalyses::all();
  const DataLayout &DL = M.getDataLayout();
  DenseMap<StructType *, Optional<TypeSize>> Cache;
  for (GlobalVariable &GV : M.globals()) {
    if (!GV.hasInitializer())
      continue;
    checkGlobalVariable(GV, DL, Cache);
  }
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    checkAllocationSize(F, DL, Cache);
  }
  return PreservedAnalyses::all();
}
