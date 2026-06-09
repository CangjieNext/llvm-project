//===- StackMaps.cpp ------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/StackMaps.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/GCMetadata.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/StackMapEncode.h"
#include "llvm/CodeGen/TargetFrameLowering.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Statepoint.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

using namespace llvm;

#define DEBUG_TYPE "stackmaps"

static cl::opt<int> StackMapVersion(
    "stackmap-version", cl::init(3), cl::Hidden,
    cl::desc("Specify the stackmap encoding version (default = 3)"));

static cl::opt<bool> EnableCompressedBitMap(
    "enable-compressed-bitmap", cl::init(true), cl::Hidden,
    cl::desc("Enable Compressed BitMap"));
namespace llvm {
// struct start address alignment
int32_t OffsetStepSize = 8;
int32_t FuncPtrSize = 8;

extern cl::opt<bool> CJPipeline;
extern cl::opt<bool> EnableStackGrow;
} // namespace llvm
const char *StackMaps::WSMP = "Stack Maps: ";
namespace {
enum CJStackMapFormat : uint64_t {
  CJ_STACKMAP_BITMAP = 0,
  CJ_STACKMAP_COMPRESSED_BITMAP = 1
};
const int32_t RawDataWidth = 31; // for compressed stack map

// unordered_map<regNo, bitIdx>, regNo can be used to find callee saved reg
// while bitIdx is used in prologue
const std::unordered_map<uint32_t, uint32_t> X86CalleeSavedReg = {
    // rbx, r12 - r15
    {3, 1 << 0}, {12, 1 << 1}, {13, 1 << 2}, {14, 1 << 3}, {15, 1 << 4},
};
const std::vector<std::string> X86PrologueBit2Reg = {"rbx", "r12", "r13", "r14",
                                                     "r15"};
const std::unordered_map<uint32_t, uint32_t> X86WinCalleeSavedReg = {
    // rbx, rsi, rdi, r12 - r15, xmm6 - xmm15
    {3, 1 << 0},   {4, 1 << 1},   {5, 1 << 2},   {12, 1 << 3},  {13, 1 << 4},
    {14, 1 << 5},  {15, 1 << 6},  {23, 1 << 7},  {24, 1 << 8},  {25, 1 << 9},
    {26, 1 << 10}, {27, 1 << 11}, {28, 1 << 12}, {29, 1 << 13}, {30, 1 << 14},
    {31, 1 << 15}, {32, 1 << 16},
};
const std::vector<std::string> X86WinPrologueBit2Reg = {
    "rbx",  "rsi",  "rdi",   "r12",   "r13",   "r14",   "r15",   "xmm6", "xmm7",
    "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"};
const std::unordered_map<uint32_t, uint32_t> AArch64CalleeSavedReg = {
    // x19 - x30
    {19, 1 << 0},
    {20, 1 << 1},
    {21, 1 << 2},
    {22, 1 << 3},
    {23, 1 << 4},
    {24, 1 << 5},
    {25, 1 << 6},
    {26, 1 << 7},
    {27, 1 << 8},
    {28, 1 << 9},
    {29, 1 << 10},
    {30, 1 << 11},
    // D8 - D15
    {72, 1 << 12},
    {73, 1 << 13},
    {74, 1 << 14},
    {75, 1 << 15},
    {76, 1 << 16},
    {77, 1 << 17},
    {78, 1 << 18},
    {79, 1 << 19},
};
const std::vector<std::string> AArch64PrologueBit2Reg = {
    "x19", "x20", "x21", "x22",     "x23",     "x24", "x25",
    "x26", "x27", "x28", "x29(fp)", "x30(lr)", "d8",  "d9",
    "d10", "d11", "d12", "d13",     "d14",     "d15",
};

const std::vector<std::string> X86Bit2Reg = {
    "rax",   "rdx",   "rcx",   "rbx",
    "rsi",   "rdi",   "rbp",   "rsp", // 0-7
    "r8",    "r9",    "r10",   "r11",
    "r12",   "r13",   "r14",   "r15", // 8-15
    "rip",   "xmm0",  "xmm1",  "xmm2",
    "xmm3",  "xmm4",  "xmm5",  "xmm6", // 16-23
    "xmm7",  "xmm8",  "xmm9",  "xmm10",
    "xmm11", "xmm12", "xmm13", "xmm14", // 24-31
    "xmm15"};
const std::vector<std::string> AArch64Bit2Reg = {
    "x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",  "x9",  "x10",
    "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19", "x20", "x21",
    "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x29", "x30", "x31"};
const std::unordered_map<uint32_t, uint32_t> ARMCalleeSavedReg = {
    // r4 - r11, r14
    {4, 1 << 0},
    {5, 1 << 1},
    {6, 1 << 2},
    {7, 1 << 3},
    {8, 1 << 4},
    {9, 1 << 5},
    {10, 1 << 6},
    {11, 1 << 7},
    {14, 1 << 8},
    // d8 - d15
    {264, 1 << 9},
    {265, 1 << 10},
    {266, 1 << 11},
    {267, 1 << 12},
    {268, 1 << 13},
    {269, 1 << 14},
    {270, 1 << 15},
    {271, 1 << 16},
};
const std::vector<std::string> ARMPrologueBit2Reg = {
    "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r14(lr)",
    "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15", };
const std::vector<std::string> ARMBit2Reg = {
    "r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",  "r8",  "r9",  "r10",
    "r11", "r12", "r13", "r14", "r15"};
} // namespace
static uint64_t getConstMetaVal(const MachineInstr &MI, unsigned Idx) {
  assert(MI.getOperand(Idx).isImm() &&
         MI.getOperand(Idx).getImm() == StackMaps::ConstantOp);
  const auto &MO = MI.getOperand(Idx + 1);
  assert(MO.isImm());
  return MO.getImm();
}

StackMapOpers::StackMapOpers(const MachineInstr *MI) : MI(MI) {
  assert(getVarIdx() <= MI->getNumOperands() && "invalid stackmap definition");
}

PatchPointOpers::PatchPointOpers(const MachineInstr *MI)
    : MI(MI), HasDef(MI->getOperand(0).isReg() && MI->getOperand(0).isDef() &&
                     !MI->getOperand(0).isImplicit()) {
#ifndef NDEBUG
  unsigned CheckStartIdx = 0, e = MI->getNumOperands();
  while (CheckStartIdx < e && MI->getOperand(CheckStartIdx).isReg() &&
         MI->getOperand(CheckStartIdx).isDef() &&
         !MI->getOperand(CheckStartIdx).isImplicit())
    ++CheckStartIdx;

  assert(getMetaIdx() == CheckStartIdx &&
         "Unexpected additional definition in Patchpoint intrinsic.");
#endif
}

unsigned PatchPointOpers::getNextScratchIdx(unsigned StartIdx) const {
  if (!StartIdx)
    StartIdx = getVarIdx();

  // Find the next scratch register (implicit def and early clobber)
  unsigned ScratchIdx = StartIdx, e = MI->getNumOperands();
  while (ScratchIdx < e && !(MI->getOperand(ScratchIdx).isReg() &&
                             MI->getOperand(ScratchIdx).isDef() &&
                             MI->getOperand(ScratchIdx).isImplicit() &&
                             MI->getOperand(ScratchIdx).isEarlyClobber()))
    ++ScratchIdx;

  assert(ScratchIdx != e && "No scratch register available");
  return ScratchIdx;
}

unsigned StatepointOpers::getNumStackPtrsIdx() {
  // Take index of num of struct arg entries and skip all struct arg entries.
  unsigned CurIdx = getNumStructArgEntriesIdx();
  unsigned StructArgSize = getConstMetaVal(*MI, CurIdx - 1);
  CurIdx++;
  // 2 : StructArgMap comes with pairs
  return (CurIdx + 1 + 2 * StructArgSize);
}

unsigned StatepointOpers::getNumStructArgEntriesIdx() {
  // Take index of num of gc map entries and skip all gc map entries.
  unsigned CurIdx = getNumGcMapEntriesIdx();
  unsigned GCMapSize = getConstMetaVal(*MI, CurIdx - 1);
  CurIdx++;
  // 2 : GcMap comes with pairs
  return (CurIdx + 1 + 2 * GCMapSize); // skip <StackMaps::ConstantOp>
}

unsigned StatepointOpers::getNumGcMapEntriesIdx() {
  // Take index of num of allocas and skip all allocas records.
  unsigned CurIdx = getNumAllocaIdx();
  unsigned NumAllocas = getConstMetaVal(*MI, CurIdx - 1);
  CurIdx++;
  while (NumAllocas--)
    CurIdx = StackMaps::getNextMetaArgIdx(MI, CurIdx);
  return CurIdx + 1; // skip <StackMaps::ConstantOp>
}

unsigned StatepointOpers::getNumAllocaIdx() {
  // Take index of num of gc ptrs and skip all gc ptr records.
  unsigned CurIdx = getNumGCPtrIdx();
  unsigned NumGCPtrs = getConstMetaVal(*MI, CurIdx - 1);
  CurIdx++;
  while (NumGCPtrs--)
    CurIdx = StackMaps::getNextMetaArgIdx(MI, CurIdx);
  return CurIdx + 1; // skip <StackMaps::ConstantOp>
}

unsigned StatepointOpers::getNumGCPtrIdx() {
  // Take index of num of deopt args and skip all deopt records.
  unsigned CurIdx = getNumDeoptArgsIdx();
  unsigned NumDeoptArgs = getConstMetaVal(*MI, CurIdx - 1);
  CurIdx++;
  while (NumDeoptArgs--) {
    CurIdx = StackMaps::getNextMetaArgIdx(MI, CurIdx);
  }
  return CurIdx + 1; // skip <StackMaps::ConstantOp>
}

int StatepointOpers::getFirstGCPtrIdx() {
  unsigned NumGCPtrsIdx = getNumGCPtrIdx();
  unsigned NumGCPtrs = getConstMetaVal(*MI, NumGCPtrsIdx - 1);
  if (NumGCPtrs == 0)
    return -1;
  ++NumGCPtrsIdx; // skip <num gc ptrs>
  assert(NumGCPtrsIdx < MI->getNumOperands());
  return (int)NumGCPtrsIdx;
}

int StatepointOpers::getFirstAllocaIdx() {
  unsigned NumAllocaIdx = getNumAllocaIdx();
  unsigned NumAllocas = getConstMetaVal(*MI, NumAllocaIdx - 1);
  if (NumAllocas == 0)
    return -1;
  ++NumAllocaIdx; // skip <num allocas>
  assert(NumAllocaIdx < MI->getNumOperands());
  return (int)NumAllocaIdx;
}

