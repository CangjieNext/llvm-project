//===-- CJStackPointerInserter.cpp - Cangjie Insert Stack Pointers---------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file implements the High-performance stack-pointers analysis pass. For
// each statepoint machine instruction in the function, this pass calculates
// the set of stack pointers that are lived after the instruction (i.e., the
// instruction calculates the stack pointer that may be used after the
// instruction, these stack pointers may be in registers or stack slots).
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/CJStackPointerInserter.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include <queue>

using namespace llvm;

char CJStackPointerInserter::ID = 0;
char &llvm::CJStackPointerInserterID = CJStackPointerInserter::ID;
INITIALIZE_PASS_BEGIN(CJStackPointerInserter, "cangjie-stack-pointer-inserter",
                      "Cangjie Stack Pointers Inserter", false, false)
INITIALIZE_PASS_DEPENDENCY(UnreachableMachineBlockElim)
INITIALIZE_PASS_END(CJStackPointerInserter, "cangjie-stack-pointer-inserter",
                    "Cangjie Stack Pointers Inserter", false, false)

namespace {

// Output log information and terminate the program.
void Check(bool Condition, const Twine &Message) {
  if (!Condition) {
    dbgs() << "CJStackPointerInserter-Check: " << Message << "\n";
    report_fatal_error("Broken function found, compilation aborted!");
  }
}

// Return true if is x86, and return false if is aarch64.
// Otherwise an error is reported.
bool isX86Triple(MachineFunction &MF) {
  if (MF.getTarget().getTargetTriple().isX86()) {
    return true;
  } else if (MF.getTarget().getTargetTriple().isAArch64()) {
    return false;
  } else {
    report_fatal_error("Unsupported target type!");
  }
}

// Parse the pointer variable in the input parameter of the function.
void parseFuncArgPointers(MachineFunction &MF, FuncArgPointers &Data) {
  Function &F = MF.getFunction();
  if (F.arg_empty())
    return;

  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  const bool IsWindows = MF.getTarget().getMCAsmInfo()->usesWindowsCFI();
  const std::vector<MCPhysReg> RegList = TRI->getArgRegs(MF);
  const unsigned MaxRegNum = RegList.size();
  unsigned RegIdx = 0;   // the index of RegList
  unsigned FIIdx = 1;    // the Frame index number
  unsigned FPNum = 0;    // floating point number
  unsigned MaxFPArg = 8; // xmm0-xmm7 in x86 and v0-v7 in aarch64
  if (IsWindows) {
    MaxFPArg = 4; // xmm0-xmm3 in windows
  }

  // Record the index of the parameter and exclude floating-point types.
  for (unsigned i = 0; i < F.arg_size(); ++i) {
    Argument *Arg = F.getArg(i);
    if (Arg->hasStructRetAttr()) {
      Check(i == 0, "The sret arg position is not 0.");
      if (isX86Triple(MF)) {
        MCRegister MCReg(RegList[RegIdx]);
        Data.Regs.push_back(MCReg);
        ++RegIdx;
        if (IsWindows) {
          // Windows registers are used based on parameter locations, that is,
          // location 0:rcx/xmm0, 1:rdx/xmm1, 2:r8/xmm2, 3:r9/xmm3
          ++FPNum;
        }
      } else { // AArch64
        MCRegister X8Reg(TRI->getX8Register());
        Data.Regs.push_back(X8Reg);
      }
      continue;
    }

    Type *ArgTy = Arg->getType();
    if (ArgTy->isVectorTy()) {
      auto *VTy = llvm::dyn_cast<llvm::FixedVectorType>(ArgTy);
      uint64_t NumElements = VTy->getNumElements();
      uint64_t ElementBitSize = VTy->getElementType()->getPrimitiveSizeInBits();
      if ((NumElements * ElementBitSize) <= 128) {
        ++FPNum;
        if (FPNum > MaxFPArg) {
          ++FIIdx; // push to stack when more than max float registers.
        } else {
          if (IsWindows) {
            Check(RegIdx < MaxRegNum, "Register Idx caculate error in windows");
            ++RegIdx;
          }
        }
        continue;
      }
    }

    if (ArgTy->isHalfTy() || ArgTy->isFloatTy() || ArgTy->isDoubleTy()) {
      ++FPNum;
      if (FPNum > MaxFPArg) {
        ++FIIdx; // push to stack when more than max float registers.
      } else {
        if (IsWindows) {
          Check(RegIdx < MaxRegNum, "Register Idx caculate error in windows");
          ++RegIdx;
        }
      }
      continue;
    }

    if (ArgTy->isPointerTy()) {
      if (RegIdx < MaxRegNum) {
        MCRegister MCReg(RegList[RegIdx]);
        Data.Regs.push_back(MCReg);
      } else {
        Data.FIs.push_back(-FIIdx);
      }
    }

    // Parameters beyond the register range are stored in stack frames.
    if (RegIdx < MaxRegNum) {
      ++RegIdx;
      if (IsWindows) {
        Check(FPNum < MaxFPArg, "Float Register Idx caculate error in windows");
        ++FPNum;
      }
    } else {
      ++FIIdx;
    }
  }
}

class SPType {
public:
  enum TypeID {
    RegTyID = 0,
    SlotTyID,
  };

