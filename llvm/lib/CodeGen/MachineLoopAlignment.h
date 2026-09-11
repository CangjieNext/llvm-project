//===- MachineLoopAlignment.h - Loop alignment policy -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_CODEGEN_MACHINELOOPALIGNMENT_H
#define LLVM_LIB_CODEGEN_MACHINELOOPALIGNMENT_H

#include "llvm/ADT/Optional.h"

namespace llvm {

class MachineBasicBlock;
class MachineBranchProbabilityInfo;
class MachineFunction;
class MachineLoop;
class MachineLoopInfo;
class MBFIWrapper;
class ProfileSummaryInfo;
class TargetLoweringBase;

/// Share the profitability checks between ordinary and adaptive alignment.
class MachineLoopAlignment {
  const TargetLoweringBase &TLI;
  MBFIWrapper &MBFI;
  const MachineBranchProbabilityInfo &MBPI;
  ProfileSummaryInfo *ProfileSummary;

public:
  MachineLoopAlignment(const TargetLoweringBase &TLI, MBFIWrapper &MBFI,
                       const MachineBranchProbabilityInfo &MBPI,
                       ProfileSummaryInfo *ProfileSummary);

  bool shouldAlign(MachineBasicBlock &MBB, const MachineLoop &Loop) const;

  /// Apply in the Cangjie pipeline after ordinary alignment and size checks.
  void alignSmallLoops(MachineFunction &MF, MachineLoopInfo &MLI,
                       bool HasExplicitBlockAlignment,
                       Optional<unsigned> MaxBytesOverride) const;
};

} // namespace llvm

#endif