int StatepointOpers::getFirstStackPtrIdx() {
  unsigned NumStackPtrsIdx = getNumStackPtrsIdx();
  unsigned NumStackPtrs = getConstMetaVal(*MI, NumStackPtrsIdx - 1);
  if (NumStackPtrs == 0)
    return -1;
  ++NumStackPtrsIdx; // skip <num stack ptrs>
  assert(NumStackPtrsIdx < MI->getNumOperands());
  return (int)NumStackPtrsIdx;
}

unsigned StatepointOpers::getGCPointerMap(
    SmallVectorImpl<std::pair<unsigned, unsigned>> &GCMap) {
  unsigned CurIdx = getNumGcMapEntriesIdx();
  unsigned GCMapSize = getConstMetaVal(*MI, CurIdx - 1);
  CurIdx++;
  for (unsigned N = 0; N < GCMapSize; ++N) {
    unsigned B = MI->getOperand(CurIdx++).getImm();
    unsigned D = MI->getOperand(CurIdx++).getImm();
    GCMap.push_back(std::make_pair(B, D));
  }

  return GCMapSize;
}

unsigned StatepointOpers::getStructArgMap(
    SmallVectorImpl<std::pair<unsigned, signed>> &StructArgMap) {
  unsigned CurIdx = getNumStructArgEntriesIdx();
  unsigned StructArgMapSize = getConstMetaVal(*MI, CurIdx - 1);
  CurIdx++;
  for (unsigned N = 0; N < StructArgMapSize; ++N) {
    unsigned B = MI->getOperand(CurIdx++).getImm();
    signed Offset = MI->getOperand(CurIdx++).getImm();
    StructArgMap.push_back(std::make_pair(B, Offset));
  }

  return StructArgMapSize;
}

const Function *StatepointOpers::getCalledFunction() {
  const MachineOperand &CallTarget = getCallTarget();
  if (!CallTarget.isGlobal())
    return nullptr;

  return dyn_cast<const Function>(CallTarget.getGlobal());
}

bool StatepointOpers::isCJStackCheck() {
  return getID() == Cangjie::CJStatepointID::StackCheck;
}

StackMaps::StackMaps(AsmPrinter &AP) : AP(AP) {
  if (StackMapVersion != 3)
    llvm_unreachable("Unsupported stackmap version!");
}

unsigned StackMaps::getNextMetaArgIdx(const MachineInstr *MI, unsigned CurIdx) {
  assert(CurIdx < MI->getNumOperands() && "Bad meta arg index");
  const auto &MO = MI->getOperand(CurIdx);
  if (MO.isImm()) {
    switch (MO.getImm()) {
    default:
      llvm_unreachable("Unrecognized operand type.");
    case StackMaps::DirectMemRefOp:
    case StackMaps::IndirectMemRefOp:
      CurIdx += 3;
      break;
    case StackMaps::ConstantOp:
      ++CurIdx;
      break;
    }
  }
  ++CurIdx;
  assert(CurIdx < MI->getNumOperands() && "points past operand list");
  return CurIdx;
}

/// Go up the super-register chain until we hit a valid dwarf register number.
static unsigned getDwarfRegNum(unsigned Reg, const TargetRegisterInfo *TRI) {
  int RegNum = TRI->getDwarfRegNum(Reg, false);
  for (MCSuperRegIterator SR(Reg, TRI); SR.isValid() && RegNum < 0; ++SR)
    RegNum = TRI->getDwarfRegNum(*SR, false);

  assert(RegNum >= 0 && "Invalid Dwarf register number.");
  return (unsigned)RegNum;
}

// cj array element should be pointer type, or primitive type or struct type
void StackMaps::processArrayType(ArrayType *AT, int64_t RefOffset,
                                 LocationVec &Locations, unsigned Reg,
                                 const GCStrategy *GS) const {
  const TargetRegisterInfo *TRI = AP.MF->getSubtarget().getRegisterInfo();
  const DataLayout &DL = AP.MF->getDataLayout();
  uint64_t Size = AT->getNumElements();
  Type *ElementType = AT->getElementType();
  if (isa<PointerType>(ElementType)) {
    for (unsigned Index = 0; Index < Size; Index++) {
      Locations.emplace_back(StackMaps::Location::Indirect,
                             FuncPtrSize, // size of the register
                             getDwarfRegNum(Reg, TRI),
                             RefOffset + Index * FuncPtrSize);
    }
  } else if (StructType *ArrayStruct = dyn_cast<StructType>(ElementType)) {
    int64_t DerivedOffset = DL.getStructLayout(ArrayStruct)->getSizeInBytes();
    for (unsigned Index = 0; Index < Size; Index++) {
      processAllocaStructType(ArrayStruct, RefOffset + Index * DerivedOffset,
                              Locations, Reg, GS);
    }
  }
}

void StackMaps::processAllocaStructType(StructType *ST, int64_t RefOffset,
                                        LocationVec &Locations, unsigned Reg,
                                        const GCStrategy *GS) const {
  const TargetRegisterInfo *TRI = AP.MF->getSubtarget().getRegisterInfo();
  const DataLayout &DL = AP.MF->getDataLayout();
  for (int64_t i = 0; i < ST->getNumElements(); i++) {
    int64_t Offset = DL.getStructLayout(ST)->getElementOffset(i) + RefOffset;
    if (PointerType *EPT = dyn_cast<PointerType>(ST->getElementType(i))) {
      if (!*GS->isGCManagedPointer(EPT)) {
        continue;
      }
      Locations.emplace_back(StackMaps::Location::Indirect,
                             FuncPtrSize, // size of the register
                             getDwarfRegNum(Reg, TRI), Offset);
    } else if (StructType *EST = dyn_cast<StructType>(ST->getElementType(i))) {
      processAllocaStructType(EST, Offset, Locations, Reg, GS);
    } else if (ArrayType *AT = dyn_cast<ArrayType>(ST->getElementType(i))) {
      processArrayType(AT, Offset, Locations, Reg, GS);
    }
  }
}

MachineInstr::const_mop_iterator
StackMaps::parseImmOperand(MachineInstr::const_mop_iterator MOI,
                           CallsiteInfo &CSInfo, bool IsResult) const {
  const TargetRegisterInfo *TRI = AP.MF->getSubtarget().getRegisterInfo();
  assert(MOI->isImm() && "MOI is not a Imm when parseImmOperand!");
  switch (MOI->getImm()) {
  default:
    report_fatal_error("Unrecognized operand type.");
  case StackMaps::DirectMemRefOp: {
    const auto &DL = AP.MF->getDataLayout();
    unsigned Size = DL.getPointerSizeInBits();
    assert((Size % 8) == 0 && "Need pointer size in bytes.");
    Size /= 8;
    Register Reg = (++MOI)->getReg();
    int64_t Imm = (++MOI)->getImm();
    CSInfo.Locations.emplace_back(StackMaps::Location::Direct, Size,
                                  getDwarfRegNum(Reg, TRI), Imm);
    int64_t FI = (++MOI)->getImm();
    (void)FI;
    assert(!CJPipeline && "illegal cangjie process");
    break;
  }
  case StackMaps::IndirectMemRefOp: {
    int64_t Size = (++MOI)->getImm();
    assert(Size > 0 && "Need a valid size for indirect memory locations.");
    Register Reg = (++MOI)->getReg();
    int64_t Imm = (++MOI)->getImm();
    CSInfo.Locations.emplace_back(StackMaps::Location::Indirect, Size,
                                  getDwarfRegNum(Reg, TRI), Imm);
    if (!IsResult) {
      CSInfo.RefPairs.emplace_back(CSInfo.Locations.back());
    }
    break;
  }
  case StackMaps::ConstantOp: {
    ++MOI;
    assert(MOI->isImm() && "Expected constant operand.");
    int64_t Imm = MOI->getImm();
    CSInfo.Locations.emplace_back(Location::Constant, sizeof(int64_t), 0, Imm);
    break;
  }
  }
  return ++MOI;
}

MachineInstr::const_mop_iterator
StackMaps::parseStructArgsOperand(MachineInstr::const_mop_iterator MOI,
                                  LocationVec &FOLocations,
                                  unsigned Offset) const {
  assert(MOI->isImm() && "MOI is not a Imm when parseStructArgsOperand!");
  assert((Offset % OffsetStepSize == 0) && "Struct Offset should be aligned!");
  const TargetRegisterInfo *TRI = AP.MF->getSubtarget().getRegisterInfo();
  if (MOI->getImm() == StackMaps::DirectMemRefOp) {
    Register Reg = (++MOI)->getReg();
    int64_t Imm = (++MOI)->getImm();
    int64_t FI = (++MOI)->getImm();
    (void)FI;

#ifndef NDEBUG
    const AllocaInst *Inst = AP.MF->getFrameInfo().getObjectAllocation(FI);
    assert(Inst && "The FI is not alloca inst!");
    const DataLayout &DL = AP.MF->getDataLayout();
    StructType *ST = dyn_cast<StructType>(Inst->getAllocatedType());
    assert(DL.getStructLayout(ST)->getSizeInBytes() > Offset &&
           "The offset cannot be greater than the size of struct.");
#endif
    // 4/8:ptr size
    FOLocations.emplace_back(StackMaps::Location::Indirect, FuncPtrSize,
                             getDwarfRegNum(Reg, TRI), Imm + Offset);
  } else if (MOI->getImm() == StackMaps::IndirectMemRefOp) {
    int64_t Size = (++MOI)->getImm();
    Register Reg = (++MOI)->getReg();
    int64_t Imm = (++MOI)->getImm();

    FOLocations.emplace_back(StackMaps::Location::Indirect, Size,
                             getDwarfRegNum(Reg, TRI), Imm);
  } else {
    assert(false && "MOI is error when parseStructArgsOperand!");
  }
  return ++MOI;
}

MachineInstr::const_mop_iterator
StackMaps::parseAllocaOperand(MachineInstr::const_mop_iterator MOI,
                              LocationVec &Locations) const {
  const TargetRegisterInfo *TRI = AP.MF->getSubtarget().getRegisterInfo();
  assert(MOI->isImm() && "MOI is not a Imm when parseAllocaOperand!");
  assert(MOI->getImm() == StackMaps::DirectMemRefOp &&
         "MOI is not a DirectMemRefOp when parseAllocaOperand!");

  Register Reg = (++MOI)->getReg();
  int64_t Imm = (++MOI)->getImm();
  int64_t FI = (++MOI)->getImm();

  GCStrategy *GS = AP.getAnalysisIfAvailable<GCModuleInfo>()->getGCStrategy(
      StringRef("cangjie")); // cfile ref strategy.
  // add FI in FOLocation
  const AllocaInst *Inst = AP.MF->getFrameInfo().getObjectAllocation(FI);
  if (Inst != nullptr) {
    if (auto *ST = dyn_cast<StructType>(Inst->getAllocatedType())) {
      processAllocaStructType(ST, Imm, Locations, Reg, GS);
    } else if (auto *PT = dyn_cast<PointerType>(Inst->getAllocatedType())) {
      if (*GS->isGCManagedPointer(PT) ||
          *GS->isGCManagedPointer(PT->getElementType())) {
        // 4/8:ptr size
        Locations.emplace_back(StackMaps::Location::Indirect, FuncPtrSize,
                               getDwarfRegNum(Reg, TRI), Imm);
      }
    }
  } else {
    // The FI of alloca may be modify in tryToElideArgumentCopy.
    // In this case, alloca type must be a pointer type.
    // 4/8:ptr size
    Locations.emplace_back(StackMaps::Location::Indirect, FuncPtrSize,
                           getDwarfRegNum(Reg, TRI), Imm);
  }
  return ++MOI;
}