  SPType(TypeID ID, int SubclassData) : ID(ID), SubclassData(SubclassData) {}

  TypeID getTypeID() const { return ID; }
  int getSubclassData() const { return SubclassData; }

private:
  TypeID ID : 2;         // The current base type of this type.
  int SubclassData : 30; // Space for subclasses to store data.
};

class RegSPType : public SPType {
public:
  explicit RegSPType(int Reg) : SPType(RegTyID, Reg) {}

  int getReg() { return getSubclassData(); }

  /// Implement support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const SPType *T) { return T->getTypeID() == RegTyID; }
};

class SlotSPType : public SPType {
public:
  SlotSPType(int Data, int Offset) : SPType(SlotTyID, Data), Offset(Offset) {}

  SlotInfo getSlot() const {
    SlotInfo SI(getSubclassData(), Offset);
    return SI;
  }

  /// Implement support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const SPType *T) { return T->getTypeID() == SlotTyID; }

private:
  int Offset = 0;
};

class StackOperand {
public:
  const Triple &Target;

  StackOperand(const Triple &Target) : Target(Target) {}

  // Returns the number of bytes offset from the loaded stack address.
  int getLoadStackOffset(MachineInstr &MI) {
    if (Target.isX86()) {
      // x86: $r0 = MOV64rm %stack, 1, $noreg, 8, $noreg
      return MI.getOperand(4).getImm();
    } else if (Target.isAArch64()) {
      // aarch64: $x0 = LDRXui %stack, 1
      return MI.getOperand(2).getImm();
    }
    Check(false, "Incorrect target type");
    return -1;
  }

  // Returns the number of bytes offset from the stored stack address.
  int getStoreStackOffset(MachineInstr &MI) {
    if (Target.isX86()) {
      // x86: MOV64mr %stack, 1, $noreg, 24, $noreg, $rbx
      return MI.getOperand(3).getImm();
    } else if (Target.isAArch64()) {
      // aarch64: STRXui $x20, %stack, 3
      return MI.getOperand(2).getImm();
    }
    Check(false, "Incorrect target type");
    return -1;
  }

  // The destination register is operand 0 for both x86 and aarch64.
  Register getLoadReg(MachineInstr &MI) { return MI.getOperand(0).getReg(); }

  Register getStoreReg(MachineInstr &MI) {
    if (Target.isX86()) {
      // x86: MOV64mr %stack, 1, $noreg, 24, $noreg, $rbx
      return MI.getOperand(5).getReg();
    } else if (Target.isAArch64()) {
      // aarch64: STRXui $x20, %stack, 3
      return MI.getOperand(0).getReg();
    }
    Check(false, "Incorrect target type");
    return -1;
  }
};

/// Use data flow analysis to calculate the stack-pointer set on statepoint.
class StackPointerAnalysis : public StackOperand {
public:
  StackPointerAnalysis(MachineFunction &MF,
                       SetVector<MachineInstr *> &Statepoints,
                       StatepointLives &Data, FuncArgPointers &ArgPtrs)
      : StackOperand(MF.getTarget().getTargetTriple()), MF(MF),
        TII(MF.getSubtarget().getInstrInfo()),
        TRI(MF.getSubtarget().getRegisterInfo()), Statepoints(Statepoints),
        Data(Data), ArgPtrs(ArgPtrs) {}

  ~StackPointerAnalysis() { clearSPData(); }

