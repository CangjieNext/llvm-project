//===- CangjieStackSizeCheck.h - Cangjie 2GB stack frame check -*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Shared implementation of the Cangjie 2GB stack frame limit, enforced by the
// X86, AArch64 and ARM backends when the Cangjie pipeline is enabled.
//
// MachineFrameInfo::getStackSize() accumulates in uint64 and can wrap to a
// small value when several huge objects share one frame (e.g. a handful of
// near-2^61-byte VArrays), silently defeating a plain size comparison. The
// check therefore cross-validates the reported size with a checked sum of the
// individual object sizes whenever a wrap is possible.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_CANGJIESTACKSIZECHECK_H
#define LLVM_CODEGEN_CANGJIESTACKSIZECHECK_H

#include "CangjieDemangle.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/Twine.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Support/CheckedArithmetic.h"
#include "llvm/Support/ErrorHandling.h"

namespace llvm {

/// The Cangjie stack frame limit (2GB), an implementation restriction held by
/// the backends rather than a language limit.
constexpr uint64_t CangjieMaxStackSize = 2ULL * 1024 * 1024 * 1024;

/// Reports the Cangjie stack size diagnostic for \p MF with the demangled
/// function name and aborts compilation.
[[noreturn]] inline void
reportCangjieStackSizeExceeded(const MachineFunction &MF) {
  auto D = Cangjie::Demangle(MF.getName().str());
  std::string DemangledName =
      D.GetPkgName() + std::string(D.GetPkgName().empty() ? "" : "::") +
      D.GetFullName();
  report_fatal_error("The stacksize of " + Twine(DemangledName) +
                         " exceeds cangjie max stacksize(2GB).\nCompilation "
                         "stopped! Please check the implementation of " +
                         Twine(DemangledName) + "!",
                     false);
}

/// Returns the sum of every stack object's size in checked arithmetic (a
/// lower bound of the true frame size), or None if the sum overflows uint64.
/// Variable-sized (dynamic alloca) objects contribute their static unit size
/// only: their runtime count is unknown, and the IR-level
/// CJAllocationSizeCheck already guarantees their allocated type is
/// representable.
inline Optional<uint64_t>
getCheckedCangjieStackObjectSizeSum(const MachineFrameInfo &MFI) {
  uint64_t Total = 0;
  for (int I = MFI.getObjectIndexBegin(), E = MFI.getObjectIndexEnd(); I != E;
       ++I) {
    int64_t Size = MFI.getObjectSize(I);
    if (Size <= 0)
      continue;
    if (auto NewTotal = checkedAddUnsigned(Total, static_cast<uint64_t>(Size)))
      Total = *NewTotal;
    else
      return None;
  }
  return Total;
}

/// Enforces the Cangjie 2GB stack frame limit for \p MF. \p ReportedSize is
/// the frame size the backend already computed through the existing interface
/// (MFI.getStackSize(), possibly target-adjusted), so the common cases exit
/// early without walking objects:
///
///  * Fast path, reject: ReportedSize >= 2GB (unchanged current behaviour).
///  * Fast path, pass: too few objects for a wrap. The uint64 accumulation
///    can only wrap past 2^64 bytes; in the Cangjie pipeline every object is
///    smaller than 2^61 bytes (the frontend caps VArray counts at Int64 max
///    and CJAllocationSizeCheck rejects larger object types), so a wrap needs
///    at least 9 objects.
///  * Slow path: cross-check with the checked object-size sum.
inline void checkCangjieStackSize(const MachineFunction &MF,
                                  uint64_t ReportedSize) {
  if (ReportedSize >= CangjieMaxStackSize)
    reportCangjieStackSizeExceeded(MF);
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  if (MFI.getObjectIndexEnd() - MFI.getObjectIndexBegin() <= 8)
    return;
  Optional<uint64_t> Sum = getCheckedCangjieStackObjectSizeSum(MFI);
  if (!Sum || *Sum >= CangjieMaxStackSize)
    reportCangjieStackSizeExceeded(MF);
}

} // end namespace llvm

#endif // LLVM_CODEGEN_CANGJIESTACKSIZECHECK_H