MachineInstr::const_mop_iterator
StackMaps::parseStackPtrOperand(MachineInstr::const_mop_iterator MOI,
                                LocationVec &Locations) const {
  assert(MOI->isImm() && "MOI is not a Imm when parseStackPtrOperand!");
  const TargetRegisterInfo *TRI = AP.MF->getSubtarget().getRegisterInfo();
  if (MOI->getImm() == StackMaps::IndirectMemRefOp) {
    int64_t Size = (++MOI)->getImm();
    Register Reg = (++MOI)->getReg();
    int64_t Imm = (++MOI)->getImm();
    assert((Imm % OffsetStepSize == 0) && "Stack Offset align error!");
    Locations.emplace_back(StackMaps::Location::Indirect, Size,
                           getDwarfRegNum(Reg, TRI), Imm);
  } else if (MOI->getImm() == StackMaps::DirectMemRefOp) {
    Register Reg = (++MOI)->getReg();
    int64_t Imm = (++MOI)->getImm();
    int64_t FI = (++MOI)->getImm();
    (void)FI;
    assert((Imm % OffsetStepSize == 0) && "Stack Offset align error!");
    Locations.emplace_back(StackMaps::Location::Indirect, FuncPtrSize,
                           getDwarfRegNum(Reg, TRI), Imm); // 4/8: pointer size
  } else {
    report_fatal_error("MOI is error when parseStackPtrOperand!");
  }
  return ++MOI;
}

MachineInstr::const_mop_iterator
StackMaps::parseRegOperand(MachineInstr::const_mop_iterator MOI,
                           CallsiteInfo &CSInfo, LocationVec &Locations,
                           bool IsResult) const {
  const TargetRegisterInfo *TRI = AP.MF->getSubtarget().getRegisterInfo();
  assert(MOI->isReg() && "MOI is not a Reg when parseRegOperand!");
  // Skip implicit registers (this includes our scratch registers)
  if (MOI->isImplicit())
    return ++MOI;

  if (MOI->isUndef()) {
    // Record `undef` register as constant. Use same value as ISel uses.
    Locations.emplace_back(Location::Constant, sizeof(int64_t), 0, 0xFEFEFEFE);
    return ++MOI;
  }

  assert(Register::isPhysicalRegister(MOI->getReg()) &&
         "Virtreg operands should have been rewritten before now.");
  const TargetRegisterClass *RC = TRI->getMinimalPhysRegClass(MOI->getReg());
  assert(!MOI->getSubReg() && "Physical subreg still around.");

  // 15, 31: max general regNum of ref, x86_64:15 , aarch64:31 and arm:15
  unsigned MaxRegIdx = isAArch64() ? 31 : 15;
  unsigned Offset = 0;
  unsigned DwarfRegNum = getDwarfRegNum(MOI->getReg(), TRI);
  unsigned LLVMRegNum = *TRI->getLLVMRegNum(DwarfRegNum, false);
  unsigned SubRegIdx = TRI->getSubRegIndex(LLVMRegNum, MOI->getReg());
  if (SubRegIdx)
    Offset = TRI->getSubRegIdxOffset(SubRegIdx);

  Locations.emplace_back(Location::Register, TRI->getSpillSize(*RC),
                         DwarfRegNum, Offset);

  if (!IsResult && CJPipeline) {
    if (DwarfRegNum > MaxRegIdx) {
      report_fatal_error("ref reg don't support float regNum!");
    }
    CSInfo.RefPairs.emplace_back(CSInfo.Locations.back());
  }
  return ++MOI;
}

MachineInstr::const_mop_iterator
StackMaps::parseOperand(MachineInstr::const_mop_iterator MOI,
                        CallsiteInfo &CSInfo, bool IsResult) const {
  if (MOI->isImm()) {
    return parseImmOperand(MOI, CSInfo, IsResult);
  }
  // The physical register number will ultimately be encoded as a DWARF regno.
  // The stack map also records the size of a spill slot that can hold the
  // register content. (The runtime can track the actual size of the data type
  // if it needs to.)
  if (MOI->isReg()) {
    return parseRegOperand(MOI, CSInfo, CSInfo.Locations, IsResult);
  }

  if (MOI->isRegLiveOut())
    CSInfo.LiveOuts = parseRegisterLiveOutMask(MOI->getRegLiveOut());

  return ++MOI;
}

void StackMaps::print(raw_ostream &OS) {
  const TargetRegisterInfo *TRI =
      AP.MF ? AP.MF->getSubtarget().getRegisterInfo() : nullptr;
  OS << WSMP << "callsites:\n";
  for (const auto &CSI : CSInfos) {
    const LocationVec &CSLocs = CSI.Locations;
    const LiveOutVec &LiveOuts = CSI.LiveOuts;

    OS << WSMP << "callsite " << CSI.ID << "\n";
    OS << WSMP << "  has " << CSLocs.size() << " locations\n";

    unsigned Idx = 0;
    for (const auto &Loc : CSLocs) {
      OS << WSMP << "\t\tLoc " << Idx << ": ";
      switch (Loc.Type) {
      case Location::Unprocessed:
        OS << "<Unprocessed operand>";
        break;
      case Location::Register:
        OS << "Register ";
        if (TRI)
          OS << printReg(Loc.Reg, TRI);
        else
          OS << Loc.Reg;
        break;
      case Location::Direct:
        OS << "Direct ";
        if (TRI)
          OS << printReg(Loc.Reg, TRI);
        else
          OS << Loc.Reg;
        if (Loc.Offset)
          OS << " + " << Loc.Offset;
        break;
      case Location::Indirect:
        OS << "Indirect ";
        if (TRI)
          OS << printReg(Loc.Reg, TRI);
        else
          OS << Loc.Reg;
        OS << "+" << Loc.Offset;
        break;
      case Location::Constant:
        OS << "Constant " << Loc.Offset;
        break;
      case Location::ConstantIndex:
        OS << "Constant Index " << Loc.Offset;
        break;
      }
      OS << "\t[encoding: .byte " << Loc.Type << ", .byte 0"
         << ", .short " << Loc.Size << ", .short " << Loc.Reg << ", .short 0"
         << ", .int " << Loc.Offset << "]\n";
      Idx++;
    }

    OS << WSMP << "\thas " << LiveOuts.size() << " live-out registers\n";

    Idx = 0;
    for (const auto &LO : LiveOuts) {
      OS << WSMP << "\t\tLO " << Idx << ": ";
      if (TRI)
        OS << printReg(LO.Reg, TRI);
      else
        OS << LO.Reg;
      OS << "\t[encoding: .short " << LO.DwarfRegNum << ", .byte 0, .byte "
         << LO.Size << "]\n";
      Idx++;
    }
  }
}

/// Create a live-out register record for the given register Reg.
StackMaps::LiveOutReg
StackMaps::createLiveOutReg(unsigned Reg, const TargetRegisterInfo *TRI) const {
  unsigned DwarfRegNum = getDwarfRegNum(Reg, TRI);
  unsigned Size = TRI->getSpillSize(*TRI->getMinimalPhysRegClass(Reg));
  return LiveOutReg(Reg, DwarfRegNum, Size);
}

/// Parse the register live-out mask and return a vector of live-out registers
/// that need to be recorded in the stackmap.
StackMaps::LiveOutVec
StackMaps::parseRegisterLiveOutMask(const uint32_t *Mask) const {
  assert(Mask && "No register mask specified");
  const TargetRegisterInfo *TRI = AP.MF->getSubtarget().getRegisterInfo();
  LiveOutVec LiveOuts;

  // Create a LiveOutReg for each bit that is set in the register mask.
  for (unsigned Reg = 0, NumRegs = TRI->getNumRegs(); Reg != NumRegs; ++Reg)
    if ((Mask[Reg / 32] >> (Reg % 32)) & 1)
      LiveOuts.push_back(createLiveOutReg(Reg, TRI));

  // We don't need to keep track of a register if its super-register is already
  // in the list. Merge entries that refer to the same dwarf register and use
  // the maximum size that needs to be spilled.

  llvm::sort(LiveOuts, [](const LiveOutReg &LHS, const LiveOutReg &RHS) {
    // Only sort by the dwarf register number.
    return LHS.DwarfRegNum < RHS.DwarfRegNum;
  });

  for (auto I = LiveOuts.begin(), E = LiveOuts.end(); I != E; ++I) {
    for (auto *II = std::next(I); II != E; ++II) {
      if (I->DwarfRegNum != II->DwarfRegNum) {
        // Skip all the now invalid entries.
        I = --II;
        break;
      }
      I->Size = std::max(I->Size, II->Size);
      if (TRI->isSuperRegister(I->Reg, II->Reg))
        I->Reg = II->Reg;
      II->Reg = 0; // mark for deletion.
    }
  }

  llvm::erase_if(LiveOuts, [](const LiveOutReg &LO) { return LO.Reg == 0; });

  return LiveOuts;
}