  void computeStackPointerMap() {
    // Worklist containing pending BBs
    std::queue<MachineBasicBlock *> Worklist;
    MapVector<MachineBasicBlock *, SetVector<SPType *>> Ins, Outs;

    // Calculate the livein register of each BB.
    computeLiveinRegs();

    // Seed the stack pointer for each individual block
    for (MachineBasicBlock &MBB : MF) {
      if (MBB.isEntryBlock()) {
        // Initialize seeds as input parameters.
        initialSPData(Ins[&MBB]);
        Outs[&MBB] = Ins[&MBB];
      } else {
        Ins[&MBB].clear();
        Outs[&MBB].clear();
      }

      transferSPData(MBB, Outs[&MBB]);
      if (!Outs[&MBB].empty()) {
        for (auto *Succ : MBB.successors()) {
          Worklist.push(Succ);
        }
      }
    }

    // Iterate until stable
    while (!Worklist.empty()) {
      MachineBasicBlock *MBB = Worklist.front();
      Worklist.pop();
      SetVector<SPType *> Tmp = Ins[MBB];
      const auto OldSize = Tmp.size();
      for (auto *Pred : MBB->predecessors()) {
        Tmp.set_union(Outs[Pred]);
      }

      // assert: OldLiveIn is a subset of NewLiveIn
      if (Tmp.size() == OldSize) {
        // If the sets are the same size, then we didn't actually add anything
        // when unioning our successors SPSet.  Thus, the SPSet of this block
        // hasn't changed.
        continue;
      }

      transferSPData(*MBB, Tmp);
      // assert: OldLiveOut is a subset of NewLiveOut
      if (Outs[MBB].size() != Tmp.size()) {
        Outs[MBB] = Tmp;
        for (auto *Succ : MBB->successors()) {
          Worklist.push(Succ);
        }
      }
    }
  }

  void transferSPData(MachineBasicBlock &MBB, SetVector<SPType *> &Tmp) {
    updateLiveInRegs(MBB, Tmp);

    for (MachineInstr &MI : MBB) {
      if (MI.isDebugOrPseudoInstr())
        continue;

      if (Statepoints.contains(&MI)) {
        StatepointOpers SI(&MI);
        if (SI.isCJStackCheck())
          continue;

        updateSPDataInStatepoint(&MI, Tmp);
        removeDeadReg(MI, Tmp);
        continue;
      }

      int FI = 0;
      unsigned MemBytes = 0;
      if (TII->isStoreToStackSlot(MI, FI, MemBytes) && MemBytes == 8) {
        Register SrcReg = getStoreReg(MI);
        if (Tmp.contains(getSPData(SrcReg))) {
          int Imm = getStoreStackOffset(MI);
          Tmp.insert(getSPData(FI, Imm, false));
        }
      } else if (TII->isLoadFromStackSlot(MI, FI, MemBytes) && MemBytes == 8) {
        int Imm = getLoadStackOffset(MI);
        if (Tmp.contains(getSPData(FI, Imm, false))) {
          Register DstReg = getLoadReg(MI);
          Tmp.insert(getSPData(DstReg));
        }
      } else if (TII->isLEA64r(MI) || TII->isADDXri(MI) || TII->isADDXrr(MI) ||
                 TII->isORRXri(MI)) {
        MachineOperand &RegMO = MI.getOperand(0);
        MachineOperand &SrcMO = MI.getOperand(1);
        if (SrcMO.isFI()) {
          Tmp.insert(getSPData(RegMO.getReg()));
        } else {
          assert(SrcMO.isReg() && "Incorrect MI operand!");
          if (Tmp.contains(getSPData(SrcMO.getReg()))) {
            Tmp.insert(getSPData(RegMO.getReg()));
          }
        }
      } else if (MI.isCopy()) {
        // $r12 = COPY renamable $rbx
        Register SrcReg = MI.getOperand(1).getReg();
        if (Tmp.contains(getSPData(SrcReg))) {
          Tmp.insert(getSPData(MI.getOperand(0).getReg()));
        }
      }

      removeDeadReg(MI, Tmp);
    }
  }

private:
  MachineFunction &MF;
  const TargetInstrInfo *TII;
  const TargetRegisterInfo *TRI;
  SetVector<MachineInstr *> &Statepoints;
  StatepointLives &Data;
  FuncArgPointers &ArgPtrs;
  DenseMap<MachineBasicBlock *, SmallSet<SPType *, 16>> LiveinRegs;

  // Cache All stack pointer data and clear it later.
  SmallVector<SPType *, 128> CacheSPData;

  // Keeps track of the offsets of stack slot.
  DenseMap<int, SPType *> SPRegsMap;
  DenseMap<SlotInfo, SPType *> SPSlotsMap;

