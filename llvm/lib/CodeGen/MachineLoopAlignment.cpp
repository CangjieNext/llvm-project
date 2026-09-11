//===- MachineLoopAlignment.cpp - Loop alignment policy --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MachineLoopAlignment.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/CodeGen/MBFIWrapper.h"
#include "llvm/CodeGen/MachineBranchProbabilityInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineSizeOpts.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Target/TargetMachine.h"

using namespace llvm;

static cl::opt<bool> CJAdaptiveLoopAlignment(
    "cj-adaptive-loop-alignment",
    cl::desc("Align small AArch64 loops according to their size in the "
             "Cangjie pipeline"),
    cl::init(true), cl::Hidden);

static constexpr unsigned MinLoopAlignment = 16;
static constexpr unsigned MaxLoopSize = 64;
static constexpr unsigned ColdLoopRatio = 5;

MachineLoopAlignment::MachineLoopAlignment(
    const TargetLoweringBase &TLI, MBFIWrapper &MBFI,
    const MachineBranchProbabilityInfo &MBPI, ProfileSummaryInfo *ProfileSummary)
    : TLI(TLI), MBFI(MBFI), MBPI(MBPI), ProfileSummary(ProfileSummary)
{
}

bool MachineLoopAlignment::shouldAlign(MachineBasicBlock &MBB,
                                       const MachineLoop &Loop) const
{
  const BranchProbability ColdProb(1, ColdLoopRatio);
  BlockFrequency Freq = MBFI.getBlockFreq(&MBB);
  BlockFrequency EntryFreq = MBFI.getBlockFreq(&MBB.getParent()->front());
  if (Freq < EntryFreq * ColdProb) {
    return false;
  }
  if (Freq < MBFI.getBlockFreq(Loop.getHeader()) * ColdProb) {
    return false;
  }
  if (llvm::shouldOptimizeForSize(&MBB, ProfileSummary, &MBFI) &&
      !TLI.alignLoopsWithOptSize()) {
    return false;
  }

  MachineBasicBlock *LayoutPred = &*std::prev(MBB.getIterator());
  if (!LayoutPred->isSuccessor(&MBB)) {
    return true;
  }
  BranchProbability LayoutProb = MBPI.getEdgeProbability(LayoutPred, &MBB);
  BlockFrequency LayoutEdgeFreq = MBFI.getBlockFreq(LayoutPred) * LayoutProb;
  return LayoutEdgeFreq <= Freq * ColdProb;
}

/// Include internal block padding after ordinary block alignment has run.
/// Inline assembly can contain directives whose sizes depend on their address.
static Align getSmallLoopAlignment(MachineBasicBlock &Top, MachineLoop &Loop,
                                   const TargetInstrInfo &TII)
{
  unsigned LoopSize = 0;
  unsigned NumBlocks = 0;
  Align MinAlignment(MinLoopAlignment);
  for (auto I = Top.getIterator(), E = Top.getParent()->end();
       I != E && Loop.contains(&*I); ++I) {
    if (&*I != &Top) {
      Align BlockAlignment = I->getAlignment();
      if (BlockAlignment > Align(MaxLoopSize)) {
        return Align(1);
      }
      MinAlignment = std::max(MinAlignment, BlockAlignment);
      LoopSize = alignTo(LoopSize, BlockAlignment);
    }
    for (const MachineInstr &MI : *I) {
      if (MI.isInlineAsm()) {
        return Align(1);
      }
      unsigned InstSize = TII.getInstSizeInBytes(MI);
      if (InstSize > MaxLoopSize - LoopSize) {
        return Align(1);
      }
      LoopSize += InstSize;
    }
    ++NumBlocks;
  }
  // A split loop can contain unrelated blocks in its physical layout.
  if (NumBlocks != Loop.getNumBlocks()) {
    return Align(1);
  }
  return std::max(MinAlignment,
                  Align(PowerOf2Ceil(std::max(MinLoopAlignment, LoopSize))));
}

void MachineLoopAlignment::alignSmallLoops(
    MachineFunction &MF, MachineLoopInfo &MLI, bool HasExplicitBlockAlignment,
    Optional<unsigned> MaxBytesOverride) const
{
  if (!CJAdaptiveLoopAlignment || HasExplicitBlockAlignment ||
      !MF.getTarget().getTargetTriple().isAArch64() ||
      MF.getTarget().Options.LoopAlignment) {
    return;
  }

  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  for (MachineBasicBlock &MBB : drop_begin(MF)) {
    MachineLoop *Loop = MLI.getLoopFor(&MBB);
    if (!Loop || !Loop->isInnermost() ||
        Loop->contains(&*std::prev(MBB.getIterator())) ||
        !shouldAlign(MBB, *Loop)) {
      continue;
    }
    Align Alignment = getSmallLoopAlignment(MBB, *Loop, TII);
    if (Alignment == 1) {
      continue;
    }
    MBB.setAlignment(std::max(MBB.getAlignment(), Alignment));
    MBB.setMaxBytesForAlignment(MaxBytesOverride.value_or(0));
  }
}