void StackMaps::updateOrInsertFnInfo(const MCSymbol *FnSym,
                                     CallsiteInfo &CallInfo) {
  auto Itr = FnInfos.find(FnSym);
  if (Itr == FnInfos.end()) {
    // create a new function info.
    // get the stack size of the current function
    const MachineFrameInfo &MFI = AP.MF->getFrameInfo();
    const TargetRegisterInfo *RegInfo = AP.MF->getSubtarget().getRegisterInfo();
    bool HasDynamicFrameSize =
        MFI.hasVarSizedObjects() || RegInfo->hasStackRealignment(*(AP.MF));
    uint64_t FrameSize = HasDynamicFrameSize ? UINT64_MAX : MFI.getStackSize();
    // Use frame pointer offset instead of frame size on win64 in cangjie.
    if (CJPipeline && AP.getSubtargetInfo().getTargetTriple().isOSWindows()) {
      FrameSize = MFI.getWin64FramePointerOffset();
    }
    if (CJPipeline && isARM()) {
      FrameSize = MFI.getStackSize();
    }
    // get func callee saved regInfo
    const auto &CSInfo = AP.MF->getFrameInfo().getCalleeSavedInfo();
    std::map<unsigned, int> CSReg2Stack; // <reg, stackOffset>
    const TargetFrameLowering *TFI = AP.MF->getSubtarget().getFrameLowering();
    for (const auto &CS : CSInfo) {
      if (CS.isSpilledToReg()) {
        continue;
      }
      unsigned RegNo = getDwarfRegNum(CS.getReg(), RegInfo);
      if (isARM() && (RegNo == 11 /*R11*/ || RegNo == 14 /*LR*/))
        continue;
      Register Reg;
      CSReg2Stack[RegNo] =
          TFI->getFrameIndexRefForCJ(*AP.MF, CS.getFrameIdx(), Reg).getFixed();
    }
    FnInfos.insert(std::make_pair(FnSym, FunctionInfo(FrameSize, CSReg2Stack)));
    Itr = FnInfos.find(FnSym);
  }

  // Do not need emit callsite info when it has no any Locations and LineNumber
  // in cangjie pipeline.
  if (CJPipeline && CallInfo.RefPairs.empty() && CallInfo.FOLocations.empty() &&
      CallInfo.StackLocations.empty() && CallInfo.LineNumber == 0)
    return;

  // update callsite count.
  Itr->second.RecordCount++;
  CSInfos.emplace_back(CallInfo);
}

MachineInstr::const_mop_iterator
StackMaps::parseCangjieStackOpers(MachineInstr::const_mop_iterator MOI,
                                  CallsiteInfo &CSInfo) const {
  if (MOI->isReg()) {
    MOI = parseRegOperand(MOI, CSInfo, CSInfo.StackLocations, true);
  } else if (MOI->isImm()) {
    MOI = parseStackPtrOperand(MOI, CSInfo.StackLocations);
  } else {
    report_fatal_error("Unrecognized operand type.");
  }
  return MOI;
}

void StackMaps::parseCangjieStatepointOpers(
    const MachineInstr &MI, StatepointOpers &SO, CallsiteInfo &CSInfo,
    unsigned NumAllocas, SmallVectorImpl<unsigned> &GCPtrIndices) const {
  auto MOB = MI.operands_begin();
  // Map logical index of alloca ptr to MI operand index.
  SmallVector<unsigned, 8> AllocaIndices;

  if (NumAllocas) {
    unsigned AllocaIdx = (unsigned)SO.getFirstAllocaIdx();
    assert((int)AllocaIdx != -1 && "AllocaIdx cannot be -1");
    while (NumAllocas--) {
      AllocaIndices.push_back(AllocaIdx);
      AllocaIdx = StackMaps::getNextMetaArgIdx(&MI, AllocaIdx);
    }

    SmallVector<std::pair<unsigned, signed>, 8> StructPairs;
    unsigned NumStructPairs = SO.getStructArgMap(StructPairs);
    (void)NumStructPairs;
    LLVM_DEBUG(dbgs() << "NumStructPairs = " << NumStructPairs << "\n");

    for (auto &P : StructPairs) {
      assert(P.first < AllocaIndices.size() && "struct ptr index not found");
      unsigned StructIdx = AllocaIndices[P.first];
      if (P.second == -1) {
        parseAllocaOperand(MOB + StructIdx, CSInfo.FOLocations);
      } else {
        parseStructArgsOperand(MOB + StructIdx, CSInfo.FOLocations, P.second);
      }
    }
  }

  if (EnableStackGrow) {
    unsigned NumStackPtrsIdx = SO.getNumStackPtrsIdx();
    auto MOI = MOB + NumStackPtrsIdx;
    unsigned NumStackPtrs = MOI->getImm();
    ++MOI;
    while (NumStackPtrs--) {
      MOI = parseCangjieStackOpers(MOI, CSInfo);
    }
  }
}

// See statepoint MI format description in StatepointOpers' class comment
// in include/llvm/CodeGen/StackMaps.h
void StackMaps::parseStatepointOpers(const MachineInstr &MI,
                                     MachineInstr::const_mop_iterator MOI,
                                     MachineInstr::const_mop_iterator MOE,
                                     CallsiteInfo &CSInfo) {
  LLVM_DEBUG(dbgs() << "record statepoint : " << MI << "\n");
  StatepointOpers SO(&MI);
  MOI = parseOperand(MOI, CSInfo, false); // CC
  MOI = parseOperand(MOI, CSInfo, false); // Flags
  MOI = parseOperand(MOI, CSInfo, false); // Num Deopts

  // Record Deopt Args.
  unsigned NumDeoptArgs = CSInfo.Locations.back().Offset;
  assert(CSInfo.Locations.back().Type == Location::Constant);
  assert(NumDeoptArgs == SO.getNumDeoptArgs());

  while (NumDeoptArgs--)
    MOI = parseOperand(MOI, CSInfo, false);

  // Record gc base/derived pairs
  assert(MOI->isImm() && MOI->getImm() == StackMaps::ConstantOp);
  ++MOI;
  assert(MOI->isImm());
  unsigned NumGCPointers = MOI->getImm();
  ++MOI;

  // Map logical index of GC ptr to MI operand index.
  SmallVector<unsigned, 8> GCPtrIndices;
  if (NumGCPointers) {
    unsigned GCPtrIdx = (unsigned)SO.getFirstGCPtrIdx();
    assert((int)GCPtrIdx != -1);
    assert(MOI - MI.operands_begin() == GCPtrIdx + 0LL);
    while (NumGCPointers--) {
      GCPtrIndices.push_back(GCPtrIdx);
      GCPtrIdx = StackMaps::getNextMetaArgIdx(&MI, GCPtrIdx);
    }

    SmallVector<std::pair<unsigned, unsigned>, 8> GCPairs;
    unsigned NumGCPairs = SO.getGCPointerMap(GCPairs);
    (void)NumGCPairs;
    LLVM_DEBUG(dbgs() << "NumGCPairs = " << NumGCPairs << "\n");

    auto MOB = MI.operands_begin();
    for (auto &P : GCPairs) {
      assert(P.first < GCPtrIndices.size() && "base pointer index not found");
      assert(P.second < GCPtrIndices.size() &&
             "derived pointer index not found");
      unsigned BaseIdx = GCPtrIndices[P.first];
      unsigned DerivedIdx = GCPtrIndices[P.second];
      LLVM_DEBUG(dbgs() << "Base : " << BaseIdx << " Derived : " << DerivedIdx
                        << "\n");
      parseOperand(MOB + BaseIdx, CSInfo, false);
      parseOperand(MOB + DerivedIdx, CSInfo, false);
    }

    MOI = MOB + GCPtrIdx;
  }

  // Record gc allocas
  assert(MOI < MOE);
  assert(MOI->isImm() && MOI->getImm() == StackMaps::ConstantOp);
  ++MOI;
  unsigned NumAllocas = MOI->getImm();
  ++MOI;
  if (CJPipeline) {
    parseCangjieStatepointOpers(MI, SO, CSInfo, NumAllocas, GCPtrIndices);
  } else {
    while (NumAllocas--) {
      MOI = parseOperand(MOI, CSInfo, false);
      assert(MOI < MOE);
    }
  }
}

void StackMaps::recordStackMapOpers(const MCSymbol &MILabel,
                                    const MachineInstr &MI,
                                    StackMapOpersInfo OpersInfo,
                                    bool RecordResult, bool RecordAllRef) {
  CallsiteInfo CSInfo;
  CSInfo.ID = OpersInfo.ID;
  CSInfo.RecordAllRefInReg = RecordAllRef;
  if (RecordResult) {
    assert(PatchPointOpers(&MI).hasDef() && "Stackmap has no return value.");
    parseOperand(MI.operands_begin(), CSInfo, true);
  }

  // Parse operands.
  if (isStatepointOpcode(MI.getOpcode()))
    parseStatepointOpers(MI, OpersInfo.MOI, OpersInfo.MOE, CSInfo);
  else
    while (OpersInfo.MOI != OpersInfo.MOE)
      OpersInfo.MOI = parseOperand(OpersInfo.MOI, CSInfo, false);

  // Move large constants into the constant pool.
  for (auto &Loc : CSInfo.Locations) {
    // Constants are encoded as sign-extended integers.
    // -1 is directly encoded as .long 0xFFFFFFFF with no constant pool.
    if (Loc.Type == Location::Constant && !isInt<32>(Loc.Offset)) {
      Loc.Type = Location::ConstantIndex;
      // ConstPool is intentionally a MapVector of 'uint64_t's (as
      // opposed to 'int64_t's).  We should never be in a situation
      // where we have to insert either the tombstone or the empty
      // keys into a map, and for a DenseMap<uint64_t, T> these are
      // (uint64_t)0 and (uint64_t)-1.  They can be and are
      // represented using 32 bit integers.
      assert((uint64_t)Loc.Offset != DenseMapInfo<uint64_t>::getEmptyKey() &&
             (uint64_t)Loc.Offset !=
                 DenseMapInfo<uint64_t>::getTombstoneKey() &&
             "empty and tombstone keys should fit in 32 bits!");
      auto Result = ConstPool.insert(std::make_pair(Loc.Offset, Loc.Offset));
      Loc.Offset = Result.first - ConstPool.begin();
    }
  }
  MCContext &OutContext = AP.OutStreamer->getContext();
  // Create an expression to calculate the offset of the callsite from function
  // entry.
  CSInfo.CSOffsetExpr = MCBinaryExpr::createSub(
      MCSymbolRefExpr::create(&MILabel, OutContext),
      MCSymbolRefExpr::create(AP.CurrentFnSymForSize, OutContext), OutContext);

  // get Line number from debug info
  uint32_t Line = 0;
  if (isStatepointOpcode(MI.getOpcode())) {
    if (OpersInfo.ID == Cangjie::CJStatepointID::StackCheck &&
        MI.getMF()->getFunction().getSubprogram() != nullptr) {
      Line = MI.getMF()->getFunction().getSubprogram()->getLine();
    }
  }
  CSInfo.LineNumber = Line;
  DILocation *DIL = MI.getDebugLoc().get();
  if (DIL != nullptr) {
    DILocation *InlinedDIL = DIL->getInlinedAt();
    while (InlinedDIL != nullptr) {
      DIL = InlinedDIL;
      InlinedDIL = InlinedDIL->getInlinedAt();
    }
    CSInfo.LineNumber = DIL->getLine();
  }
  updateOrInsertFnInfo(AP.CurrentFnSym, CSInfo);
}