  SPType *getSPData(int Data, int Offset = 0, bool isReg = true) {
    if (isReg) {
      if (SPRegsMap.count(Data)) {
        return SPRegsMap[Data];
      }

      RegSPType *RegSP = new RegSPType(Data);
      CacheSPData.push_back(RegSP);
      SPRegsMap[Data] = RegSP;
      return RegSP;
    } else {
      // stack slot
      SlotInfo SI(Data, Offset);
      if (SPSlotsMap.count(SI)) {
        return SPSlotsMap[SI];
      }

      SlotSPType *SlotSP = new SlotSPType(Data, Offset);
      CacheSPData.push_back(SlotSP);
      SPSlotsMap[SI] = SlotSP;
      return SlotSP;
    }
  }

  void clearSPData() {
    for (auto SP : CacheSPData) {
      delete SP;
    }
    CacheSPData.clear();
    SPRegsMap.clear();
    SPSlotsMap.clear();
  }

  // The SP data is initialized to the pointer parameter of the function.
  void initialSPData(SetVector<SPType *> &In) {
    for (int Reg : ArgPtrs.Regs) {
      SPType *SP = getSPData(Reg);
      In.insert(SP);
    }
    for (int FI : ArgPtrs.FIs) {
      SPType *SP = getSPData(FI, 0, false);
      In.insert(SP);
    }
  }

  void computeLiveinRegs() {
    for (MachineBasicBlock &MBB : MF) {
      for (const auto &LI : MBB.liveins()) {
        assert(Register::isPhysicalRegister(LI.PhysReg) &&
               "Cannot have a live-in virtual register!");
        const TargetRegisterClass *RC = TRI->getMinimalPhysRegClass(LI.PhysReg);
        unsigned RegSize = TRI->getRegSizeInBits(*RC);
        if (RegSize == 64) { // 64: Register bit width
          SPType *SP = getSPData(LI.PhysReg);
          LiveinRegs[&MBB].insert(SP);
        }
      }
    }
  }

  void removeDeadReg(MachineInstr &MI, SetVector<SPType *> &Tmp) {
    SetVector<SPType *> KeepRegs;
    for (unsigned i = 0; i < MI.getNumOperands(); ++i) {
      MachineOperand &MO = MI.getOperand(i);
      if (MO.isRegMask())
        continue;

      if (!MO.isReg() || MO.getReg() == 0)
        continue;

      Register MOReg = MO.getReg();
      assert(Register::isPhysicalRegister(MOReg) &&
             "Cannot be a virtual register in Operand!");
      SPType *SP = getSPData(MOReg);
      // If reg is used for target reg in the instruction, keep it.
      // for example: renamable $rax = LEA64r killed renamable $rax
      if (MO.isKill()) {
        if (KeepRegs.contains(SP)) {
          continue;
        } else {
          Tmp.remove(SP);
        }
      } else {
        KeepRegs.insert(SP);
      }
    }
  }

  // Filter out non-livein registers in SPSet.
  void updateLiveInRegs(MachineBasicBlock &MBB, SetVector<SPType *> &SPSet) {
    for (auto It = SPSet.begin(); It != SPSet.end();) {
      if (auto *RegSP = dyn_cast<RegSPType>(*It)) {
        if (!LiveinRegs[&MBB].contains(RegSP)) {
          It = SPSet.erase(It);
          continue;
        }
      }
      ++It;
    }
  }

  void updateSPDataInStatepoint(MachineInstr *MI, SetVector<SPType *> &SPSet) {
    for (SPType *SP : SPSet) {
      if (auto *SPReg = dyn_cast<RegSPType>(SP)) {
        Data[MI].Regs.insert(SPReg->getReg());
      } else {
        auto *SPSlot = dyn_cast<SlotSPType>(SP);
        assert(SPSlot && "Incorrect SP data.");
        Data[MI].Slots.insert(SPSlot->getSlot());
      }
    }
  }
};

class StackLives : public StackOperand {
public:
  // Represent the live analysis data for stacks.
  MapVector<MachineBasicBlock *, SetVector<SlotInfo>> KillSet;
  MapVector<MachineBasicBlock *, SetVector<SlotInfo>> LiveSet;
  MapVector<MachineBasicBlock *, SetVector<SlotInfo>> LiveIn;
  MapVector<MachineBasicBlock *, SetVector<SlotInfo>> LiveOut;

