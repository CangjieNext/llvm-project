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

/// Enforces the Cangjie 2GB stack frame limit for \p MF. \p ReportedSize is
/// the frame size the backend already computed through the existing interface
/// (MFI.getStackSize(), possibly target-adjusted).
///  * Fast path: ReportedSize >= 2GB rejects immediately (unchanged current
///    behaviour).
///  * Otherwise the reported size could be a wrapped value: the uint64
///    StackSize accumulation wraps past 2^64 bytes when several huge objects
///    share one frame. Re-sum the individual object sizes in checked
///    arithmetic, exiting as soon as the running total reaches 2GB or the
///    addition overflows.
/// Raw object sizes are summed without alignment padding, so the total is a
/// lower bound of the true frame size: padding can only make the real frame
/// larger, hence rejecting on this sum has no false positives. Variable-sized
/// (dynamic alloca) objects contribute their static unit size only: their
/// runtime count is unknown, and the IR-level CJAllocationSizeCheck already
/// guarantees their allocated type is representable.
inline void checkCangjieStackSize(const MachineFunction &MF,
                                  uint64_t ReportedSize) {
  if (ReportedSize >= CangjieMaxStackSize)
    reportCangjieStackSizeExceeded(MF);
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  uint64_t Total = 0;
  for (int I = MFI.getObjectIndexBegin(), E = MFI.getObjectIndexEnd(); I != E;
       ++I) {
    int64_t Size = MFI.getObjectSize(I);
    if (Size <= 0)
      continue;
    auto NewTotal = checkedAddUnsigned(Total, static_cast<uint64_t>(Size));
    if (!NewTotal || *NewTotal >= CangjieMaxStackSize)
      reportCangjieStackSizeExceeded(MF);
    Total = *NewTotal;
  }
}

} // end namespace llvm

#endif // LLVM_CODEGEN_CANGJIESTACKSIZECHECK_H