void StackMaps::recordStackMap(const MCSymbol &L, const MachineInstr &MI) {
  assert(MI.getOpcode() == TargetOpcode::STACKMAP && "expected stackmap");

  StackMapOpers Opers(&MI);
  StackMapOpersInfo OpersInfo;
  OpersInfo.ID = MI.getOperand(PatchPointOpers::IDPos).getImm();
  OpersInfo.MOI = std::next(MI.operands_begin(), Opers.getVarIdx());
  OpersInfo.MOE = MI.operands_end();
  recordStackMapOpers(L, MI, OpersInfo);
}

void StackMaps::recordPatchPoint(const MCSymbol &L, const MachineInstr &MI) {
  assert(MI.getOpcode() == TargetOpcode::PATCHPOINT && "expected patchpoint");

  PatchPointOpers Opers(&MI);
  StackMapOpersInfo OpersInfo;
  OpersInfo.ID = Opers.getID();
  OpersInfo.MOI = std::next(MI.operands_begin(), Opers.getStackMapStartIdx());
  OpersInfo.MOE = MI.operands_end();
  recordStackMapOpers(L, MI, OpersInfo, Opers.isAnyReg() && Opers.hasDef());

#ifndef NDEBUG
  // verify anyregcc
  auto &Locations = CSInfos.back().Locations;
  if (Opers.isAnyReg()) {
    unsigned NArgs = Opers.getNumCallArgs();
    for (unsigned i = 0, e = (Opers.hasDef() ? NArgs + 1 : NArgs); i != e; ++i)
      assert(Locations[i].Type == Location::Register &&
             "anyreg arg must be in reg.");
  }
#endif
}

void StackMaps::recordStatepoint(const MCSymbol &L, const MachineInstr &MI,
                                 bool RecordAllRefInReg) {
  StackMapOpersInfo OpersInfo;
  if (isStatepointOpcode(MI.getOpcode())) {
    // Record all the deopt and gc operands (they're contiguous and run from the
    // initial index to the end of the operand list)
    StatepointOpers opers(&MI);
    OpersInfo.ID = opers.getID();
    OpersInfo.MOI = MI.operands_begin() + opers.getVarIdx();
    OpersInfo.MOE = MI.operands_end();
  } else {
    // for div/idiv, don't need to traverse operands, only to record label
    OpersInfo.ID = 0;
    OpersInfo.MOI = MI.operands_end();
    OpersInfo.MOE = MI.operands_end();
  }

  recordStackMapOpers(L, MI, OpersInfo, false, RecordAllRefInReg);
}

void StackMaps::recordCJStackMap(const MachineInstr &MI,
                                 bool RecordAllRefInReg) {
  const Triple TT(AP.MMI->getModule()->getTargetTriple());
  OffsetStepSize = TT.isARM() ? 4 : 8;
  MCSymbol *MILabel = AP.OutStreamer->getContext().createTempSymbol();
  AP.OutStreamer->emitLabel(MILabel);
  recordStatepoint(*MILabel, MI, RecordAllRefInReg);
}

/// Emit the stackmap header.
///
/// Header {
///   uint8  : Stack Map Version (currently 3)
///   uint8  : Reserved (expected to be 0)
///   uint16 : Reserved (expected to be 0)
/// }
/// uint32 : NumFunctions
/// uint32 : NumConstants
/// uint32 : NumRecords
void StackMaps::emitStackmapHeader(MCStreamer &OS) {
  // Header.
  OS.emitIntValue(StackMapVersion, 1); // Version.
  OS.emitIntValue(0, 1);               // Reserved.
  OS.emitInt16(0);                     // Reserved.

  // Num functions.
  LLVM_DEBUG(dbgs() << WSMP << "#functions = " << FnInfos.size() << '\n');
  OS.emitInt32(FnInfos.size());
  // Num constants.
  LLVM_DEBUG(dbgs() << WSMP << "#constants = " << ConstPool.size() << '\n');
  OS.emitInt32(ConstPool.size());
  // Num callsites.
  LLVM_DEBUG(dbgs() << WSMP << "#callsites = " << CSInfos.size() << '\n');
  OS.emitInt32(CSInfos.size());
}

/// Emit the function frame record for each function.
///
/// StkSizeRecord[NumFunctions] {
///   uint64 : Function Address
///   uint64 : Stack Size
///   uint64 : Record Count
/// }
void StackMaps::emitFunctionFrameRecords(MCStreamer &OS) {
  // Function Frame records.
  LLVM_DEBUG(dbgs() << WSMP << "functions:\n");
  for (auto const &FR : FnInfos) {
    LLVM_DEBUG(dbgs() << WSMP << "function addr: " << FR.first
                      << " frame size: " << FR.second.StackSize
                      << " callsite count: " << FR.second.RecordCount << '\n');
    OS.emitSymbolValue(FR.first, 8);
    OS.emitIntValue(FR.second.StackSize, 8);
    OS.emitIntValue(FR.second.RecordCount, 8);
  }
}

/// Emit the constant pool.
///
/// int64  : Constants[NumConstants]
void StackMaps::emitConstantPoolEntries(MCStreamer &OS) {
  // Constant pool entries.
  LLVM_DEBUG(dbgs() << WSMP << "constants:\n");
  for (const auto &ConstEntry : ConstPool) {
    LLVM_DEBUG(dbgs() << WSMP << ConstEntry.second << '\n');
    OS.emitIntValue(ConstEntry.second, 8);
  }
}

/// Emit the callsite info for each callsite.
///
/// StkMapRecord[NumRecords] {
///   uint64 : PatchPoint ID
///   uint32 : Instruction Offset
///   uint16 : Reserved (record flags)
///   uint16 : NumLocations
///   Location[NumLocations] {
///     uint8  : Register | Direct | Indirect | Constant | ConstantIndex
///     uint8  : Size in Bytes
///     uint16 : Dwarf RegNum
///     int32  : Offset
///   }
///   uint16 : Padding
///   uint16 : NumLiveOuts
///   LiveOuts[NumLiveOuts] {
///     uint16 : Dwarf RegNum
///     uint8  : Reserved
///     uint8  : Size in Bytes
///   }
///   uint32 : Padding (only if required to align to 8 byte)
/// }
///
/// Location Encoding, Type, Value:
///   0x1, Register, Reg                 (value in register)
///   0x2, Direct, Reg + Offset          (frame index)
///   0x3, Indirect, [Reg + Offset]      (spilled value)
///   0x4, Constant, Offset              (small constant)
///   0x5, ConstIndex, Constants[Offset] (large constant)
void StackMaps::emitCallsiteEntries(MCStreamer &OS) {
  LLVM_DEBUG(print(dbgs()));
  // Callsite entries.
  for (const auto &CSI : CSInfos) {
    const LocationVec &CSLocs = CSI.Locations;
    const LiveOutVec &LiveOuts = CSI.LiveOuts;

    // Verify stack map entry. It's better to communicate a problem to the
    // runtime than crash in case of in-process compilation. Currently, we do
    // simple overflow checks, but we may eventually communicate other
    // compilation errors this way.
    if (CSLocs.size() > UINT16_MAX || LiveOuts.size() > UINT16_MAX) {
      OS.emitIntValue(UINT64_MAX, 8); // Invalid ID.
      OS.emitValue(CSI.CSOffsetExpr, 4);
      OS.emitInt16(0); // Reserved.
      OS.emitInt16(0); // 0 locations.
      OS.emitInt16(0); // padding.
      OS.emitInt16(0); // 0 live-out registers.
      OS.emitInt32(0); // padding.
      continue;
    }

    OS.emitIntValue(CSI.ID, 8);
    OS.emitValue(CSI.CSOffsetExpr, 4);

    // Reserved for flags.
    OS.emitInt16(0);
    OS.emitInt16(CSLocs.size());

    for (const auto &Loc : CSLocs) {
      OS.emitIntValue(Loc.Type, 1);
      OS.emitIntValue(0, 1); // Reserved
      OS.emitInt16(Loc.Size);
      OS.emitInt16(Loc.Reg);
      OS.emitInt16(0); // Reserved
      OS.emitInt32(Loc.Offset);
    }

    // Emit alignment to 8 byte.
    OS.emitValueToAlignment(8);

    // Num live-out registers and padding to align to 4 byte.
    OS.emitInt16(0);
    OS.emitInt16(LiveOuts.size());

    for (const auto &LO : LiveOuts) {
      OS.emitInt16(LO.DwarfRegNum);
      OS.emitIntValue(0, 1);
      OS.emitIntValue(LO.Size, 1);
    }
    // Emit alignment to 8 byte.
    OS.emitValueToAlignment(8);
  }
}

/// Serialize the stackmap data.
void StackMaps::serializeToStackMapSection() {
  (void)WSMP;
  // Bail out if there's no stack map data.
  assert((!CSInfos.empty() || ConstPool.empty()) &&
         "Expected empty constant pool too!");
  assert((!CSInfos.empty() || FnInfos.empty()) &&
         "Expected empty function record too!");
  if (CSInfos.empty())
    return;

  MCContext &OutContext = AP.OutStreamer->getContext();
  MCStreamer &OS = *AP.OutStreamer;

  // Create the section.
  MCSection *StackMapSection =
      OutContext.getObjectFileInfo()->getStackMapSection();
  OS.switchSection(StackMapSection);

  // Emit a dummy symbol to force section inclusion.
  OS.emitLabel(OutContext.getOrCreateSymbol(Twine("__LLVM_StackMaps")));

  // Serialize data.
  LLVM_DEBUG(dbgs() << "********** Stack Map Output **********\n");
  emitStackmapHeader(OS);
  emitFunctionFrameRecords(OS);
  emitConstantPoolEntries(OS);
  emitCallsiteEntries(OS);
  OS.addBlankLine();

  // Clean up.
  CSInfos.clear();
  ConstPool.clear();
}

void StackMaps::emitCangjieStackMaps(MCStreamer &OS) {
  emitCangjieCompressedStackMaps(OS);
  return;
}
// use varint and BitTable to store stackMaps
// Variable-length integer:
//  The first four bits determine the length of the encoded integer:
//  Values 0..11 represent the result as-is, with no further following bits.
//  Values 12..15 mean the result is in the next 8/16/24/32-bits respectively.
// BitTable:
//   varInt RecordNums
//   varInt Record.elem0's maxBitLen
//   varInt Record.elem1's maxBitLen
//   ...
//   varInt record.elemN's maxBitLen(N are fixed for different tables)
//   Record0 {elem0, elem1, ... elemN}
//   Record1 {elem0, elem1, ... elemN}
//   ...
//   RecordM {elem0, elem1, ... elemN}
// each elem of Record is extends to related maxBitLen
void StackMaps::emitCangjieCompressedStackMaps(MCStreamer &OS) {
  MCContext &OutContext = OS.getContext();
  unsigned CSIdxStart = 0;
  unsigned CSIdxEnd = 0;
  const Triple TT(AP.MMI->getModule()->getTargetTriple());
  bool IsWindows = TT.isOSWindows();
  OffsetStepSize = TT.isARM() ? 4 : 8;
  FuncPtrSize = TT.isARM() ? 4 : 8;
  for (auto const &FR : FnInfos) {
    MCSymbol *StackmapFunction =
        OutContext.getOrCreateSymbol(".Lstack_map." + FR.first->getName());
    OS.emitLabel(StackmapFunction);
    CSIdxEnd = CSIdxStart + FR.second.RecordCount;
    CompressedInfo Data(
        (IsWindows && isX86_64()) ? X86WinCalleeSavedReg
            : isX86_64() ? X86CalleeSavedReg 
            : isAArch64() ? AArch64CalleeSavedReg : ARMCalleeSavedReg);
    prepareCompressedData(Data, FR.second, CSIdxStart, CSIdxEnd);
    emitCangjieCompressedData(OS, Data);
    CSIdxStart = CSIdxEnd;
  }
  OS.addBlankLine();
  reset();
}