  StackLives(MachineFunction &MF, FuncArgPointers &ArgPtrs)
      : StackOperand(MF.getTarget().getTargetTriple()), MF(MF),
        TII(MF.getSubtarget().getInstrInfo()), MFI(MF.getFrameInfo()),
        TRI(MF.getSubtarget().getRegisterInfo()), MRI(MF.getRegInfo()),
        DL(MF.getDataLayout()), ArgPtrs(ArgPtrs) {}

  void computeStackKillSet(MachineBasicBlock *MBB) {
    for (auto &MI : *MBB) {
      int FI;
      unsigned MemBytes;
      if (TII->isStoreToStackSlot(MI, FI, MemBytes) && MemBytes == 8) {
        if (MFI.getObjectAllocation(FI))
          continue;

        if (MFI.isSpillSlotObjectIndex(FI)) {
          // The spill is a define point of the stack pointer.
          int Imm = getStoreStackOffset(MI);
          insertLiveSet(KillSet[MBB], FI, Imm);
          continue;
        }
        assert(MFI.isStatepointSpillSlotObjectIndex(FI) &&
               "Unknown stack type");
      }
    }
  }

  void computeStackLiveIn(MachineBasicBlock::reverse_iterator Begin,
                          MachineBasicBlock::reverse_iterator End,
                          SetVector<SlotInfo> &LiveTmp) {
    for (auto &MI : make_range(Begin, End)) {
      int FI;
      unsigned MemBytes;

      if (TII->isLoadFromStackSlot(MI, FI, MemBytes) && MemBytes == 8) {
        int Imm = getLoadStackOffset(MI);
        const AllocaInst *AI = MFI.getObjectAllocation(FI);
        if (AI != nullptr) {
          assert(MFI.isAliasedObjectIndex(FI));
          if (isPointerByLocalStack(AI, Imm)) {
            insertLiveSet(LiveTmp, FI, Imm);
          }
          continue;
        }
        if (MFI.isFixedObjectIndex(FI)) {
          if (isPointerByFixedObject(FI)) {
            insertLiveSet(LiveTmp, FI, Imm);
          }
          continue;
        }
        if (MFI.isSpillSlotObjectIndex(FI)) {
          // The reload is a use point of the stack pointer.
          insertLiveSet(LiveTmp, FI, Imm);
          continue;
        }
        assert(MFI.isStatepointSpillSlotObjectIndex(FI) &&
               "Unknown stack type");
        continue;
      }

      if (TII->isStoreToStackSlot(MI, FI, MemBytes) && MemBytes == 8) {
        int Imm = getStoreStackOffset(MI);
        const AllocaInst *AI = MFI.getObjectAllocation(FI);
        if (AI != nullptr) {
          assert(MFI.isAliasedObjectIndex(FI));
          if (isPointerByLocalStack(AI, Imm)) {
            insertLiveSet(LiveTmp, FI, Imm);
          }
          continue;
        }
        if (MFI.isFixedObjectIndex(FI)) {
          if (isPointerByFixedObject(FI)) {
            insertLiveSet(LiveTmp, FI, Imm);
          }
          continue;
        }
        if (MFI.isSpillSlotObjectIndex(FI)) {
          // The spill is a define point of the stack pointer.
          removeLiveSet(LiveTmp, FI, Imm);
          continue;
        }
        assert(MFI.isStatepointSpillSlotObjectIndex(FI) &&
               "Unknown stack type");
      }
    }
  }

  void computeStackLiveness() {
    SmallSetVector<MachineBasicBlock *, 32> Worklist;

    // Seed the liveness for each individual block
    for (MachineBasicBlock &MBBs : MF) {
      MachineBasicBlock *MBB = &MBBs;
      KillSet[MBB].clear();
      LiveSet[MBB].clear();
      LiveOut[MBB].clear();
      computeStackKillSet(MBB);
      computeStackLiveIn(MBB->rbegin(), MBB->rend(), LiveSet[MBB]);

      LiveIn[MBB] = LiveOut[MBB];
      LiveIn[MBB].set_subtract(KillSet[MBB]);
      LiveIn[MBB].set_union(LiveSet[MBB]);
      if (!LiveIn[MBB].empty())
        Worklist.insert(MBB->pred_begin(), MBB->pred_end());
    }

    // Propagate that liveness until stable
    while (!Worklist.empty()) {
      MachineBasicBlock *MBB = Worklist.pop_back_val();

      // Compute our new liveout set, then exit early if it hasn't changed
      // despite the contribution of our successor.
      SetVector<SlotInfo> TmpLiveOut = LiveOut[MBB];
      const auto OldLiveOutSize = TmpLiveOut.size();
      for (MachineBasicBlock *Succ : MBB->successors()) {
        assert(LiveIn.count(Succ));
        TmpLiveOut.set_union(LiveIn[Succ]);
      }

      // assert OutLiveOut is a subset of LiveOut
      if (OldLiveOutSize == TmpLiveOut.size()) {
        // If the sets are the same size, then we didn't actually add anything
        // when unioning our successors LiveIn.  Thus, the LiveIn of this block
        // hasn't changed.
        continue;
      }
      LiveOut[MBB] = TmpLiveOut;

      // Apply the effects of this basic block
      SetVector<SlotInfo> LiveTmp = TmpLiveOut;
      LiveTmp.set_subtract(KillSet[MBB]);
      LiveTmp.set_union(LiveSet[MBB]);

      assert(LiveIn.count(MBB));
      SetVector<SlotInfo> &OldLiveIn = LiveIn[MBB];
      // assert: OldLiveIn is a subset of LiveTmp
      if (OldLiveIn.size() != LiveTmp.size()) {
        LiveIn[MBB] = LiveTmp;
        Worklist.insert(MBB->pred_begin(), MBB->pred_end());
      }
    }
  }

  void analyzeStatepointLiveness(MachineInstr *MI, CJStackLives &Lives) {
    MachineBasicBlock *MBB = MI->getParent();

    // Note: The copy is intentional and required
    assert(LiveOut.count(MBB));
    SetVector<SlotInfo> LiveTmp = LiveOut[MBB];

    computeStackLiveIn(MBB->rbegin(), ++MI->getIterator().getReverse(),
                       LiveTmp);

    // Records the alive stack pointer.
    Lives.Slots.remove_if([&](SlotInfo V) { return !LiveTmp.contains(V); });
  }

private:
  MachineFunction &MF;
  const TargetInstrInfo *TII;
  const MachineFrameInfo &MFI;
  const TargetRegisterInfo *TRI;
  const MachineRegisterInfo &MRI;
  const DataLayout &DL;
  FuncArgPointers &ArgPtrs;

  void insertLiveSet(SetVector<SlotInfo> &LiveTmp, int FI, int Offset) {
    assert(FI < MFI.getObjectIndexEnd() && "The FI exceeds the object range.");
    LiveTmp.insert(std::make_pair(FI, Offset));
  }

  void removeLiveSet(SetVector<SlotInfo> &LiveTmp, int FI, int Offset) {
    LiveTmp.remove(std::make_pair(FI, Offset));
  }

  bool isPointerInArray(ArrayType *AT, const int64_t Imm, int64_t CurPos = 0) {
    uint64_t Num = AT->getNumElements();
    Type *ElementType = AT->getElementType();
    if (ElementType->isPointerTy()) {
      unsigned ArraySize = DL.getPointerSize() * Num;
      Check(ArraySize + CurPos > Imm, "The Imm exceeds the array range.");
      return true;
    } else if (StructType *EST = dyn_cast<StructType>(ElementType)) {
      unsigned StructSize = DL.getStructLayout(EST)->getSizeInBytes();
      unsigned ArraySize = StructSize * Num;
      Check(ArraySize + CurPos > Imm, "The Imm exceeds the array range.");
      int Offset = CurPos;
      while (Offset + StructSize <= Imm) {
        Offset += StructSize;
      }
      return isPointerInStruct(EST, Imm, Offset);
    }
    return false;
  }

  // Calculate the location of the field where the imm is located by greedy.
  bool isPointerInStruct(StructType *ST, const int64_t Imm,
                         int64_t CurPos = 0) {
    const StructLayout *Layout = DL.getStructLayout(ST);
    unsigned StructSize = Layout->getSizeInBytes();
    Check(StructSize + CurPos > Imm, "The Imm exceeds the struct range.");
    unsigned Cur = 0;
    unsigned Next = Cur + 1;
    for (; Cur < ST->getNumElements(); Cur++, Next++) {
      if (Next < ST->getNumElements()) {
        int NextOffset = Layout->getElementOffset(Next) + CurPos;
        if (NextOffset <= Imm) {
          continue;
        }
      }

      int Offset = Layout->getElementOffset(Cur) + CurPos;
      Type *ET = ST->getElementType(Cur);
      if (ET->isPointerTy()) {
        Check(Offset == Imm, "Incorrect offset calculation");
        return true;
      } else if (StructType *EST = dyn_cast<StructType>(ET)) {
        return isPointerInStruct(EST, Imm, Offset);
      } else if (ArrayType *EAT = dyn_cast<ArrayType>(ET)) {
        return isPointerInArray(EAT, Imm, Offset);
      } else {
        return false;
      }
    }
    Check(false, "Incorrect offset calculation");
    return false;
  }