static void setPrologueInfo(CompressedInfo &Data,
                            const StackMaps::FunctionInfo &FnInfo) {
  if (FnInfo.StackSize == UINT64_MAX) {
    Data.StackSize = 0; // lea offset(%rbp) rsp, stack back
  } else {
    Data.StackSize = FnInfo.StackSize;
  }
  for (const auto &CS : FnInfo.CSReg2Stack) {
    Data.CalleeSavedReg |= Data.CSRegMap.at(CS.first);
    int Offset = CS.second / OffsetStepSize;
    Offset = (Offset > 0) ? Offset : -Offset;
    Data.CalleeSavedOffsets.emplace_back(Offset);
  }
  return;
}

static void
parseCallsiteRefs(const StackMaps::CallsiteInfo &CSI,
                  std::map<StackMaps::Location, std::set<StackMaps::Location>>
                      &Base2Derived) {
  if (CSI.RefPairs.size() % 2 != 0) {
    report_fatal_error("Base and Derived Ptrs should be paired!");
  }
  for (unsigned I = 0, J = CSI.RefPairs.size(); I < J;) {
    if (CSI.RefPairs[I] == CSI.RefPairs[I + 1]) {
      // insert empty derived if base and derived are the same
      Base2Derived[CSI.RefPairs[I]].insert({});
    } else {
      Base2Derived[CSI.RefPairs[I]].insert(CSI.RefPairs[I + 1]);
    }
    I += 2; // ptr comes in <base, derived> pair.
  }
  // FO doesn't record deriveds
  for (const auto &FOLoc : CSI.FOLocations) {
    Base2Derived.insert({FOLoc, {}});
  }
}

namespace {
// this func gets base ptrs.
StackMaps::Location
getLocation(std::map<StackMaps::Location,
                     std::set<StackMaps::Location>>::const_iterator Itr) {
  return Itr->first;
}
// this func gets derived ptrs.
StackMaps::Location
getLocation(std::set<StackMaps::Location>::const_iterator Itr) {
  return *Itr;
}

struct MaxWidthOfRefInfo {
  unsigned RegBit;
  unsigned BaseOffsetBytes;
  unsigned SlotBitIdx;
  unsigned CompressedSlotBit;
};

void calculateStackSlots(MaxWidthOfRefInfo &WidthInfo,
                         CompressedInfo::SlotItem &StackSlot,
                         const SmallVector<int64_t, 8> &BOffsets,
                         int64_t MaxOffset, int64_t MinOffset) {
  StackSlot.BaseOffset = MaxOffset;
  uint32_t MaxBitIdx = (MaxOffset - MinOffset) / OffsetStepSize;
  WidthInfo.SlotBitIdx = std::max(WidthInfo.SlotBitIdx, MaxBitIdx);
  // 64: uint64_t
  uint32_t SlotBitVecSize = (MaxBitIdx / 64) + 1;
  StackSlot.SlotBit = std::vector<uint64_t>(SlotBitVecSize, 0);

  uint32_t OriDataVecSize = (MaxBitIdx / RawDataWidth) + 1;
  auto OriDataVec = std::vector<uint32_t>(OriDataVecSize, 0);

  for (const auto &Offset : BOffsets) {
    if (Offset % OffsetStepSize != 0) {
      report_fatal_error("Offset should be aligned!");
    }
    uint32_t BitIdx = (MaxOffset - Offset) / OffsetStepSize;
    // 64: uint64_t
    StackSlot.SlotBit[BitIdx / 64] |= ((uint64_t)1 << (BitIdx % 64));
    // prepare origion data. highest bit is always keep zero here.
    OriDataVec[BitIdx / RawDataWidth] |=
        ((uint32_t)1 << (BitIdx % RawDataWidth));
  }
  // compress origion data
  uint32_t AllZeroCnt = 0;
  uint32_t AllOneCnt = 0;
  unsigned CompressedBitCnt = 0;
  auto PushCompressedValue = [&](uint32_t &Value, uint32_t Flag) {
    // 2: 2 bits for compressed fowarding bit
    CompressedBitCnt += getVarIntBitNumsForUInt(Value) + 2;
    StackSlot.CompressedSlotBit.push_back(Value | Flag);
    if (Value >= (uint32_t)1 << 30) {
      report_fatal_error("unsupport AllOneCnt/ AllZeroCnt now!\n");
    }
    Value = 0;
  };

  for (unsigned I = 0; I < OriDataVecSize; ++I) {
    if (OriDataVec[I] == 0) {
      if (AllOneCnt != 0) {
        PushCompressedValue(AllOneCnt, 0x40000000);
      }
      AllZeroCnt++;
    } else if (OriDataVec[I] == 0x7fffffff) {
      if (AllZeroCnt != 0) {
        PushCompressedValue(AllZeroCnt, 0);
      }
      AllOneCnt++;
    } else {
      if (AllOneCnt != 0) {
        PushCompressedValue(AllOneCnt, 0x40000000);
      }
      if (AllZeroCnt != 0) {
        PushCompressedValue(AllZeroCnt, 0);
      }
      // + 1: 1 bit to indicate value is not compressed
      if (I != OriDataVecSize - 1) {
        CompressedBitCnt += RawDataWidth + 1;
      } else {
        CompressedBitCnt += getValidBitNums(OriDataVec[I]) + 1;
      }

      StackSlot.CompressedSlotBit.push_back(OriDataVec[I] | 0x80000000);
    }
  }
  if (AllOneCnt != 0) {
    PushCompressedValue(AllOneCnt, 0x40000000);
  }
  WidthInfo.CompressedSlotBit = std::max(WidthInfo.CompressedSlotBit,
                                         CompressedBitCnt);
}

// Process base when traversing map and process derivation when traversing set
template <typename T>
std::pair<unsigned, unsigned>
addItemInfo(CompressedInfo &Data, const StackMaps::CallsiteInfo &CSI,
            MaxWidthOfRefInfo &WidthInfo, T &Input) {
  auto Itr = Input.cbegin();
  auto EndItr = Input.cend();
  int64_t MaxOffset = INT64_MIN;
  int64_t MinOffset = INT64_MAX;
  SmallVector<int64_t, 8> BOffsets;
  CompressedInfo::RegItem RegInfo{0};
  CompressedInfo::SlotItem StackSlot{0, {0}, {}};
  while (Itr != EndItr) {
    StackMaps::Location Loc = getLocation(Itr);
    if (Loc.Type == StackMaps::Location::Register) {
      // Record all references at yield point and record called saved only at
      // other points
      if (CSI.RecordAllRefInReg ||
          Data.CSRegMap.find(Loc.Reg) != Data.CSRegMap.end()) {
        RegInfo.RegBit |= 1 << Loc.Reg;
      }
    } else {
      BOffsets.push_back(Loc.Offset);
      MaxOffset = std::max(Loc.Offset, MaxOffset);
      MinOffset = std::min(Loc.Offset, MinOffset);
    }
    ++Itr;
  }

  WidthInfo.RegBit = std::max(WidthInfo.RegBit, RegInfo.RegBit);
  // 64: use uint64_t to store bit value.
  if (!BOffsets.empty()) {
    calculateStackSlots(WidthInfo, StackSlot, BOffsets, MaxOffset, MinOffset);
  }
  unsigned BaseOffsetBytes = getMinBytesForInt(StackSlot.BaseOffset);
  WidthInfo.BaseOffsetBytes = std::max(WidthInfo.BaseOffsetBytes,
                                       BaseOffsetBytes);
  unsigned RegIdxPlusOne = Data.RegItems.getOrInsertIndex(RegInfo);
  unsigned SlotIdxPlusOne = Data.SlotItems.getOrInsertIndex(StackSlot);
  return std::make_pair(RegIdxPlusOne, SlotIdxPlusOne);
}
} // end anonymous namespace

static void genStackMapInfo(CompressedInfo &Data,
                            const StackMaps::CallsiteInfo &CSI,
                            MaxWidthOfRefInfo &WidthInfo) {
  // <base, <derives>>. use map and set to keep order
  std::map<StackMaps::Location, std::set<StackMaps::Location>> Base2Derived;
  parseCallsiteRefs(CSI, Base2Derived);

  CompressedInfo::IdxItem &IdxInfo = Data.StackMapItem.back().second;
  // pass map<base, <derives>> to addItemInfo to process base ptrs
  std::pair<unsigned, unsigned> RefIdx =
      addItemInfo(Data, CSI, WidthInfo, Base2Derived);
  IdxInfo.RegIdxPlusOne = RefIdx.first;
  IdxInfo.SlotIdxPlusOne = RefIdx.second;

  bool IsAllIdxsInvalid = true;
  // <regIdxPlusOne, slotIdxPlusOne>
  std::vector<std::pair<unsigned, unsigned>> IdxsInfo;
  auto Itr = Base2Derived.cbegin();
  auto EndItr = Base2Derived.cend();
  while (Itr != EndItr) {
    // pass each base's set<derives> to addItemInfo to process derived ptrs
    std::pair<unsigned, unsigned> DerivedRefIdx =
        addItemInfo(Data, CSI, WidthInfo, Itr->second);
    if (DerivedRefIdx.first != 0 || DerivedRefIdx.second != 0) {
      IsAllIdxsInvalid = false;
    }
    IdxsInfo.emplace_back(DerivedRefIdx);
    ++Itr;
  }
  if (IsAllIdxsInvalid) {
    IdxInfo.DerivedInfoStartIdx = 0;
  } else {
    IdxInfo.DerivedInfoStartIdx = Data.getOrInsertDerivedInfoStartIdx(IdxsInfo);
  }

  if (EnableStackGrow) {
    // The set of the Stack Ptrs Locations.
    std::set<StackMaps::Location> SPLocs(CSI.StackLocations.begin(),
                                         CSI.StackLocations.end());
    std::pair<unsigned, unsigned> StackPtrIdx =
        addItemInfo(Data, CSI, WidthInfo, SPLocs);
    IdxInfo.SPRegIdxPlusOne = StackPtrIdx.first;
    IdxInfo.SPSlotIdxPlusOne = StackPtrIdx.second;
  }
}