  bool isPointerByLocalStack(const AllocaInst *AI, int Imm) {
    int Offset = Target.isX86() ? Imm : Imm * 8; // 8: pointer size.
    if (auto *ST = dyn_cast<StructType>(AI->getAllocatedType())) {
      return isPointerInStruct(ST, Offset);
    } else if (auto *AT = dyn_cast<ArrayType>(AI->getAllocatedType())) {
      return isPointerInArray(AT, Offset);
    } else if (AI->getAllocatedType()->isPointerTy()) {
      return true;
    }
    return false;
  }

  bool isPointerByFixedObject(int Object) {
    for (int FI : ArgPtrs.FIs) {
      if (FI == Object)
        return true;
    }
    return false;
  }
};
} // namespace

void CJStackPointerInserter::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequiredID(UnreachableMachineBlockElimID);
  AU.addRequired<MachineDominatorTree>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

bool CJStackPointerInserter::needRewriteCall(MachineInstr &MI) {
  if (!isStatepointOpcode(MI.getOpcode()))
    return false;

  StatepointOpers SO(&MI);
  const Function *CalledFunc = SO.getCalledFunction();
  if (CalledFunc && (CalledFunc->hasFnAttribute("gc-safepoint") ||
                     CalledFunc->hasFnAttribute("cj-runtime"))) {
    return false;
  }

  return true;
}

void CJStackPointerInserter::filterGCPointer(
    MachineInstr &MI, StatepointOpers &SO, SmallSetVector<unsigned, 16> &Regs,
    SmallSetVector<SlotInfo, 16> &Slots) {
  unsigned GCPtrNumIdx = SO.getNumGCPtrIdx();
  unsigned GCPtrNum = MI.getOperand(GCPtrNumIdx).getImm();
  if (GCPtrNum == 0) {
    return;
  }

  unsigned GCPtrIdx = (unsigned)SO.getFirstGCPtrIdx();
  assert((int)GCPtrIdx != -1);
  while (GCPtrNum--) {
    MachineOperand &MO = MI.getOperand(GCPtrIdx);
    if (MO.isReg() && MO.getReg() != 0) {
      if (Regs.contains(MO.getReg())) {
        Regs.remove(MO.getReg());
      }
    } else {
      assert(MO.isImm() && "MO is not a Imm when parse gc stack slot!");
      assert(MO.getImm() == StackMaps::IndirectMemRefOp &&
             "MO is not a IndirectMemRefOp type when parse gc stack slot!");
      int FI = MI.getOperand(GCPtrIdx + 2).getIndex();   // 2: FI index
      int Offset = MI.getOperand(GCPtrIdx + 3).getImm(); // 3: Offset
      SlotInfo StackPair = std::make_pair(FI, Offset);
      if (Slots.contains(StackPair)) {
        Slots.remove(StackPair);
      }
    }

    GCPtrIdx = StackMaps::getNextMetaArgIdx(&MI, GCPtrIdx);
  }
}

void CJStackPointerInserter::recordArgStackPtrs(
    SmallSetVector<unsigned, 16> &Regs, SmallSetVector<SlotInfo, 16> &Slots,
    FuncArgPointers &ArgPtrs) {
  if (!ArgPtrs.Regs.empty()) {
    Regs.insert(ArgPtrs.Regs.begin(), ArgPtrs.Regs.end());
  }

  if (!ArgPtrs.FIs.empty()) {
    for (int FI : ArgPtrs.FIs) {
      Slots.insert(std::make_pair(FI, 0));
    }
  }
}