void StackMaps::prepareCompressedData(CompressedInfo &Data,
                                      const FunctionInfo &FnInfo,
                                      unsigned CSIdx, unsigned CSIdxEnd) const {
  // prologue
  setPrologueInfo(Data, FnInfo);
  // 1: baseoffset occupies at least 1 byte because it may be a negative number
  MaxWidthOfRefInfo WidthInfo{0, 1, 0, 0};
  unsigned MaxLN = 0;
  // CallSiteItem format:
  // callsite label
  // RegIdx, SlotIdx, LNIdx, DerivedStartIdx, SPRegIdx, SPSlotIdx
  while (CSIdx < CSIdxEnd) {
    const auto &CSI = CSInfos[CSIdx++];
    Data.StackMapItem.insert(
        std::make_pair(CSI.CSOffsetExpr, CompressedInfo::IdxItem()));
    genStackMapInfo(Data, CSI, WidthInfo);

    CompressedInfo::LineNumberItem LineNumber{CSI.LineNumber};
    MaxLN = (MaxLN > LineNumber.LN) ? MaxLN : LineNumber.LN;
    Data.StackMapItem.back().second.LNIdxPlusOne =
        Data.LNItems.getOrInsertIndex(LineNumber);
  }
  // stackMapItem, the data width of each column is recorded based on the
  // maximum size.
  Data.MaxBits.RegIdx = getValidBitNums(Data.RegItems.Items.size());
  Data.MaxBits.SlotIdx = getValidBitNums(Data.SlotItems.Items.size());
  Data.MaxBits.LNIdx = getValidBitNums(Data.LNItems.Items.size());
  Data.MaxBits.DerivedIdx = getValidBitNums(Data.DerivedInfo.size());
  if (EnableStackGrow) {
    Data.MaxBits.SPRegIdx = Data.MaxBits.RegIdx;
    Data.MaxBits.SPSlotIdx = Data.MaxBits.SlotIdx;
    // add paddingBits to SPSlotIdx since PC should be byte (8 bits) aligned
    uint32_t PaddingBits =
        (8 - ((Data.MaxBits.RegIdx + Data.MaxBits.SlotIdx + Data.MaxBits.LNIdx +
               Data.MaxBits.DerivedIdx + Data.MaxBits.SPRegIdx +
               Data.MaxBits.SPSlotIdx) %
              8)) %
        8;
    Data.MaxBits.SPSlotIdx += PaddingBits;
  } else {
    // add paddingBits to DerivedIdx since PC should be byte (8 bits) aligned
    uint32_t PaddingBits =
        (8 - ((Data.MaxBits.RegIdx + Data.MaxBits.SlotIdx + Data.MaxBits.LNIdx +
               Data.MaxBits.DerivedIdx) %
              8)) %
        8;
    Data.MaxBits.DerivedIdx += PaddingBits;
  }

  // RegItem
  Data.MaxBits.RegBit = getValidBitNums(WidthInfo.RegBit);
  // SlotItem. 8: 8 bits per byte
  Data.MaxBits.BaseOffset = WidthInfo.BaseOffsetBytes * 8;
  Data.MaxBits.SlotBit = WidthInfo.SlotBitIdx + 1;
  // 1024: a empirical value of stacksize.
  if (EnableCompressedBitMap && Data.StackSize > 1024 &&
      WidthInfo.CompressedSlotBit != 0 &&
      Data.MaxBits.SlotBit > WidthInfo.CompressedSlotBit) {
    Data.FormatType = CJ_STACKMAP_COMPRESSED_BITMAP;
    Data.MaxBits.SlotBit = WidthInfo.CompressedSlotBit;
  } else {
    Data.FormatType = CJ_STACKMAP_BITMAP;
  }
  // LineNumberItem
  Data.MaxBits.LN = getValidBitNums(MaxLN);
  return;
}

// =========================
// varInt StackSize
// varInt FormatVersion
// varInt CalleeSaveReg
// varInt CalleeSaveOffsets[]
// =========================
// varInt StackMapItemNums
// varInt RegIdx
// varInt SlotIdx
// varInt LNIdx
// varInt DerivedIdx (EnableCJCopyGC)
// varInt SPRegIdx (EnableStackGrow)
// varInt SPSlotIdx (EnableStackGrow)
// varInt PaddingBits
// bits[PaddingBits]
// StackMapItemNums * {PC, RegIdxPlusOne, SlotIdxPlusOne, LNIdxPlusOne,
//                     DerivedInfoStartIdx(EnableCJCopyGC),
//                     SPRegIdxPlusOne(EnableStackGrow),
//                     SPSlotIdxPlusOne(EnableStackGrow)}
// =========================
// varInt RegInfosNums
// varInt RegBit
// RegInfosNums * {RegBit}
// =========================
// varInt StackSlotsNums
// varInt BaseOffset
// varInt SlotBit
// StackSlotsNums * {BaseOffset, SlotBit}
// =========================
// varInt LineNumbersNums
// varInt LNBit
// LineNumbersNums * {LN}
// ========================= (EnableCJCopyGC)
// varInt DerivedInfoNums
// DerivedInfoNums * {RegIdxPlusOne, SlotIdxPlusOne,}
void StackMaps::emitCangjieCompressedData(MCStreamer &OS,
                                          const CompressedInfo &Data) const {
  const Triple TT(AP.MMI->getModule()->getTargetTriple());
  bool IsWindows = TT.isOSWindows();
  DataEncoder Writer(
      OS, Data,
      ((IsWindows && isX86_64()) ? X86WinPrologueBit2Reg
          : isX86_64() ? X86PrologueBit2Reg 
          : isAArch64() ? AArch64PrologueBit2Reg : ARMPrologueBit2Reg),
      (isX86_64() ? X86Bit2Reg 
          : isAArch64() ? AArch64Bit2Reg : ARMBit2Reg));
  Writer.emitPrologueAndStackMapItemHeader();
  // emit PC breaks the consistency of the emit buffer
  Writer.emitStackMapItem();
  if (!Data.StackMapItem.empty()) {
    // Now we can write all the data before we emit the buffer.
    Writer.writeRegRefAndSlotRef();
    Writer.writeLineNumber();
    Writer.writeDerivedInfo();
    Writer.emitCommentForDataAfterStackMapItem();
  }
  Writer.emitBufferContent();
  return;
}

// varInt CalleeSaveReg
// varInt CalleeSaveOffsets[]
// =========================
// varInt StackMapItemNums
// varInt RegIdx
// varInt SlotIdx
// varInt LNIdx
// varInt DerivedIdx (EnableCJCopyGC)
// varInt SPRegIdx (EnableStackGrow)
// varInt SPSlotIdx (EnableStackGrow)
// varInt PaddingBits
void DataEncoder::emitPrologueAndStackMapItemHeader() {
  writeVarUint(Data.StackSize);
  writeVarUint(EnableCompressedBitMap ? Data.FormatType : CJ_STACKMAP_BITMAP);

  writeVarUint(Data.CalleeSavedReg);
  for (auto Offset : Data.CalleeSavedOffsets) {
    writeVarUint(Offset);
  }
  writeVarUint(Data.StackMapItem.size());
  if (!Data.StackMapItem.empty()) {
    writeVarUint(Data.MaxBits.RegIdx);
    writeVarUint(Data.MaxBits.SlotIdx);
    writeVarUint(Data.MaxBits.LNIdx);
    writeVarUint(Data.MaxBits.DerivedIdx);
    if (EnableStackGrow) {
      writeVarUint(Data.MaxBits.SPRegIdx);
      writeVarUint(Data.MaxBits.SPSlotIdx);
    }
  }
  writeBitsToPadding();
  // emitBuffer since PC should be Byte aligned and can't write into buffer
  emitCommentForPrologueAndStackMapItemHeader();
  emitBufferContent();
}

// StackMapItemNums * {PC, RegIdxPlusOne, SlotIdxPlusOne, LNIdxPlusOne,
//                     DerivedInfoStartIdx(EnableCJCopyGC),
//                     SPRegIdxPlusOne(EnableStackGrow),
//                     SPSlotIdxPlusOne(EnableStackGrow)}
void DataEncoder::emitStackMapItem() {
  for (const auto &StackMap : Data.StackMapItem) {
    // emit PC breaks the consistency of the emit buffer
    OS.emitValue(StackMap.first, 4);
    writeBits(Data.MaxBits.RegIdx, StackMap.second.RegIdxPlusOne);
    writeBits(Data.MaxBits.SlotIdx, StackMap.second.SlotIdxPlusOne);
    writeBits(Data.MaxBits.LNIdx, StackMap.second.LNIdxPlusOne);
    writeBits(Data.MaxBits.DerivedIdx, StackMap.second.DerivedInfoStartIdx);

    if (EnableStackGrow) {
      writeBits(Data.MaxBits.SPRegIdx, StackMap.second.SPRegIdxPlusOne);
      writeBits(Data.MaxBits.SPSlotIdx, StackMap.second.SPSlotIdxPlusOne);
    }
    emitCommentForStackMapItem();
    emitBufferContent();
  }
}

// varInt RegInfosNums
// varInt RegBit
// RegInfosNums * {RegBit}
// =========================
// varInt SlotsNums
// varInt BaseOffset
// varInt SlotBit
// SlotsNums * {BaseOffset, SlotBit}
void DataEncoder::writeRegRefAndSlotRef() {
  // skip index 0 since it is invalid
  writeVarUint(Data.RegItems.Items.size() - 1);
  writeVarUint(Data.MaxBits.RegBit);
  for (unsigned I = 1, E = Data.RegItems.Items.size(); I < E; ++I) {
    writeBits(Data.MaxBits.RegBit, Data.RegItems.Items[I].RegBit);
  }
  // skip index 0 since it is invalid
  writeVarUint(Data.SlotItems.Items.size() - 1);
  writeVarUint(Data.MaxBits.BaseOffset);
  writeVarUint(Data.MaxBits.SlotBit);
  for (unsigned I = 1, E = Data.SlotItems.Items.size(); I < E; ++I) {
    writeBits(Data.MaxBits.BaseOffset, Data.SlotItems.Items[I].BaseOffset);
    if (FormatType == CJ_STACKMAP_BITMAP) {
      writeBitVec(Data.MaxBits.SlotBit, Data.SlotItems.Items[I].SlotBit);
    } else {
      writeCompressedBitVec(Data.MaxBits.SlotBit,
                            Data.SlotItems.Items[I].CompressedSlotBit);
    }
  }
}
// varInt LineNumbersNums
// varInt LNBit
// LineNumbersNums * {LN}
void DataEncoder::writeLineNumber() {
  // skip index 0 since it is invalid
  writeVarUint(Data.LNItems.Items.size() - 1);
  writeVarUint(Data.MaxBits.LN);
  for (unsigned I = 1, E = Data.LNItems.Items.size(); I < E; ++I) {
    writeBits(Data.MaxBits.LN, Data.LNItems.Items[I].LN);
  }
}
// varInt DerivedInfoNums
// DerivedInfoNums * {RegIdxPlusOne, SlotIdxPlusOne,}
void DataEncoder::writeDerivedInfo() {
  writeVarUint(Data.DerivedInfo.size() - 1);
  for (unsigned I = 1, E = Data.DerivedInfo.size(); I < E; ++I) {
    writeBits(Data.MaxBits.RegIdx, Data.DerivedInfo[I].first);
    writeBits(Data.MaxBits.SlotIdx, Data.DerivedInfo[I].second);
  }
}

void DataEncoder::emitBufferContent() {
  if (Buffer.empty()) {
    RemainedBits = 0;
    return;
  }
  unsigned Idx = 0;
  while (Idx < Buffer.size() - 1) {
    OS.emitIntValue(Buffer[Idx++], 8); // 8: 8 bytes of uint64_t
  }
  uint64_t Value = Buffer[Idx];
  // 64: use uint64_t as buffer
  int32_t UsedBits = 64 - RemainedBits;
  while (UsedBits > 0) {
    OS.emitIntValue(Value & 0xFF, 1);
    // 8: 8 bits per Byte
    Value >>= 8;
    UsedBits -= 8;
  }
  // clear Buffer
  reset();
  return;
}

void DataEncoder::emitCommentForPrologueAndStackMapItemHeader() {
  const static std::vector<std::string> StackMapTypeStr = {
      "bit map", "compressed bit map"};
  SmallString<128> Str; // initial vector size
  raw_svector_ostream Comment(Str);
  uint64_t StackSize = readVarUint();
  Comment << "StackSize: " << StackSize;
  FormatType = readVarUint();
  Comment << "\n\t#StackmapFormatType: " << StackMapTypeStr.at(FormatType);
  Comment << "\n\t#CalleeSaveReg: ";
  int32_t CalleeSaveReg = readVarUint();
  int CalleeSaveOffsetsSize = 0;
  (Comment << "(0x").write_hex(CalleeSaveReg) << "=" << CalleeSaveReg << ")";
  for (int I = 0, E = PrologueBit2Reg.size(); I < E; ++I) {
    if (CalleeSaveReg & (1 << I)) {
      Comment << ", " << PrologueBit2Reg.at(I);
      CalleeSaveOffsetsSize++;
    }
  }
  Comment << ", offsets(without sign): [";
  if (CalleeSaveOffsetsSize != 0) {
    for (int I = 0; I < CalleeSaveOffsetsSize - 1; ++I) {
      Comment << (readVarUint() * OffsetStepSize) << ", ";
    }
    Comment << (readVarUint() * OffsetStepSize);
  }
  uint32_t StackMapItemNum = readVarUint();
  Comment << "]\n\t#StackMapItem nums:" << StackMapItemNum;
  OS.emitRawComment(Comment.str());

  if (StackMapItemNum > 0) {
    MaxBits.RegIdx = readVarUint();
    MaxBits.SlotIdx = readVarUint();
    MaxBits.LNIdx = readVarUint();
    MaxBits.DerivedIdx = readVarUint();
    if (EnableStackGrow) {
      MaxBits.SPRegIdx = readVarUint();
      MaxBits.SPSlotIdx = readVarUint();
    }
  }
  uint64_t SkipBitNums = readVarUint();
  if (SkipBitNums != 0) {
    readBits(SkipBitNums);
  }
}

void DataEncoder::emitCommentForStackMapItem() {
  SmallString<128> Str; // initial vector size
  raw_svector_ostream Comment(Str);
  int32_t RegIdx = readBits(MaxBits.RegIdx) - 1;
  int32_t SlotIdx = readBits(MaxBits.SlotIdx) - 1;
  int32_t LNIdx = readBits(MaxBits.LNIdx) - 1;
  int32_t DerivedStartIdx = readBits(MaxBits.DerivedIdx) - 1;
  if (EnableStackGrow) {
    int32_t SPRegIdx = readBits(MaxBits.SPRegIdx) - 1;
    int32_t SPSlotIdx = readBits(MaxBits.SPSlotIdx) - 1;
    Comment << "[RegIdx: " << RegIdx << ", SlotIdx: " << SlotIdx
            << ", LNIdx: " << LNIdx << ", DerivedStartIdx: " << DerivedStartIdx
            << ", SPRegIdx: " << SPRegIdx << ", SPSlotIdx: " << SPSlotIdx << "]";
  } else {
    Comment << "[RegIdx: " << RegIdx << ", SlotIdx: " << SlotIdx
            << ", LNIdx: " << LNIdx << ", DerivedStartIdx: " << DerivedStartIdx
            << "]";
  }
  OS.emitRawComment(Comment.str());
}

void DataEncoder::emitCommentForSlots(raw_svector_ostream &Comment,
                                      unsigned int I) {
  int32_t BaseOffset = readBitsAsInt(MaxBits.BaseOffset);
  auto EmitSlotBitComment = [&](uint64_t SlotBit, unsigned Width) {
    (Comment << " 0x").write_hex(SlotBit) << "[";
    for (unsigned Idx = 0; Idx < Width; ++Idx) { // 64: uint64_t
      if (SlotBit & ((uint64_t)1 << Idx)) {
        // Each bit represents 4 or 8 Byte offsets
        int32_t Off = BaseOffset - Idx * FuncPtrSize;
        Comment << " " << Off;
      }
    }
    Comment << " ]";
  };

  Comment << "\n\t\t#Idx[" << I << "]: BaseOffset: " << BaseOffset
          << ", SlotBits:";
  int32_t SlotBitBitNums = (int32_t)MaxBits.SlotBit;
  if (FormatType == CJ_STACKMAP_BITMAP) {
    // 64: use uint64_t to store info
    while (SlotBitBitNums > 64) {
      SlotBitBitNums -= 64;
      EmitSlotBitComment(readBits(64), 64);
      BaseOffset -= 64 * FuncPtrSize; // each bit represents 4/8 bytes
    }
    EmitSlotBitComment(readBits(SlotBitBitNums), SlotBitBitNums);
  } else {
    uint64_t IsOrginalVal;
    uint64_t AllOne;
    uint64_t Cnts;
    while (SlotBitBitNums > 0) {
      IsOrginalVal = readBits(1);
      SlotBitBitNums--;
      if (IsOrginalVal) {
        int32_t EmitBits = std::min(SlotBitBitNums, RawDataWidth);
        EmitSlotBitComment(readBits(EmitBits), EmitBits);
        SlotBitBitNums -= EmitBits;
        BaseOffset -= EmitBits * 8;
      } else {
        // bits are padding bits if remain bits are less than var int bits + 1
        if (SlotBitBitNums < SingleVarIntBits + 1) {
          readBits(SlotBitBitNums);
          break;
        }
        AllOne = readBits(1);
        SlotBitBitNums--;
        Cnts = readVarUint();
        SlotBitBitNums -= getVarIntBitNumsForUInt(Cnts);
        assert(SlotBitBitNums >= 0 && "slot bit nums must >= 0!\n");
        // Cnts == 0 means we are in padding bits. read remain bits and break
        if (Cnts == 0) {
          while (SlotBitBitNums > 64) {
            readBits(64);
            SlotBitBitNums -= 64;
          }
          readBits(SlotBitBitNums);
          break;
        }
        Comment << (AllOne ? " AllOnes(Ref)[" : " AllZeros(Val)[") << BaseOffset
                << "..."
                << (BaseOffset - (int32_t)(Cnts * RawDataWidth - 1) * 8)
                << " ]";
        BaseOffset -= Cnts * RawDataWidth * 8;
      }
    }
  }
  return;
}

void DataEncoder::emitCommentForRegsAndSlots() {
  SmallString<128> Str; // initial vector size
  raw_svector_ostream Comment(Str);
  uint32_t RegNums = readVarUint();
  Comment << "RegNums: " << RegNums;
  MaxBits.RegBit = readVarUint();
  for (unsigned I = 0; I < RegNums; ++I) {
    uint32_t Reg = readBits(MaxBits.RegBit);
    (Comment << "\n\t\t#Idx[" << I << "]: (0x").write_hex(Reg)
        << "=" << Reg << ")";
    for (int J = 0; J < 32; ++J) { // 32: uint32_t
      if (Reg & (1 << J)) {
        Comment << ", " << Bit2RegStr.at(J);
      }
    }
  }
  uint32_t SlotsNums = readVarUint();
  Comment << "\n\t#SlotsNums: " << SlotsNums;
  MaxBits.BaseOffset = readVarUint();
  MaxBits.SlotBit = readVarUint();
  for (unsigned I = 0; I < SlotsNums; ++I) {
    emitCommentForSlots(Comment, I);
  }
  OS.emitRawComment(Comment.str());
}

void DataEncoder::emitCommentForLineNumber() {
  SmallString<128> Str; // initial vector size
  raw_svector_ostream Comment(Str);
  uint32_t LineNumbersNums = readVarUint();
  Comment << "LineNumbersNums: " << LineNumbersNums;
  MaxBits.LN = readVarUint();
  for (unsigned I = 0; I < LineNumbersNums; ++I) {
    Comment << "\n\t\t#Idx[" << I << "]: " << readBits(MaxBits.LN);
  }
  OS.emitRawComment(Comment.str());
}

void DataEncoder::emitCommentForDerivedInfo() {
  SmallString<128> Str; // initial vector size
  raw_svector_ostream Comment(Str);
  uint32_t DerivedInfoNums = readVarUint();
  Comment << "DerivedInfoNums: " << DerivedInfoNums;
  for (unsigned I = 0; I < DerivedInfoNums; ++I) {
    int32_t RegIdx = readBits(MaxBits.RegIdx) - 1;
    int32_t SlotIdx = readBits(MaxBits.SlotIdx) - 1;
    Comment << "\n\t\t#Idx[" << I << "]: RegIdx: " << RegIdx
            << ", SlotIdx: " << SlotIdx;
  }
  OS.emitRawComment(Comment.str());
}

void DataEncoder::emitCommentForDataAfterStackMapItem() {
  emitCommentForRegsAndSlots();
  emitCommentForLineNumber();
  emitCommentForDerivedInfo();
}