bool CJStackPointerInserter::rewriteStatepoint(MachineFunction &MF,
                                               MachineInstr &MI,
                                               CJStackLives &StackLives,
                                               FuncArgPointers &ArgPtrs) {
  SmallSetVector<unsigned, 16> SPRegs;
  SmallSetVector<SlotInfo, 16> SPSlots;
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();

  StatepointOpers SO(&MI);
  if (SO.isCJStackCheck()) {
    recordArgStackPtrs(SPRegs, SPSlots, ArgPtrs);
    if (SPRegs.empty() && SPSlots.empty()) {
      return false; // not need rewrite
    }
  } else {
    if (StackLives.empty()) { // not need rewrite
      return false;
    }

    for (unsigned Reg : StackLives.Regs) {
      if (TRI->isCalleeSavedPhysReg(Reg, MF)) {
        SPRegs.insert(Reg);
      }
    }
    SPSlots.insert(StackLives.Slots.begin(), StackLives.Slots.end());
  }

  // Filter the GC pointer.
  filterGCPointer(MI, SO, SPRegs, SPSlots);
  int Num = SPRegs.size() + SPSlots.size();
  if (Num == 0) {
    return false;
  }

  unsigned NumIdx = SO.getNumStackPtrsIdx();
  assert(MI.getOperand(NumIdx).getImm() == 0 &&
         "The num of stack pointers should be 0!");
  MachineFrameInfo &MFI = MF.getFrameInfo();
  MachineInstrBuilder MIB = BuildMI(MF, MI.getDebugLoc(), MI.getDesc());

  // Inherit previous memory operands.
  MIB.cloneMemRefs(MI);
  for (unsigned i = 0; i < MI.getNumOperands(); ++i) {
    MachineOperand &MO = MI.getOperand(i);
    if (i == NumIdx) {
      MIB.addImm(Num);
      for (unsigned j = 0; j < SPRegs.size(); ++j) {
        MIB.addReg(SPRegs[j]);
      }
      for (unsigned j = 0; j < SPSlots.size(); ++j) {
        MIB.addImm(StackMaps::IndirectMemRefOp);
        MIB.addImm(MFI.getObjectSize(SPSlots[j].first));
        MIB.addFrameIndex(SPSlots[j].first);
        MIB.addImm(SPSlots[j].second);
      }
      continue;
    }

    if (!MO.isFI()) {
      // Index of Def operand this Use it tied to.
      // Since Defs are coming before Uses, if Use is tied, then
      // index of Def must be smaller that index of that Use.
      // Also, Defs preserve their position in new MI.
      unsigned TiedTo = i;
      if (MO.isReg() && MO.isTied())
        TiedTo = MI.findTiedOperandIdx(i);
      MIB.add(MO);
      if (TiedTo < i)
        MIB->tieOperands(TiedTo, MIB->getNumOperands() - 1);
    } else {
      MIB.add(MO);
    }
  }
  // STATEPOINT_TAIL_CALL is a terminator (isTerminator/isReturn) and is the
  // last instruction in its block, so MI.getNextNode() is null. Insert the
  // rewritten MI before MI; MI itself is erased later, leaving the rewritten
  // MI in MI's original position. For a regular STATEPOINT (non-terminator)
  // we preserve the original "insert after MI" behavior.
  MachineBasicBlock::iterator InsertPt;
  if (MI.isTerminator())
    InsertPt = MI.getIterator();
  else
    InsertPt = MI.getNextNode();
  MI.getParent()->insert(InsertPt, MIB);
  return true;
}

bool CJStackPointerInserter::runOnMachineFunction(MachineFunction &MF) {
  bool Changed = false;
  if (!MF.getFunction().hasCangjieGC())
    return Changed;

  // Gather all the MBBs that statepoints need to rewritten.
  SetVector<MachineInstr *> Statepoints;
  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &I : MBB) {
      if (needRewriteCall(I)) {
        Statepoints.insert(&I);
      }
    }
  }

  if (Statepoints.empty())
    return Changed;

  // This variable may not be empty and needs to be cleaned.
  SmallVector<MachineInstr *, 64> RemoveMI;
  FuncArgPointers ArgPtrs;
  parseFuncArgPointers(MF, ArgPtrs);

  // Records the stack live variable set of each statepoint.
  StatepointLives LiveData;
  StackPointerAnalysis SPA(MF, Statepoints, LiveData, ArgPtrs);
  SPA.computeStackPointerMap();

  StackLives Live(MF, ArgPtrs);
  Live.computeStackLiveness();
  for (auto *MI : Statepoints) {
    Live.analyzeStatepointLiveness(MI, LiveData[MI]);
    if (rewriteStatepoint(MF, *MI, LiveData[MI], ArgPtrs)) {
      RemoveMI.push_back(MI);
    }
  }

  if (RemoveMI.size()) {
    for (auto MI : RemoveMI) {
      MI->eraseFromParent();
    }
    Changed = true;
  }

  return Changed;
}
