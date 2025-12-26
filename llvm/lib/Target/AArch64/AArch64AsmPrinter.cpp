//===- AArch64AsmPrinter.cpp - AArch64 LLVM assembly writer ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to the AArch64 assembly language.
//
//===----------------------------------------------------------------------===//

#include "AArch64.h"
#include "AArch64MCInstLower.h"
#include "AArch64MachineFunctionInfo.h"
#include "AArch64RegisterInfo.h"
#include "AArch64Subtarget.h"
#include "AArch64TargetObjectFile.h"
#include "MCTargetDesc/AArch64AddressingModes.h"
#include "MCTargetDesc/AArch64InstPrinter.h"
#include "MCTargetDesc/AArch64MCExpr.h"
#include "MCTargetDesc/AArch64MCTargetDesc.h"
#include "MCTargetDesc/AArch64TargetStreamer.h"
#include "TargetInfo/AArch64TargetInfo.h"
#include "Utils/AArch64BaseInfo.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Triple.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/CJMetadata.h"
#include "llvm/CodeGen/FaultMaps.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachineModuleInfoImpls.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/StackMaps.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Statepoint.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Instrumentation/HWAddressSanitizer.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>

using namespace llvm;

#define DEBUG_TYPE "asm-printer"

namespace llvm {
extern cl::opt<bool> CJPipeline;
extern cl::opt<bool> EnableStackGrow;
}
namespace {

class AArch64AsmPrinter : public AsmPrinter {
  AArch64MCInstLower MCInstLowering;
  StackMaps SM;
  CJMetadataInfo CMI;
  FaultMaps FM;
  const AArch64Subtarget *STI;
  bool ShouldEmitWeakSwiftAsyncExtendedFramePointerFlags = false;

public:
  AArch64AsmPrinter(TargetMachine &TM, std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer)), MCInstLowering(OutContext, *this),
        SM(*this), CMI(*this, SM), FM(*this) {
    SM.setAArch64();
  }

  StringRef getPassName() const override { return "AArch64 Assembly Printer"; }

  /// Wrapper for MCInstLowering.lowerOperand() for the
  /// tblgen'erated pseudo lowering.
  bool lowerOperand(const MachineOperand &MO, MCOperand &MCOp) const {
    return MCInstLowering.lowerOperand(MO, MCOp);
  }

  void emitStartOfAsmFile(Module &M) override;
  void emitJumpTableInfo() override;

  void emitFunctionEntryLabel() override;

  void LowerJumpTableDest(MCStreamer &OutStreamer, const MachineInstr &MI);

  void LowerMOPS(MCStreamer &OutStreamer, const MachineInstr &MI);

  void LowerSTACKMAP(MCStreamer &OutStreamer, StackMaps &SM,
                     const MachineInstr &MI);
  void LowerPATCHPOINT(MCStreamer &OutStreamer, StackMaps &SM,
                       const MachineInstr &MI);
  bool LowerCangjieSpecificTCRETUR(MCStreamer &OutStreamer, StackMaps &SM,
                                  const MachineInstr &MI);
  void LowerSTATEPOINT(MCStreamer &OutStreamer, StackMaps &SM,
                       const MachineInstr &MI);
  void LowerFAULTING_OP(const MachineInstr &MI);

  void LowerPATCHABLE_FUNCTION_ENTER(const MachineInstr &MI);
  void LowerPATCHABLE_FUNCTION_EXIT(const MachineInstr &MI);
  void LowerPATCHABLE_TAIL_CALL(const MachineInstr &MI);

  typedef std::tuple<unsigned, bool, uint32_t> HwasanMemaccessTuple;
  std::map<HwasanMemaccessTuple, MCSymbol *> HwasanMemaccessSymbols;
  void LowerHWASAN_CHECK_MEMACCESS(const MachineInstr &MI);
  void emitHwasanMemaccessSymbols(Module &M);

  void emitSled(const MachineInstr &MI, SledKind Kind);

  /// tblgen'erated driver function for lowering simple MI->MC
  /// pseudo instructions.
  bool emitPseudoExpansionLowering(MCStreamer &OutStreamer,
                                   const MachineInstr *MI);

  void emitInstruction(const MachineInstr *MI) override;

  void emitFunctionHeaderComment() override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AsmPrinter::getAnalysisUsage(AU);
    AU.setPreservesAll();
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    AArch64FI = MF.getInfo<AArch64FunctionInfo>();
    STI = &MF.getSubtarget<AArch64Subtarget>();

    SetupMachineFunction(MF);

    if (STI->isTargetCOFF()) {
      bool Internal = MF.getFunction().hasInternalLinkage();
      COFF::SymbolStorageClass Scl = Internal ? COFF::IMAGE_SYM_CLASS_STATIC
                                              : COFF::IMAGE_SYM_CLASS_EXTERNAL;
      int Type =
        COFF::IMAGE_SYM_DTYPE_FUNCTION << COFF::SCT_COMPLEX_TYPE_SHIFT;

      OutStreamer->beginCOFFSymbolDef(CurrentFnSym);
      OutStreamer->emitCOFFSymbolStorageClass(Scl);
      OutStreamer->emitCOFFSymbolType(Type);
      OutStreamer->endCOFFSymbolDef();
    }

    // Emit the rest of the function body.
    emitFunctionBody();

    // Emit the XRay table for this function.
    emitXRayTable();

    CMI.recordCurrentFunc();

    // We didn't modify anything.
    return false;
  }

  void emitMetadataAddress() override;

private:
  void printOperand(const MachineInstr *MI, unsigned OpNum, raw_ostream &O);
  bool printAsmMRegister(const MachineOperand &MO, char Mode, raw_ostream &O);
  bool printAsmRegInClass(const MachineOperand &MO,
                          const TargetRegisterClass *RC, unsigned AltName,
                          raw_ostream &O);

  bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNum,
                       const char *ExtraCode, raw_ostream &O) override;
  bool PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNum,
                             const char *ExtraCode, raw_ostream &O) override;

  void PrintDebugValueComment(const MachineInstr *MI, raw_ostream &OS);

  void emitFunctionBodyEnd() override;

  MCSymbol *GetCPISymbol(unsigned CPID) const override;
  void emitEndOfAsmFile(Module &M) override;

  AArch64FunctionInfo *AArch64FI = nullptr;

  /// Emit the LOHs contained in AArch64FI.
  void emitLOHs();

  /// Emit instruction to set float register to zero.
  void emitFMov0(const MachineInstr &MI);

  using MInstToMCSymbol = std::map<const MachineInstr *, MCSymbol *>;

  MInstToMCSymbol LOHInstToLabel;

  bool shouldEmitWeakSwiftAsyncExtendedFramePointerFlags() const override {
    return ShouldEmitWeakSwiftAsyncExtendedFramePointerFlags;
  }
  MCOperand setGAAndLower(const MachineOperand &MOSym, const GlobalValue *GV,
                          unsigned Flag = AArch64II::MO_NO_FLAG) const {
    MachineOperand MOAddr(MOSym);
    MCOperand SymAddr;
    MOAddr.ChangeToGA(GV, 0);
    if (Flag != AArch64II::MO_NO_FLAG) {
      MOAddr.setTargetFlags(Flag);
    }
    MCInstLowering.lowerOperand(MOAddr, SymAddr);
    return SymAddr;
  }

  void extendStackAndInsertFFIInfoForJmp(const MCOperand &SymOriAddr,
                                         const MCOperand &SymOriAddrLo12,
                                         unsigned CallFrameSize);

  void emitCangjieCallStubInstImpl(const MachineInstr *MI, const Function *F,
                                   const MachineOperand &MOSym,
                                   unsigned Opcode) override;

  void emitGetCJThreadId() override;
  struct ParamForEmitNewObj {
    ParamForEmitNewObj(const MachineInstr *MIInit,
                       const MachineOperand &MOSymInit, unsigned int OpcodeInit,
                       MCSymbol *LFinish, const MCSymbolRefExpr *FinishExpr,
                       const MCInst &CallSlow, StringRef FastName)
        : MI(MIInit), MOSym(MOSymInit), Opcode(OpcodeInit), LFin(LFinish),
          FinExpr(FinishExpr), CallNewObjSlowPath(CallSlow),
          FastFuncName(FastName) {}
    ~ParamForEmitNewObj() = default;
    const MachineInstr *MI;
    const MachineOperand &MOSym;
    unsigned int Opcode;
    MCSymbol *LFin;
    const MCSymbolRefExpr *FinExpr;
    const MCInst &CallNewObjSlowPath;
    StringRef FastFuncName;
  };

  void emitMccNewObjectForCopyGC(const ParamForEmitNewObj &Param);
  void emitMccNewObjectFastPath(const MachineInstr *MI,
                                const MachineOperand &MOSym,
                                unsigned Opcode) override;
  void emitCJNewArrayFastPath(const MachineInstr &MI,
                              const MachineOperand &MOSym) override;
  void emitCJNewArrayFastPath(const MachineOperand &MOSym);
  void emitCJThrowException(const MachineInstr *MI,
                            const MachineOperand &MOSym,
                            unsigned Opcode) override;
  void emitAddSP(unsigned AddSize);
  void emitSubSP(unsigned Size);
  void emitCJStackCheck(const MachineInstr &MI) override;
  int emitStackOverflowCall(const MachineInstr &MI);
  void emitCangjieSafepoint(const MachineInstr &MI) override;
  void emitCangjieSafepoint();
  int emitSafePointDirectCall(unsigned Index) override;
  void emitSafepoint(const MachineInstr &MI);
  void emitGcStateCheck() override;
  void emitGetCJTLSData(int64_t Offset);
};

} // end anonymous namespace

void AArch64AsmPrinter::emitStartOfAsmFile(Module &M) {
  const Triple &TT = TM.getTargetTriple();

  if (TT.isOSBinFormatCOFF()) {
    // Emit an absolute @feat.00 symbol.  This appears to be some kind of
    // compiler features bitfield read by link.exe.
    MCSymbol *S = MMI->getContext().getOrCreateSymbol(StringRef("@feat.00"));
    OutStreamer->beginCOFFSymbolDef(S);
    OutStreamer->emitCOFFSymbolStorageClass(COFF::IMAGE_SYM_CLASS_STATIC);
    OutStreamer->emitCOFFSymbolType(COFF::IMAGE_SYM_DTYPE_NULL);
    OutStreamer->endCOFFSymbolDef();
    int64_t Feat00Flags = 0;

    if (M.getModuleFlag("cfguard")) {
      Feat00Flags |= 0x800; // Object is CFG-aware.
    }

    if (M.getModuleFlag("ehcontguard")) {
      Feat00Flags |= 0x4000; // Object also has EHCont.
    }

    OutStreamer->emitSymbolAttribute(S, MCSA_Global);
    OutStreamer->emitAssignment(
        S, MCConstantExpr::create(Feat00Flags, MMI->getContext()));
  }

  if (!TT.isOSBinFormatELF())
    return;

  // Assemble feature flags that may require creation of a note section.
  unsigned Flags = 0;
  if (const auto *BTE = mdconst::extract_or_null<ConstantInt>(
          M.getModuleFlag("branch-target-enforcement")))
    if (BTE->getZExtValue())
      Flags |= ELF::GNU_PROPERTY_AARCH64_FEATURE_1_BTI;

  if (const auto *Sign = mdconst::extract_or_null<ConstantInt>(
          M.getModuleFlag("sign-return-address")))
    if (Sign->getZExtValue())
      Flags |= ELF::GNU_PROPERTY_AARCH64_FEATURE_1_PAC;

  if (Flags == 0)
    return;

  // Emit a .note.gnu.property section with the flags.
  if (auto *TS = static_cast<AArch64TargetStreamer *>(
          OutStreamer->getTargetStreamer()))
    TS->emitNoteSection(Flags);
}

void AArch64AsmPrinter::emitFunctionHeaderComment() {
  const AArch64FunctionInfo *FI = MF->getInfo<AArch64FunctionInfo>();
  Optional<std::string> OutlinerString = FI->getOutliningStyle();
  if (OutlinerString != None)
    OutStreamer->getCommentOS() << ' ' << OutlinerString;
}

void AArch64AsmPrinter::LowerPATCHABLE_FUNCTION_ENTER(const MachineInstr &MI)
{
  const Function &F = MF->getFunction();
  if (F.hasFnAttribute("patchable-function-entry")) {
    unsigned Num;
    if (F.getFnAttribute("patchable-function-entry")
            .getValueAsString()
            .getAsInteger(10, Num))
      return;
    emitNops(Num);
    return;
  }

  emitSled(MI, SledKind::FUNCTION_ENTER);
}

void AArch64AsmPrinter::LowerPATCHABLE_FUNCTION_EXIT(const MachineInstr &MI) {
  emitSled(MI, SledKind::FUNCTION_EXIT);
}

void AArch64AsmPrinter::LowerPATCHABLE_TAIL_CALL(const MachineInstr &MI) {
  emitSled(MI, SledKind::TAIL_CALL);
}

void AArch64AsmPrinter::emitSled(const MachineInstr &MI, SledKind Kind) {
  static const int8_t NoopsInSledCount = 7;
  // We want to emit the following pattern:
  //
  // .Lxray_sled_N:
  //   ALIGN
  //   B #32
  //   ; 7 NOP instructions (28 bytes)
  // .tmpN
  //
  // We need the 28 bytes (7 instructions) because at runtime, we'd be patching
  // over the full 32 bytes (8 instructions) with the following pattern:
  //
  //   STP X0, X30, [SP, #-16]! ; push X0 and the link register to the stack
  //   LDR W0, #12 ; W0 := function ID
  //   LDR X16,#12 ; X16 := addr of __xray_FunctionEntry or __xray_FunctionExit
  //   BLR X16 ; call the tracing trampoline
  //   ;DATA: 32 bits of function ID
  //   ;DATA: lower 32 bits of the address of the trampoline
  //   ;DATA: higher 32 bits of the address of the trampoline
  //   LDP X0, X30, [SP], #16 ; pop X0 and the link register from the stack
  //
  OutStreamer->emitCodeAlignment(4, &getSubtargetInfo());
  auto CurSled = OutContext.createTempSymbol("xray_sled_", true);
  OutStreamer->emitLabel(CurSled);
  auto Target = OutContext.createTempSymbol();

  // Emit "B #32" instruction, which jumps over the next 28 bytes.
  // The operand has to be the number of 4-byte instructions to jump over,
  // including the current instruction.
  EmitToStreamer(*OutStreamer, MCInstBuilder(AArch64::B).addImm(8));

  for (int8_t I = 0; I < NoopsInSledCount; I++)
    EmitToStreamer(*OutStreamer, MCInstBuilder(AArch64::HINT).addImm(0));

  OutStreamer->emitLabel(Target);
  recordSled(CurSled, MI, Kind, 2);
}

void AArch64AsmPrinter::LowerHWASAN_CHECK_MEMACCESS(const MachineInstr &MI) {
  Register Reg = MI.getOperand(0).getReg();
  bool IsShort =
      MI.getOpcode() == AArch64::HWASAN_CHECK_MEMACCESS_SHORTGRANULES;
  uint32_t AccessInfo = MI.getOperand(1).getImm();
  MCSymbol *&Sym =
      HwasanMemaccessSymbols[HwasanMemaccessTuple(Reg, IsShort, AccessInfo)];
  if (!Sym) {
    // FIXME: Make this work on non-ELF.
    if (!TM.getTargetTriple().isOSBinFormatELF())
      report_fatal_error("llvm.hwasan.check.memaccess only supported on ELF");

    std::string SymName = "__hwasan_check_x" + utostr(Reg - AArch64::X0) + "_" +
                          utostr(AccessInfo);
    if (IsShort)
      SymName += "_short_v2";
    Sym = OutContext.getOrCreateSymbol(SymName);
  }

  EmitToStreamer(*OutStreamer,
                 MCInstBuilder(AArch64::BL)
                     .addExpr(MCSymbolRefExpr::create(Sym, OutContext)));
}

void AArch64AsmPrinter::emitHwasanMemaccessSymbols(Module &M) {
  if (HwasanMemaccessSymbols.empty())
    return;

  const Triple &TT = TM.getTargetTriple();
  assert(TT.isOSBinFormatELF());
  std::unique_ptr<MCSubtargetInfo> STI(
      TM.getTarget().createMCSubtargetInfo(TT.str(), "", ""));
  assert(STI && "Unable to create subtarget info");

  MCSymbol *HwasanTagMismatchV1Sym =
      OutContext.getOrCreateSymbol("__hwasan_tag_mismatch");
  MCSymbol *HwasanTagMismatchV2Sym =
      OutContext.getOrCreateSymbol("__hwasan_tag_mismatch_v2");

  const MCSymbolRefExpr *HwasanTagMismatchV1Ref =
      MCSymbolRefExpr::create(HwasanTagMismatchV1Sym, OutContext);
  const MCSymbolRefExpr *HwasanTagMismatchV2Ref =
      MCSymbolRefExpr::create(HwasanTagMismatchV2Sym, OutContext);

  for (auto &P : HwasanMemaccessSymbols) {
    unsigned Reg = std::get<0>(P.first);
    bool IsShort = std::get<1>(P.first);
    uint32_t AccessInfo = std::get<2>(P.first);
    const MCSymbolRefExpr *HwasanTagMismatchRef =
        IsShort ? HwasanTagMismatchV2Ref : HwasanTagMismatchV1Ref;
    MCSymbol *Sym = P.second;

    bool HasMatchAllTag =
        (AccessInfo >> HWASanAccessInfo::HasMatchAllShift) & 1;
    uint8_t MatchAllTag =
        (AccessInfo >> HWASanAccessInfo::MatchAllShift) & 0xff;
    unsigned Size =
        1 << ((AccessInfo >> HWASanAccessInfo::AccessSizeShift) & 0xf);
    bool CompileKernel =
        (AccessInfo >> HWASanAccessInfo::CompileKernelShift) & 1;

    OutStreamer->switchSection(OutContext.getELFSection(
        ".text.hot", ELF::SHT_PROGBITS,
        ELF::SHF_EXECINSTR | ELF::SHF_ALLOC | ELF::SHF_GROUP, 0, Sym->getName(),
        /*IsComdat=*/true));

    OutStreamer->emitSymbolAttribute(Sym, MCSA_ELF_TypeFunction);
    OutStreamer->emitSymbolAttribute(Sym, MCSA_Weak);
    OutStreamer->emitSymbolAttribute(Sym, MCSA_Hidden);
    OutStreamer->emitLabel(Sym);

    OutStreamer->emitInstruction(MCInstBuilder(AArch64::SBFMXri)
                                     .addReg(AArch64::X16)
                                     .addReg(Reg)
                                     .addImm(4)
                                     .addImm(55),
                                 *STI);
    OutStreamer->emitInstruction(
        MCInstBuilder(AArch64::LDRBBroX)
            .addReg(AArch64::W16)
            .addReg(IsShort ? AArch64::X20 : AArch64::X9)
            .addReg(AArch64::X16)
            .addImm(0)
            .addImm(0),
        *STI);
    OutStreamer->emitInstruction(
        MCInstBuilder(AArch64::SUBSXrs)
            .addReg(AArch64::XZR)
            .addReg(AArch64::X16)
            .addReg(Reg)
            .addImm(AArch64_AM::getShifterImm(AArch64_AM::LSR, 56)),
        *STI);
    MCSymbol *HandleMismatchOrPartialSym = OutContext.createTempSymbol();
    OutStreamer->emitInstruction(
        MCInstBuilder(AArch64::Bcc)
            .addImm(AArch64CC::NE)
            .addExpr(MCSymbolRefExpr::create(HandleMismatchOrPartialSym,
                                             OutContext)),
        *STI);
    MCSymbol *ReturnSym = OutContext.createTempSymbol();
    OutStreamer->emitLabel(ReturnSym);
    OutStreamer->emitInstruction(
        MCInstBuilder(AArch64::RET).addReg(AArch64::LR), *STI);
    OutStreamer->emitLabel(HandleMismatchOrPartialSym);

    if (HasMatchAllTag) {
      OutStreamer->emitInstruction(MCInstBuilder(AArch64::UBFMXri)
                                       .addReg(AArch64::X16)
                                       .addReg(Reg)
                                       .addImm(56)
                                       .addImm(63),
                                   *STI);
      OutStreamer->emitInstruction(MCInstBuilder(AArch64::SUBSXri)
                                       .addReg(AArch64::XZR)
                                       .addReg(AArch64::X16)
                                       .addImm(MatchAllTag)
                                       .addImm(0),
                                   *STI);
      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::Bcc)
              .addImm(AArch64CC::EQ)
              .addExpr(MCSymbolRefExpr::create(ReturnSym, OutContext)),
          *STI);
    }

    if (IsShort) {
      OutStreamer->emitInstruction(MCInstBuilder(AArch64::SUBSWri)
                                       .addReg(AArch64::WZR)
                                       .addReg(AArch64::W16)
                                       .addImm(15)
                                       .addImm(0),
                                   *STI);
      MCSymbol *HandleMismatchSym = OutContext.createTempSymbol();
      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::Bcc)
              .addImm(AArch64CC::HI)
              .addExpr(MCSymbolRefExpr::create(HandleMismatchSym, OutContext)),
          *STI);

      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::ANDXri)
              .addReg(AArch64::X17)
              .addReg(Reg)
              .addImm(AArch64_AM::encodeLogicalImmediate(0xf, 64)),
          *STI);
      if (Size != 1)
        OutStreamer->emitInstruction(MCInstBuilder(AArch64::ADDXri)
                                         .addReg(AArch64::X17)
                                         .addReg(AArch64::X17)
                                         .addImm(Size - 1)
                                         .addImm(0),
                                     *STI);
      OutStreamer->emitInstruction(MCInstBuilder(AArch64::SUBSWrs)
                                       .addReg(AArch64::WZR)
                                       .addReg(AArch64::W16)
                                       .addReg(AArch64::W17)
                                       .addImm(0),
                                   *STI);
      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::Bcc)
              .addImm(AArch64CC::LS)
              .addExpr(MCSymbolRefExpr::create(HandleMismatchSym, OutContext)),
          *STI);

      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::ORRXri)
              .addReg(AArch64::X16)
              .addReg(Reg)
              .addImm(AArch64_AM::encodeLogicalImmediate(0xf, 64)),
          *STI);
      OutStreamer->emitInstruction(MCInstBuilder(AArch64::LDRBBui)
                                       .addReg(AArch64::W16)
                                       .addReg(AArch64::X16)
                                       .addImm(0),
                                   *STI);
      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::SUBSXrs)
              .addReg(AArch64::XZR)
              .addReg(AArch64::X16)
              .addReg(Reg)
              .addImm(AArch64_AM::getShifterImm(AArch64_AM::LSR, 56)),
          *STI);
      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::Bcc)
              .addImm(AArch64CC::EQ)
              .addExpr(MCSymbolRefExpr::create(ReturnSym, OutContext)),
          *STI);

      OutStreamer->emitLabel(HandleMismatchSym);
    }

    OutStreamer->emitInstruction(MCInstBuilder(AArch64::STPXpre)
                                     .addReg(AArch64::SP)
                                     .addReg(AArch64::X0)
                                     .addReg(AArch64::X1)
                                     .addReg(AArch64::SP)
                                     .addImm(-32),
                                 *STI);
    OutStreamer->emitInstruction(MCInstBuilder(AArch64::STPXi)
                                     .addReg(AArch64::FP)
                                     .addReg(AArch64::LR)
                                     .addReg(AArch64::SP)
                                     .addImm(29),
                                 *STI);

    if (Reg != AArch64::X0)
      OutStreamer->emitInstruction(MCInstBuilder(AArch64::ORRXrs)
                                       .addReg(AArch64::X0)
                                       .addReg(AArch64::XZR)
                                       .addReg(Reg)
                                       .addImm(0),
                                   *STI);
    OutStreamer->emitInstruction(
        MCInstBuilder(AArch64::MOVZXi)
            .addReg(AArch64::X1)
            .addImm(AccessInfo & HWASanAccessInfo::RuntimeMask)
            .addImm(0),
        *STI);

    if (CompileKernel) {
      // The Linux kernel's dynamic loader doesn't support GOT relative
      // relocations, but it doesn't support late binding either, so just call
      // the function directly.
      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::B).addExpr(HwasanTagMismatchRef), *STI);
    } else {
      // Intentionally load the GOT entry and branch to it, rather than possibly
      // late binding the function, which may clobber the registers before we
      // have a chance to save them.
      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::ADRP)
              .addReg(AArch64::X16)
              .addExpr(AArch64MCExpr::create(
                  HwasanTagMismatchRef, AArch64MCExpr::VariantKind::VK_GOT_PAGE,
                  OutContext)),
          *STI);
      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::LDRXui)
              .addReg(AArch64::X16)
              .addReg(AArch64::X16)
              .addExpr(AArch64MCExpr::create(
                  HwasanTagMismatchRef, AArch64MCExpr::VariantKind::VK_GOT_LO12,
                  OutContext)),
          *STI);
      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::BR).addReg(AArch64::X16), *STI);
    }
  }
}

void AArch64AsmPrinter::emitEndOfAsmFile(Module &M) {
  emitHwasanMemaccessSymbols(M);

  const Triple &TT = TM.getTargetTriple();
  if (TT.isOSBinFormatMachO()) {
    // Funny Darwin hack: This flag tells the linker that no global symbols
    // contain code that falls through to other global symbols (e.g. the obvious
    // implementation of multiple entry points).  If this doesn't occur, the
    // linker can safely perform dead code stripping.  Since LLVM never
    // generates code that does this, it is always safe to set.
    OutStreamer->emitAssemblerFlag(MCAF_SubsectionsViaSymbols);
  }
  emitCJMetadataInfo(CMI, M);
  // Emit stack and fault map information.
  emitStackMaps(SM);
  FM.serializeToFaultMapSection();

}

void AArch64AsmPrinter::emitLOHs() {
  SmallVector<MCSymbol *, 3> MCArgs;

  for (const auto &D : AArch64FI->getLOHContainer()) {
    for (const MachineInstr *MI : D.getArgs()) {
      MInstToMCSymbol::iterator LabelIt = LOHInstToLabel.find(MI);
      assert(LabelIt != LOHInstToLabel.end() &&
             "Label hasn't been inserted for LOH related instruction");
      MCArgs.push_back(LabelIt->second);
    }
    OutStreamer->emitLOHDirective(D.getKind(), MCArgs);
    MCArgs.clear();
  }
}

void AArch64AsmPrinter::emitFunctionBodyEnd() {
  if (!AArch64FI->getLOHRelated().empty())
    emitLOHs();
}

/// GetCPISymbol - Return the symbol for the specified constant pool entry.
MCSymbol *AArch64AsmPrinter::GetCPISymbol(unsigned CPID) const {
  // Darwin uses a linker-private symbol name for constant-pools (to
  // avoid addends on the relocation?), ELF has no such concept and
  // uses a normal private symbol.
  if (!getDataLayout().getLinkerPrivateGlobalPrefix().empty())
    return OutContext.getOrCreateSymbol(
        Twine(getDataLayout().getLinkerPrivateGlobalPrefix()) + "CPI" +
        Twine(getFunctionNumber()) + "_" + Twine(CPID));

  return AsmPrinter::GetCPISymbol(CPID);
}

void AArch64AsmPrinter::printOperand(const MachineInstr *MI, unsigned OpNum,
                                     raw_ostream &O) {
  const MachineOperand &MO = MI->getOperand(OpNum);
  switch (MO.getType()) {
  default:
    llvm_unreachable("<unknown operand type>");
  case MachineOperand::MO_Register: {
    Register Reg = MO.getReg();
    assert(Register::isPhysicalRegister(Reg));
    assert(!MO.getSubReg() && "Subregs should be eliminated!");
    O << AArch64InstPrinter::getRegisterName(Reg);
    break;
  }
  case MachineOperand::MO_Immediate: {
    O << MO.getImm();
    break;
  }
  case MachineOperand::MO_GlobalAddress: {
    PrintSymbolOperand(MO, O);
    break;
  }
  case MachineOperand::MO_BlockAddress: {
    MCSymbol *Sym = GetBlockAddressSymbol(MO.getBlockAddress());
    Sym->print(O, MAI);
    break;
  }
  }
}

bool AArch64AsmPrinter::printAsmMRegister(const MachineOperand &MO, char Mode,
                                          raw_ostream &O) {
  Register Reg = MO.getReg();
  switch (Mode) {
  default:
    return true; // Unknown mode.
  case 'w':
    Reg = getWRegFromXReg(Reg);
    break;
  case 'x':
    Reg = getXRegFromWReg(Reg);
    break;
  case 't':
    Reg = getXRegFromXRegTuple(Reg);
    break;
  }

  O << AArch64InstPrinter::getRegisterName(Reg);
  return false;
}

// Prints the register in MO using class RC using the offset in the
// new register class. This should not be used for cross class
// printing.
bool AArch64AsmPrinter::printAsmRegInClass(const MachineOperand &MO,
                                           const TargetRegisterClass *RC,
                                           unsigned AltName, raw_ostream &O) {
  assert(MO.isReg() && "Should only get here with a register!");
  const TargetRegisterInfo *RI = STI->getRegisterInfo();
  Register Reg = MO.getReg();
  unsigned RegToPrint = RC->getRegister(RI->getEncodingValue(Reg));
  if (!RI->regsOverlap(RegToPrint, Reg))
    return true;
  O << AArch64InstPrinter::getRegisterName(RegToPrint, AltName);
  return false;
}

bool AArch64AsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNum,
                                        const char *ExtraCode, raw_ostream &O) {
  const MachineOperand &MO = MI->getOperand(OpNum);

  // First try the generic code, which knows about modifiers like 'c' and 'n'.
  if (!AsmPrinter::PrintAsmOperand(MI, OpNum, ExtraCode, O))
    return false;

  // Does this asm operand have a single letter operand modifier?
  if (ExtraCode && ExtraCode[0]) {
    if (ExtraCode[1] != 0)
      return true; // Unknown modifier.

    switch (ExtraCode[0]) {
    default:
      return true; // Unknown modifier.
    case 'w':      // Print W register
    case 'x':      // Print X register
      if (MO.isReg())
        return printAsmMRegister(MO, ExtraCode[0], O);
      if (MO.isImm() && MO.getImm() == 0) {
        unsigned Reg = ExtraCode[0] == 'w' ? AArch64::WZR : AArch64::XZR;
        O << AArch64InstPrinter::getRegisterName(Reg);
        return false;
      }
      printOperand(MI, OpNum, O);
      return false;
    case 'b': // Print B register.
    case 'h': // Print H register.
    case 's': // Print S register.
    case 'd': // Print D register.
    case 'q': // Print Q register.
    case 'z': // Print Z register.
      if (MO.isReg()) {
        const TargetRegisterClass *RC;
        switch (ExtraCode[0]) {
        case 'b':
          RC = &AArch64::FPR8RegClass;
          break;
        case 'h':
          RC = &AArch64::FPR16RegClass;
          break;
        case 's':
          RC = &AArch64::FPR32RegClass;
          break;
        case 'd':
          RC = &AArch64::FPR64RegClass;
          break;
        case 'q':
          RC = &AArch64::FPR128RegClass;
          break;
        case 'z':
          RC = &AArch64::ZPRRegClass;
          break;
        default:
          return true;
        }
        return printAsmRegInClass(MO, RC, AArch64::NoRegAltName, O);
      }
      printOperand(MI, OpNum, O);
      return false;
    }
  }

  // According to ARM, we should emit x and v registers unless we have a
  // modifier.
  if (MO.isReg()) {
    Register Reg = MO.getReg();

    // If this is a w or x register, print an x register.
    if (AArch64::GPR32allRegClass.contains(Reg) ||
        AArch64::GPR64allRegClass.contains(Reg))
      return printAsmMRegister(MO, 'x', O);

    // If this is an x register tuple, print an x register.
    if (AArch64::GPR64x8ClassRegClass.contains(Reg))
      return printAsmMRegister(MO, 't', O);

    unsigned AltName = AArch64::NoRegAltName;
    const TargetRegisterClass *RegClass;
    if (AArch64::ZPRRegClass.contains(Reg)) {
      RegClass = &AArch64::ZPRRegClass;
    } else if (AArch64::PPRRegClass.contains(Reg)) {
      RegClass = &AArch64::PPRRegClass;
    } else {
      RegClass = &AArch64::FPR128RegClass;
      AltName = AArch64::vreg;
    }

    // If this is a b, h, s, d, or q register, print it as a v register.
    return printAsmRegInClass(MO, RegClass, AltName, O);
  }

  printOperand(MI, OpNum, O);
  return false;
}

bool AArch64AsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI,
                                              unsigned OpNum,
                                              const char *ExtraCode,
                                              raw_ostream &O) {
  if (ExtraCode && ExtraCode[0] && ExtraCode[0] != 'a')
    return true; // Unknown modifier.

  const MachineOperand &MO = MI->getOperand(OpNum);
  assert(MO.isReg() && "unexpected inline asm memory operand");
  O << "[" << AArch64InstPrinter::getRegisterName(MO.getReg()) << "]";
  return false;
}

void AArch64AsmPrinter::PrintDebugValueComment(const MachineInstr *MI,
                                               raw_ostream &OS) {
  unsigned NOps = MI->getNumOperands();
  assert(NOps == 4);
  OS << '\t' << MAI->getCommentString() << "DEBUG_VALUE: ";
  // cast away const; DIetc do not take const operands for some reason.
  OS << MI->getDebugVariable()->getName();
  OS << " <- ";
  // Frame address.  Currently handles register +- offset only.
  assert(MI->isIndirectDebugValue());
  OS << '[';
  for (unsigned I = 0, E = std::distance(MI->debug_operands().begin(),
                                         MI->debug_operands().end());
       I < E; ++I) {
    if (I != 0)
      OS << ", ";
    printOperand(MI, I, OS);
  }
  OS << ']';
  OS << "+";
  printOperand(MI, NOps - 2, OS);
}

void AArch64AsmPrinter::emitJumpTableInfo() {
  const MachineJumpTableInfo *MJTI = MF->getJumpTableInfo();
  if (!MJTI) return;

  const std::vector<MachineJumpTableEntry> &JT = MJTI->getJumpTables();
  if (JT.empty()) return;

  const TargetLoweringObjectFile &TLOF = getObjFileLowering();
  MCSection *ReadOnlySec = TLOF.getSectionForJumpTable(MF->getFunction(), TM);
  OutStreamer->switchSection(ReadOnlySec);

  auto AFI = MF->getInfo<AArch64FunctionInfo>();
  for (unsigned JTI = 0, e = JT.size(); JTI != e; ++JTI) {
    const std::vector<MachineBasicBlock*> &JTBBs = JT[JTI].MBBs;

    // If this jump table was deleted, ignore it.
    if (JTBBs.empty()) continue;

    unsigned Size = AFI->getJumpTableEntrySize(JTI);
    emitAlignment(Align(Size));
    OutStreamer->emitLabel(GetJTISymbol(JTI));

    const MCSymbol *BaseSym = AArch64FI->getJumpTableEntryPCRelSymbol(JTI);
    const MCExpr *Base = MCSymbolRefExpr::create(BaseSym, OutContext);

    for (auto *JTBB : JTBBs) {
      const MCExpr *Value =
          MCSymbolRefExpr::create(JTBB->getSymbol(), OutContext);

      // Each entry is:
      //     .byte/.hword (LBB - Lbase)>>2
      // or plain:
      //     .word LBB - Lbase
      Value = MCBinaryExpr::createSub(Value, Base, OutContext);
      if (Size != 4)
        Value = MCBinaryExpr::createLShr(
            Value, MCConstantExpr::create(2, OutContext), OutContext);

      OutStreamer->emitValue(Value, Size);
    }
  }
}

void AArch64AsmPrinter::emitFunctionEntryLabel() {
  if (MF->getFunction().getCallingConv() == CallingConv::AArch64_VectorCall ||
      MF->getFunction().getCallingConv() ==
          CallingConv::AArch64_SVE_VectorCall ||
      MF->getInfo<AArch64FunctionInfo>()->isSVECC()) {
    auto *TS =
        static_cast<AArch64TargetStreamer *>(OutStreamer->getTargetStreamer());
    TS->emitDirectiveVariantPCS(CurrentFnSym);
  }

  return AsmPrinter::emitFunctionEntryLabel();
}

/// Small jump tables contain an unsigned byte or half, representing the offset
/// from the lowest-addressed possible destination to the desired basic
/// block. Since all instructions are 4-byte aligned, this is further compressed
/// by counting in instructions rather than bytes (i.e. divided by 4). So, to
/// materialize the correct destination we need:
///
///             adr xDest, .LBB0_0
///             ldrb wScratch, [xTable, xEntry]   (with "lsl #1" for ldrh).
///             add xDest, xDest, xScratch (with "lsl #2" for smaller entries)
void AArch64AsmPrinter::LowerJumpTableDest(llvm::MCStreamer &OutStreamer,
                                           const llvm::MachineInstr &MI) {
  Register DestReg = MI.getOperand(0).getReg();
  Register ScratchReg = MI.getOperand(1).getReg();
  Register ScratchRegW =
      STI->getRegisterInfo()->getSubReg(ScratchReg, AArch64::sub_32);
  Register TableReg = MI.getOperand(2).getReg();
  Register EntryReg = MI.getOperand(3).getReg();
  int JTIdx = MI.getOperand(4).getIndex();
  int Size = AArch64FI->getJumpTableEntrySize(JTIdx);

  // This has to be first because the compression pass based its reachability
  // calculations on the start of the JumpTableDest instruction.
  auto Label =
      MF->getInfo<AArch64FunctionInfo>()->getJumpTableEntryPCRelSymbol(JTIdx);

  // If we don't already have a symbol to use as the base, use the ADR
  // instruction itself.
  if (!Label) {
    Label = MF->getContext().createTempSymbol();
    AArch64FI->setJumpTableEntryInfo(JTIdx, Size, Label);
    OutStreamer.emitLabel(Label);
  }

  auto LabelExpr = MCSymbolRefExpr::create(Label, MF->getContext());
  EmitToStreamer(OutStreamer, MCInstBuilder(AArch64::ADR)
                                  .addReg(DestReg)
                                  .addExpr(LabelExpr));

  // Load the number of instruction-steps to offset from the label.
  unsigned LdrOpcode;
  switch (Size) {
  case 1: LdrOpcode = AArch64::LDRBBroX; break;
  case 2: LdrOpcode = AArch64::LDRHHroX; break;
  case 4: LdrOpcode = AArch64::LDRSWroX; break;
  default:
    llvm_unreachable("Unknown jump table size");
  }

  EmitToStreamer(OutStreamer, MCInstBuilder(LdrOpcode)
                                  .addReg(Size == 4 ? ScratchReg : ScratchRegW)
                                  .addReg(TableReg)
                                  .addReg(EntryReg)
                                  .addImm(0)
                                  .addImm(Size == 1 ? 0 : 1));

  // Add to the already materialized base label address, multiplying by 4 if
  // compressed.
  EmitToStreamer(OutStreamer, MCInstBuilder(AArch64::ADDXrs)
                                  .addReg(DestReg)
                                  .addReg(DestReg)
                                  .addReg(ScratchReg)
                                  .addImm(Size == 4 ? 0 : 2));
}

void AArch64AsmPrinter::LowerMOPS(llvm::MCStreamer &OutStreamer,
                                  const llvm::MachineInstr &MI) {
  unsigned Opcode = MI.getOpcode();
  assert(STI->hasMOPS());
  assert(STI->hasMTE() || Opcode != AArch64::MOPSMemorySetTaggingPseudo);

  const auto Ops = [Opcode]() -> std::array<unsigned, 3> {
    if (Opcode == AArch64::MOPSMemoryCopyPseudo)
      return {AArch64::CPYFP, AArch64::CPYFM, AArch64::CPYFE};
    if (Opcode == AArch64::MOPSMemoryMovePseudo)
      return {AArch64::CPYP, AArch64::CPYM, AArch64::CPYE};
    if (Opcode == AArch64::MOPSMemorySetPseudo)
      return {AArch64::SETP, AArch64::SETM, AArch64::SETE};
    if (Opcode == AArch64::MOPSMemorySetTaggingPseudo)
      return {AArch64::SETGP, AArch64::SETGM, AArch64::MOPSSETGE};
    llvm_unreachable("Unhandled memory operation pseudo");
  }();
  const bool IsSet = Opcode == AArch64::MOPSMemorySetPseudo ||
                     Opcode == AArch64::MOPSMemorySetTaggingPseudo;

  for (auto Op : Ops) {
    int i = 0;
    auto MCIB = MCInstBuilder(Op);
    // Destination registers
    MCIB.addReg(MI.getOperand(i++).getReg());
    MCIB.addReg(MI.getOperand(i++).getReg());
    if (!IsSet)
      MCIB.addReg(MI.getOperand(i++).getReg());
    // Input registers
    MCIB.addReg(MI.getOperand(i++).getReg());
    MCIB.addReg(MI.getOperand(i++).getReg());
    MCIB.addReg(MI.getOperand(i++).getReg());

    EmitToStreamer(OutStreamer, MCIB);
  }
}

void AArch64AsmPrinter::LowerSTACKMAP(MCStreamer &OutStreamer, StackMaps &SM,
                                      const MachineInstr &MI) {
  unsigned NumNOPBytes = StackMapOpers(&MI).getNumPatchBytes();

  auto &Ctx = OutStreamer.getContext();
  MCSymbol *MILabel = Ctx.createTempSymbol();
  OutStreamer.emitLabel(MILabel);

  SM.recordStackMap(*MILabel, MI);
  assert(NumNOPBytes % 4 == 0 && "Invalid number of NOP bytes requested!");

  // Scan ahead to trim the shadow.
  const MachineBasicBlock &MBB = *MI.getParent();
  MachineBasicBlock::const_iterator MII(MI);
  ++MII;
  while (NumNOPBytes > 0) {
    if (MII == MBB.end() || MII->isCall() ||
        MII->getOpcode() == AArch64::DBG_VALUE ||
        MII->getOpcode() == TargetOpcode::PATCHPOINT ||
        MII->getOpcode() == TargetOpcode::STACKMAP)
      break;
    ++MII;
    NumNOPBytes -= 4;
  }

  // Emit nops.
  for (unsigned i = 0; i < NumNOPBytes; i += 4)
    EmitToStreamer(OutStreamer, MCInstBuilder(AArch64::HINT).addImm(0));
}

// Lower a patchpoint of the form:
// [<def>], <id>, <numBytes>, <target>, <numArgs>
void AArch64AsmPrinter::LowerPATCHPOINT(MCStreamer &OutStreamer, StackMaps &SM,
                                        const MachineInstr &MI) {
  auto &Ctx = OutStreamer.getContext();
  MCSymbol *MILabel = Ctx.createTempSymbol();
  OutStreamer.emitLabel(MILabel);
  SM.recordPatchPoint(*MILabel, MI);

  PatchPointOpers Opers(&MI);

  int64_t CallTarget = Opers.getCallTarget().getImm();
  unsigned EncodedBytes = 0;
  if (CallTarget) {
    assert((CallTarget & 0xFFFFFFFFFFFF) == CallTarget &&
           "High 16 bits of call target should be zero.");
    Register ScratchReg = MI.getOperand(Opers.getNextScratchIdx()).getReg();
    EncodedBytes = 16;
    // Materialize the jump address:
    EmitToStreamer(OutStreamer, MCInstBuilder(AArch64::MOVZXi)
                                    .addReg(ScratchReg)
                                    .addImm((CallTarget >> 32) & 0xFFFF)
                                    .addImm(32));
    EmitToStreamer(OutStreamer, MCInstBuilder(AArch64::MOVKXi)
                                    .addReg(ScratchReg)
                                    .addReg(ScratchReg)
                                    .addImm((CallTarget >> 16) & 0xFFFF)
                                    .addImm(16));
    EmitToStreamer(OutStreamer, MCInstBuilder(AArch64::MOVKXi)
                                    .addReg(ScratchReg)
                                    .addReg(ScratchReg)
                                    .addImm(CallTarget & 0xFFFF)
                                    .addImm(0));
    EmitToStreamer(OutStreamer, MCInstBuilder(AArch64::BLR).addReg(ScratchReg));
  }
  // Emit padding.
  unsigned NumBytes = Opers.getNumPatchBytes();
  assert(NumBytes >= EncodedBytes &&
         "Patchpoint can't request size less than the length of a call.");
  assert((NumBytes - EncodedBytes) % 4 == 0 &&
         "Invalid number of NOP bytes requested!");
  for (unsigned i = EncodedBytes; i < NumBytes; i += 4)
    EmitToStreamer(OutStreamer, MCInstBuilder(AArch64::HINT).addImm(0));
}

static unsigned calculateFrameSize(const MachineFunction *MF,
                                   const MachineInstr &MI) {
  const TargetFrameLowering *TFI = MF->getSubtarget().getFrameLowering();
  const MachineFrameInfo &MFI = MF->getFrameInfo();
  bool IsFunclet = MI.getParent()->isEHFuncletEntry();
  unsigned FrameSize = 0;
  if (IsFunclet) {
    // This is the size of the pushed CSRs.
    unsigned CSSize =
        MF->getInfo<AArch64FunctionInfo>()->getCalleeSavedStackSize();
    // This is the amount of stack a funclet needs to allocate.
    FrameSize = alignTo(CSSize + MF->getFrameInfo().getMaxCallFrameSize(),
                        TFI->getStackAlign());
  } else {
    FrameSize = MFI.getStackSize();
  }
  return FrameSize;
}

// Note: emit specific inst should update inst size info in
// AArch64InstrInfo::getInstSizeInBytes for AArch64 at the same time
//  add sp, sp, #AddSize
void AArch64AsmPrinter::emitAddSP(unsigned AddSize) {
  using namespace AArch64;
  MCInst AddSPLarge;
  MCInst AddSPMid;
  MCInst AddSPSmall;
  // 4096: the max imm for aarch64, if the size exceeds this value, we need to
  // use the add instruction with shift to do so.
  unsigned LargeNum = AddSize / (4096 * 4096);
  if (LargeNum) {
    // 24: 2 ^ 24 = 4096 * 4096
    AddSPLarge =
        MCInstBuilder(ADDXri).addReg(SP).addReg(SP).addImm(LargeNum).addImm(
            AArch64_AM::getShifterImm(AArch64_AM::LSL, 24));
  }
  unsigned MidNum = AddSize % (4096 * 4096) / 4096;
  if (MidNum > 0) {
    // 12: 2 ^ 12 = 4096
    AddSPMid =
        MCInstBuilder(ADDXri).addReg(SP).addReg(SP).addImm(MidNum).addImm(
            AArch64_AM::getShifterImm(AArch64_AM::LSL, 12));
  }
  unsigned SmallNum = AddSize % 4096;
  if (SmallNum > 0) {
    AddSPSmall =
        MCInstBuilder(ADDXri).addReg(SP).addReg(SP).addImm(SmallNum).addImm(0);
  }

  if (LargeNum > 0) {
    EmitToStreamer(*OutStreamer, AddSPLarge);
  }
  if (MidNum > 0) {
    EmitToStreamer(*OutStreamer, AddSPMid);
  }
  if (SmallNum > 0) {
    EmitToStreamer(*OutStreamer, AddSPSmall);
  }
}

// Note: emit specific inst should update inst size info in
// AArch64InstrInfo::getInstSizeInBytes for AArch64 at the same time
//  sub sp, sp, #Size
void AArch64AsmPrinter::emitSubSP(unsigned Size) {
  using namespace AArch64;
  unsigned RevertSize = Size;

  MCInst SubSPLarge;
  MCInst SubSPMid;
  MCInst SubSPSmall;
  // 4096: the max imm for aarch64, if the size exceeds this value, we need to
  // use the sub instruction with shift to do so.
  unsigned LargeNum = RevertSize / (4096 * 4096);
  if (LargeNum) {
    // 24: 2 ^ 24 = 4096 * 4096
    SubSPLarge =
        MCInstBuilder(SUBXri).addReg(SP).addReg(SP).addImm(LargeNum).addImm(
            AArch64_AM::getShifterImm(AArch64_AM::LSL, 24));
  }
  unsigned MidNum = RevertSize % (4096 * 4096) / 4096;
  if (MidNum > 0) {
    // 12: 2 ^ 12 = 4096
    SubSPMid =
        MCInstBuilder(SUBXri).addReg(SP).addReg(SP).addImm(MidNum).addImm(
            AArch64_AM::getShifterImm(AArch64_AM::LSL, 12));
  }
  unsigned SmallNum = RevertSize % 4096;
  if (SmallNum > 0) {
    SubSPSmall =
        MCInstBuilder(SUBXri).addReg(SP).addReg(SP).addImm(SmallNum).addImm(0);
  }
  if (LargeNum > 0) {
    EmitToStreamer(*OutStreamer, SubSPLarge);
  }
  if (MidNum > 0) {
    EmitToStreamer(*OutStreamer, SubSPMid);
  }
  if (SmallNum > 0) {
    EmitToStreamer(*OutStreamer, SubSPSmall);
  }
}

// Note: emit specific inst should update inst size info in
// AArch64InstrInfo::getInstSizeInBytes for AArch64 at the same time
//   ldr x9, [x28, #ProtectAddr]
//   subs xzr, sp, x9              # stack.check
//   b.gt Lstack.check.end
//   ...                           # stack.overflow handle
// .Lstack.check.end:
void AArch64AsmPrinter::emitCJStackCheck(const MachineInstr &MI) {
  using namespace AArch64;
  MCContext &Ctx = MF->getContext();
  MCSymbol *EndSym = Ctx.createTempSymbol("stack.check.end");

  // ldr x9, [x28, #ProtectAddr]
  emitGetCJTLSData(getProtectAddrOffsetInCJTLS());
  MCInst Cmp = MCInstBuilder(SUBSXrx).addReg(XZR).addReg(SP).addReg(X9).addImm(
      AArch64_AM::getArithExtendImm(AArch64_AM::UXTX, 0));
  EmitToStreamer(*OutStreamer, Cmp);

  auto BrExpr = MCSymbolRefExpr::create(EndSym, MCSymbolRefExpr::VK_None, Ctx);
  MCInst BrInst = MCInstBuilder(Bcc).addImm(AArch64CC::GT).addExpr(BrExpr);
  EmitToStreamer(*OutStreamer, BrInst);
  emitStackOverflowCall(MI);
  OutStreamer->emitLabel(EndSym);
}

// Note: emit specific inst should update inst size info in
// AArch64InstrInfo::getInstSizeInBytes for AArch64 at the same time
// >>>>>>>>>>>>>>>>>>>>>>> disable stackgrow
//  add sp, sp, #AddSize
//  mov x0, #LeftSize
//  bl CJ_MCC_ThrowStackOverflowError
// =======================
//  add sp, sp, #AddSize
//  mov x9, #LeftSize
//  bl CJ_MCC_StackGrowStub
// Ltemp
//  sub sp, sp, #AddSize
// <<<<<<<<<<<<<<<<<<<<<<< enable stackgrow
int AArch64AsmPrinter::emitStackOverflowCall(const MachineInstr &MI) {
  using namespace AArch64;
  unsigned FrameSize = calculateFrameSize(MF, MI);
  unsigned AddSize = MI.peekCJStackSize();
  emitAddSP(AddSize);

  MCContext &Ctx = MF->getContext();
  unsigned LeftSize = FrameSize - AddSize;
  if (!EnableStackGrow) { // stack overflow
    MCInst Mov = MCInstBuilder(MOVZXi).addReg(X0).addImm(LeftSize).addImm(0);
    MCSymbol *Sym = nullptr;
    if (MF->getTarget().getTargetTriple().isOSBinFormatMachO()) {
      Sym = Ctx.getOrCreateSymbol("_CJ_MCC_ThrowStackOverflowError");
    } else {
      Sym = Ctx.getOrCreateSymbol("CJ_MCC_ThrowStackOverflowError");
    }
    MCInst BLToSOFE = MCInstBuilder(BL).addExpr(
        MCSymbolRefExpr::create(Sym, MCSymbolRefExpr::VK_None, Ctx));
    EmitToStreamer(*OutStreamer, Mov);
    EmitToStreamer(*OutStreamer, BLToSOFE);
    return 3; // 3: instruction nums.
  } else {    // stack grow
    MCInst MovX9 = MCInstBuilder(MOVZXi).addReg(X9).addImm(LeftSize).addImm(0);
    MCSymbol *Sym = Ctx.getOrCreateSymbol("CJ_MCC_StackGrowStub");
    MCInst BLToCall = MCInstBuilder(BL).addExpr(
        MCSymbolRefExpr::create(Sym, MCSymbolRefExpr::VK_None, Ctx));
    EmitToStreamer(*OutStreamer, MovX9);
    EmitToStreamer(*OutStreamer, BLToCall);
    SM.recordCJStackMap(MI, true);

    // revert sp
    emitSubSP(AddSize);
    return 6; // 6: instruction nums.
  }
}

// ldr x9, [x28, #tlsOffset]
void AArch64AsmPrinter::emitGetCJTLSData(int64_t Offset) {
  MCInst Ldr;
  Ldr.setOpcode(AArch64::LDRXui);
  Ldr.addOperand(MCOperand::createReg(AArch64::X9));
  Ldr.addOperand(MCOperand::createReg(AArch64::X28));
  // 8: each imm represents 8 In MCInst.
  Ldr.addOperand(MCOperand::createImm(Offset / 8));
  Ldr.addOperand(MCOperand::createImm(0));
  EmitToStreamer(*OutStreamer, Ldr);
}

// Note: emit specific inst should update inst size info in
// AArch64InstrInfo::getInstSizeInBytes for AArch64 at the same time
void AArch64AsmPrinter::emitGcStateCheck() {
  if (TM.getTargetTriple().isWebm()) {
    // ldr x9, [x28, #MutatorOffsetInCJTLS]
    // ldr x9, [x9, #0]
    emitGetCJTLSData(getMutatorOffsetInCJTLS());
    MCInst Ldr;
    Ldr.setOpcode(AArch64::LDRXui);
    Ldr.addOperand(MCOperand::createReg(AArch64::X9));
    Ldr.addOperand(MCOperand::createReg(AArch64::X9));
    Ldr.addOperand(MCOperand::createImm(0));
    Ldr.addOperand(MCOperand::createImm(0));
    EmitToStreamer(*OutStreamer, Ldr);
  } else {
    // lsr x9, x28, #56
    MCInst Lsr;
    Lsr.setOpcode(AArch64::UBFMXri);
    Lsr.addOperand(MCOperand::createReg(AArch64::W9));
    Lsr.addOperand(MCOperand::createReg(AArch64::X28));
    Lsr.addOperand(MCOperand::createImm(56)); // 56: Shift bit
    Lsr.addOperand(MCOperand::createImm(63)); // 63: Shift range
    EmitToStreamer(*OutStreamer, Lsr);
  }
}

// Note: emit specific inst should update inst size info in
// AArch64InstrInfo::getInstSizeInBytes for AArch64 at the same time
//  ldr x9, [x28, #SafepointCheckAddrOffset]
//  cmp x9, #0
// >>>>>>>>>>>>>>>>>>>
// case 1:
//  b.ne Label
//  ...
// Label:
//  adrp  x9, CJ_MCC_HandleSafepoint.CJStubGV
//  ldr x9, [x9, :lo12:CJ_MCC_HandleSafepoint.CJStubGV]
//  blr x9
// =================== Offset beyond 0x7FFFF
// case 2:
//  b.eq Label
//  adrp  x9, CJ_MCC_HandleSafepoint.CJStubGV
//  ldr x9, [x9, :lo12:CJ_MCC_HandleSafepoint.CJStubGV]
//  blr x9
// Label:
//  ...
// <<<<<<<<<<<<<<<<<<<
void AArch64AsmPrinter::emitCangjieSafepoint(const MachineInstr &MI) {
  emitGetCJTLSData(getSafepointCheckAddrOffsetInCJTLS());

  auto &Ctx = OutStreamer->getContext();
  MCInst Cmp = MCInstBuilder(AArch64::SUBSXri)
                   .addReg(AArch64::XZR)
                   .addReg(AArch64::X9)
                   .addImm(0)
                   .addImm(0);
  EmitToStreamer(*OutStreamer, Cmp);

  // If Jmp size beyond the 19bit, emitting safepoint on following inst
  // instead of emit it at the end of the function.
  // Each Inst size is 4 bytes.
  if (MI.getMF()->getInstructionCount() * 4 > 0x7FFFF) {
    MCSymbol *EndLabel = Ctx.createTempSymbol();
    MCInst Branch = MCInstBuilder(AArch64::Bcc)
                        .addImm(AArch64CC::EQ)
                        .addExpr(MCSymbolRefExpr::create(EndLabel, Ctx));
    EmitToStreamer(*OutStreamer, Branch);
    emitSafepoint(MI);
    OutStreamer->emitLabel(EndLabel);
  } else {
    MCSymbol *JmpLabel = Ctx.createTempSymbol();
    MCSymbol *EndLabel = Ctx.createTempSymbol();
    MCInst Branch = MCInstBuilder(AArch64::Bcc)
                        .addImm(AArch64CC::NE)
                        .addExpr(MCSymbolRefExpr::create(JmpLabel, Ctx));
    EmitToStreamer(*OutStreamer, Branch);
    OutStreamer->emitLabel(EndLabel);
    SafepointStackMap.push_back(std::make_tuple(&MI, JmpLabel, EndLabel));
  }
}

void AArch64AsmPrinter::emitCangjieSafepoint() {
  emitGetCJTLSData(getSafepointCheckAddrOffsetInCJTLS());

  auto &Ctx = OutStreamer->getContext();
  MCInst Cmp = MCInstBuilder(AArch64::SUBSXri)
                   .addReg(AArch64::XZR)
                   .addReg(AArch64::X9)
                   .addImm(0)
                   .addImm(0);
  EmitToStreamer(*OutStreamer, Cmp);

  MCSymbol *EndLabel = Ctx.createTempSymbol();
  MCInst Branch = MCInstBuilder(AArch64::Bcc)
                      .addImm(AArch64CC::EQ)
                      .addExpr(MCSymbolRefExpr::create(EndLabel, Ctx));
  EmitToStreamer(*OutStreamer, Branch);

  using namespace AArch64;

  auto *AddrGV = MF->getFunction().getParent()->getGlobalVariable(
      "CJ_MCC_HandleSafepoint.CJStubGV", true);
  MachineOperand MOSym = MachineOperand::CreateGA(AddrGV, 0);
  MCOperand SymOriAddr = setGAAndLower(MOSym, AddrGV, AArch64II::MO_PAGE);
  MCOperand SymOriAddrLo12 =
      setGAAndLower(MOSym, AddrGV, AArch64II::MO_PAGEOFF | AArch64II::MO_NC);

  MCInst Adrp = MCInstBuilder(ADRP).addReg(X9).addOperand(SymOriAddr);
  EmitToStreamer(*OutStreamer, Adrp);

  MCInst Ldr = MCInstBuilder(LDRXui)
                   .addReg(X9)
                   .addReg(X9)
                   .addOperand(SymOriAddrLo12)
                   .addImm(0);
  EmitToStreamer(*OutStreamer, Ldr);

  MCInst CallReg = MCInstBuilder(BR).addReg(X9);
  EmitToStreamer(*OutStreamer, CallReg);

  OutStreamer->emitLabel(EndLabel);

  MCInst RetInst = MCInstBuilder(AArch64::RET)
                      .addReg(AArch64::LR);
  EmitToStreamer(*OutStreamer, RetInst);
}

//  adrp  x9, CJ_MCC_HandleSafepoint.CJStubGV
//  ldr x9, [x9, :lo12:CJ_MCC_HandleSafepoint.CJStubGV]
//  blr x9
// jmp  Label1
int AArch64AsmPrinter::emitSafePointDirectCall(unsigned Index) {
  auto &Ctx = OutStreamer->getContext();
  auto SS = SafepointStackMap[Index];
  MCSymbol *CurLabel = std::get<1>(SS);
  MCSymbol *BackLabel = std::get<2>(SS);
  OutStreamer->emitLabel(CurLabel);
  emitSafepoint(*std::get<0>(SS));

  auto Expr = MCSymbolRefExpr::create(BackLabel, MCSymbolRefExpr::VK_None, Ctx);
  MCInst BNoCond = MCInstBuilder(AArch64::B).addExpr(Expr);
  EmitToStreamer(*OutStreamer, BNoCond);
  // 4: instruction nums.
  return 4;
}

//  adrp  x9, CJ_MCC_HandleSafepoint.CJStubGV
//  ldr x9, [x9, :lo12:CJ_MCC_HandleSafepoint.CJStubGV]
//  blr x9
void AArch64AsmPrinter::emitSafepoint(const MachineInstr &MI) {
  using namespace AArch64;

  auto *AddrGV = MF->getFunction().getParent()->getGlobalVariable(
      "CJ_MCC_HandleSafepoint.CJStubGV", true);
  MachineOperand MOSym = MachineOperand::CreateGA(AddrGV, 0);
  MCOperand SymOriAddr = setGAAndLower(MOSym, AddrGV, AArch64II::MO_PAGE);
  MCOperand SymOriAddrLo12 =
      setGAAndLower(MOSym, AddrGV, AArch64II::MO_PAGEOFF | AArch64II::MO_NC);

  MCInst Adrp = MCInstBuilder(ADRP).addReg(X9).addOperand(SymOriAddr);
  EmitToStreamer(*OutStreamer, Adrp);

  MCInst Ldr = MCInstBuilder(LDRXui)
                   .addReg(X9)
                   .addReg(X9)
                   .addOperand(SymOriAddrLo12)
                   .addImm(0);
  EmitToStreamer(*OutStreamer, Ldr);

  MCInst CallReg = MCInstBuilder(BLR).addReg(X9);
  EmitToStreamer(*OutStreamer, CallReg);
  SM.recordCJStackMap(MI, true);
}

bool AArch64AsmPrinter::LowerCangjieSpecificTCRETUR(MCStreamer &OutStreamer, StackMaps &SM,
                                        const MachineInstr &MI) {
  auto ParentMF = MI.getParent()->getParent();
  if (ParentMF && !ParentMF->getName().startswith("OUTLINED_FUNCTION"))
    return false;
  if (CJPipeline) {
    const MachineOperand &CalleeOp = MI.getOperand(0);
    if (CalleeOp.isGlobal()) {
      auto CalleeName = CalleeOp.getGlobal()->getName();
      if (CalleeName.equals("CJ_MCC_HandleSafepoint")) {
        emitCangjieSafepoint();
        return true;
      } else if (CalleeName.equals("CJ_MCC_StackCheck")) {

      } else if (CalleeName.startswith("CJ_MCC_NewArray")) {
        emitCJNewArrayFastPath(CalleeOp);
        return true;
      }
    }
  }
  return false;
}

void AArch64AsmPrinter::LowerSTATEPOINT(MCStreamer &OutStreamer, StackMaps &SM,
                                        const MachineInstr &MI) {
  StatepointOpers SOpers(&MI);
  // Lower call target and choose correct opcode
  const MachineOperand &CallTarget = SOpers.getCallTarget();
  if (CJPipeline) {
    uint64_t ID = SOpers.getID();
    auto CallTargetName = CallTarget.getGlobal()->getName();
    if (ID != Cangjie::CJStatepointID::Default && CallTargetName.startswith("OUTLINED_FUNCTION")) {
      MCOperand CallTargetMCOp;
      MCInstLowering.lowerOperand(CallTarget, CallTargetMCOp);
      EmitToStreamer(OutStreamer,
                   MCInstBuilder(AArch64::BL).addOperand(CallTargetMCOp));
      auto &Ctx = OutStreamer.getContext();
      MCSymbol *MILabel = Ctx.createTempSymbol();
      OutStreamer.emitLabel(MILabel);
      SM.recordStatepoint(*MILabel, MI);
      return;
    }
    if (ID == Cangjie::CJStatepointID::Safepoint) {
      emitCangjieSafepoint(MI);
      return;
    }
    if (ID == Cangjie::CJStatepointID::StackCheck) {
      emitCangjieStackCheck(MI);
      return;
    }
    if (ID == Cangjie::CJStatepointID::NewArrayFast) {
      emitCJNewArrayFastPath(MI, CallTarget);
      return;
    }
  }

  if (unsigned PatchBytes = SOpers.getNumPatchBytes()) {
    // 4: size of the patchpoint intrinsic
    assert(PatchBytes % 4 == 0 && "Invalid number of NOP bytes requested");
    for (unsigned i = 0; i < PatchBytes; i += 4)
      EmitToStreamer(OutStreamer, MCInstBuilder(AArch64::HINT).addImm(0));
  } else {
    MCOperand CallTargetMCOp;
    unsigned CallOpcode;
    switch (CallTarget.getType()) {
    case MachineOperand::MO_GlobalAddress:
    case MachineOperand::MO_ExternalSymbol: {
      CallOpcode = AArch64::BL;
      if (tryEmitCangjieSpecificCallByMOSym(&MI, CallTarget, CallOpcode)) {
        SM.recordCJStackMap(MI);
        return;
      }
      MCInstLowering.lowerOperand(CallTarget, CallTargetMCOp);
      break;
    }
    case MachineOperand::MO_Immediate:
      CallTargetMCOp = MCOperand::createImm(CallTarget.getImm());
      CallOpcode = AArch64::BL;
      break;
    case MachineOperand::MO_Register:
      CallTargetMCOp = MCOperand::createReg(CallTarget.getReg());
      CallOpcode = AArch64::BLR;
      break;
    default:
      llvm_unreachable("Unsupported operand type in statepoint call target");
      break;
    }

    EmitToStreamer(OutStreamer,
                   MCInstBuilder(CallOpcode).addOperand(CallTargetMCOp));
  }

  auto &Ctx = OutStreamer.getContext();
  MCSymbol *MILabel = Ctx.createTempSymbol();
  OutStreamer.emitLabel(MILabel);
  SM.recordStatepoint(*MILabel, MI);
}

void AArch64AsmPrinter::LowerFAULTING_OP(const MachineInstr &FaultingMI) {
  // FAULTING_LOAD_OP <def>, <faltinf type>, <MBB handler>,
  //                  <opcode>, <operands>

  Register DefRegister = FaultingMI.getOperand(0).getReg();
  FaultMaps::FaultKind FK =
      static_cast<FaultMaps::FaultKind>(FaultingMI.getOperand(1).getImm());
  MCSymbol *HandlerLabel = FaultingMI.getOperand(2).getMBB()->getSymbol();
  unsigned Opcode = FaultingMI.getOperand(3).getImm();
  unsigned OperandsBeginIdx = 4;

  auto &Ctx = OutStreamer->getContext();
  MCSymbol *FaultingLabel = Ctx.createTempSymbol();
  OutStreamer->emitLabel(FaultingLabel);

  assert(FK < FaultMaps::FaultKindMax && "Invalid Faulting Kind!");
  FM.recordFaultingOp(FK, FaultingLabel, HandlerLabel);

  MCInst MI;
  MI.setOpcode(Opcode);

  if (DefRegister != (Register)0)
    MI.addOperand(MCOperand::createReg(DefRegister));

  for (auto I = FaultingMI.operands_begin() + OperandsBeginIdx,
            E = FaultingMI.operands_end();
       I != E; ++I) {
    MCOperand Dest;
    lowerOperand(*I, Dest);
    MI.addOperand(Dest);
  }

  OutStreamer->AddComment("on-fault: " + HandlerLabel->getName());
  OutStreamer->emitInstruction(MI, getSubtargetInfo());
}

void AArch64AsmPrinter::emitFMov0(const MachineInstr &MI) {
  Register DestReg = MI.getOperand(0).getReg();
  if (STI->hasZeroCycleZeroingFP() && !STI->hasZeroCycleZeroingFPWorkaround() &&
      STI->hasNEON()) {
    // Convert H/S register to corresponding D register
    if (AArch64::H0 <= DestReg && DestReg <= AArch64::H31)
      DestReg = AArch64::D0 + (DestReg - AArch64::H0);
    else if (AArch64::S0 <= DestReg && DestReg <= AArch64::S31)
      DestReg = AArch64::D0 + (DestReg - AArch64::S0);
    else
      assert(AArch64::D0 <= DestReg && DestReg <= AArch64::D31);

    MCInst MOVI;
    MOVI.setOpcode(AArch64::MOVID);
    MOVI.addOperand(MCOperand::createReg(DestReg));
    MOVI.addOperand(MCOperand::createImm(0));
    EmitToStreamer(*OutStreamer, MOVI);
  } else {
    MCInst FMov;
    switch (MI.getOpcode()) {
    default: llvm_unreachable("Unexpected opcode");
    case AArch64::FMOVH0:
      FMov.setOpcode(AArch64::FMOVWHr);
      FMov.addOperand(MCOperand::createReg(DestReg));
      FMov.addOperand(MCOperand::createReg(AArch64::WZR));
      break;
    case AArch64::FMOVS0:
      FMov.setOpcode(AArch64::FMOVWSr);
      FMov.addOperand(MCOperand::createReg(DestReg));
      FMov.addOperand(MCOperand::createReg(AArch64::WZR));
      break;
    case AArch64::FMOVD0:
      FMov.setOpcode(AArch64::FMOVXDr);
      FMov.addOperand(MCOperand::createReg(DestReg));
      FMov.addOperand(MCOperand::createReg(AArch64::XZR));
      break;
    }
    EmitToStreamer(*OutStreamer, FMov);
  }
}

// Simple pseudo-instructions have their lowering (with expansion to real
// instructions) auto-generated.
#include "AArch64GenMCPseudoLowering.inc"

void AArch64AsmPrinter::emitInstruction(const MachineInstr *MI) {
  AArch64_MC::verifyInstructionPredicates(MI->getOpcode(), STI->getFeatureBits());

  // Do any auto-generated pseudo lowerings.
  if (emitPseudoExpansionLowering(*OutStreamer, MI))
    return;

  if (MI->getOpcode() == AArch64::ADRP) {
    for (auto &Opd : MI->operands()) {
      if (Opd.isSymbol() && StringRef(Opd.getSymbolName()) ==
                                "swift_async_extendedFramePointerFlags") {
        ShouldEmitWeakSwiftAsyncExtendedFramePointerFlags = true;
      }
    }
  }

  if (AArch64FI->getLOHRelated().count(MI)) {
    // Generate a label for LOH related instruction
    MCSymbol *LOHLabel = createTempSymbol("loh");
    // Associate the instruction with the label
    LOHInstToLabel[MI] = LOHLabel;
    OutStreamer->emitLabel(LOHLabel);
  }

  // try to process cangjie specific call
  if (tryEmitCangjieSpecificCall(MI)) {
    return;
  }
  AArch64TargetStreamer *TS =
    static_cast<AArch64TargetStreamer *>(OutStreamer->getTargetStreamer());
  // Do any manual lowerings.
  switch (MI->getOpcode()) {
  default:
    break;
  case AArch64::ADR: {
    // store func begin address to stack slot, and MI in storeFunctionAddress.
    if (!MI->getFlag(MachineInstr::FrameSetup)) {
      break;
    }
    // adr x9, Lfunc_begin
    MCInst TmpInst;
    TmpInst.setOpcode(AArch64::ADR);
    TmpInst.addOperand(MCOperand::createReg(MI->getOperand(0).getReg()));
    const MCExpr *Expr = MCSymbolRefExpr::create(
        getFunctionBegin(), MCSymbolRefExpr::VK_None, OutContext);
    TmpInst.addOperand(MCOperand::createExpr(Expr));
    EmitToStreamer(*OutStreamer, TmpInst);
    return;
  }
  case AArch64::ADRP: {
    // store func begin address to stack slot, and MI in storeFunctionAddress.
    if (!MI->getFlag(MachineInstr::FrameSetup)) {
      break;
    }
    // Only for macos now.
    MCInst AdrpInst;
    Function &Func = MF->getFunction();
    const MCExpr *Expr = nullptr;
    if (!Func.hasFnAttribute("leaf-function")) {
      Metadata *MD = Func.getParent()->getModuleFlag("Cangjie_PACKAGE_ID");
      if (MD == nullptr) {
        report_fatal_error("There is not cangjie package id in module!");
      }
      StringRef PACKAGEID = dyn_cast<MDString>(MD)->getString();
      MCSymbol *DescSymbol = OutContext.getOrCreateSymbol(
          ".Lmethod_desc." + PACKAGEID + "._" + MF->getName());
      Expr = MCSymbolRefExpr::create(
          DescSymbol, MCSymbolRefExpr::VK_PAGE, OutContext);
      // adrp x10, .Lmethod_desc.PACKAGEID.xxx@PAGE
      AdrpInst.setOpcode(AArch64::ADRP);
      AdrpInst.addOperand(MCOperand::createReg(AArch64::X10));
      AdrpInst.addOperand(MCOperand::createExpr(Expr));
      EmitToStreamer(*OutStreamer, AdrpInst);

      MCInst AddInst;
      const MCExpr *AddExpr = MCSymbolRefExpr::create(
          DescSymbol, MCSymbolRefExpr::VK_PAGEOFF, OutContext);
      // add x10, x10, .Lmethod_desc.PACKAGEID.xxx@PAGEOFF
      AddInst.setOpcode(AArch64::ADDXri);
      AddInst.addOperand(MCOperand::createReg(AArch64::X10));
      AddInst.addOperand(MCOperand::createReg(AArch64::X10));
      AddInst.addOperand(MCOperand::createExpr(AddExpr));
      AddInst.addOperand(MCOperand::createImm(AArch64_AM::getShiftValue(0)));
      EmitToStreamer(*OutStreamer, AddInst);
    }
    return;
  }
  case AArch64::HINT: {
    // CurrentPatchableFunctionEntrySym can be CurrentFnBegin only for
    // -fpatchable-function-entry=N,0. The entry MBB is guaranteed to be
    // non-empty. If MI is the initial BTI, place the
    // __patchable_function_entries label after BTI.
    if (CurrentPatchableFunctionEntrySym &&
        CurrentPatchableFunctionEntrySym == CurrentFnBegin &&
        MI == &MF->front().front()) {
      int64_t Imm = MI->getOperand(0).getImm();
      if ((Imm & 32) && (Imm & 6)) {
        MCInst Inst;
        MCInstLowering.Lower(MI, Inst);
        EmitToStreamer(*OutStreamer, Inst);
        CurrentPatchableFunctionEntrySym = createTempSymbol("patch");
        OutStreamer->emitLabel(CurrentPatchableFunctionEntrySym);
        return;
      }
    }
    break;
  }
    case AArch64::MOVMCSym: {
      Register DestReg = MI->getOperand(0).getReg();
      const MachineOperand &MO_Sym = MI->getOperand(1);
      MachineOperand Hi_MOSym(MO_Sym), Lo_MOSym(MO_Sym);
      MCOperand Hi_MCSym, Lo_MCSym;

      Hi_MOSym.setTargetFlags(AArch64II::MO_G1 | AArch64II::MO_S);
      Lo_MOSym.setTargetFlags(AArch64II::MO_G0 | AArch64II::MO_NC);

      MCInstLowering.lowerOperand(Hi_MOSym, Hi_MCSym);
      MCInstLowering.lowerOperand(Lo_MOSym, Lo_MCSym);

      MCInst MovZ;
      MovZ.setOpcode(AArch64::MOVZXi);
      MovZ.addOperand(MCOperand::createReg(DestReg));
      MovZ.addOperand(Hi_MCSym);
      MovZ.addOperand(MCOperand::createImm(16));
      EmitToStreamer(*OutStreamer, MovZ);

      MCInst MovK;
      MovK.setOpcode(AArch64::MOVKXi);
      MovK.addOperand(MCOperand::createReg(DestReg));
      MovK.addOperand(MCOperand::createReg(DestReg));
      MovK.addOperand(Lo_MCSym);
      MovK.addOperand(MCOperand::createImm(0));
      EmitToStreamer(*OutStreamer, MovK);
      return;
  }
  case AArch64::MOVIv2d_ns:
    // If the target has <rdar://problem/16473581>, lower this
    // instruction to movi.16b instead.
    if (STI->hasZeroCycleZeroingFPWorkaround() &&
        MI->getOperand(1).getImm() == 0) {
      MCInst TmpInst;
      TmpInst.setOpcode(AArch64::MOVIv16b_ns);
      TmpInst.addOperand(MCOperand::createReg(MI->getOperand(0).getReg()));
      TmpInst.addOperand(MCOperand::createImm(MI->getOperand(1).getImm()));
      EmitToStreamer(*OutStreamer, TmpInst);
      return;
    }
    break;

  case AArch64::DBG_VALUE:
  case AArch64::DBG_VALUE_LIST:
    if (isVerbose() && OutStreamer->hasRawTextSupport()) {
      SmallString<128> TmpStr;
      raw_svector_ostream OS(TmpStr);
      PrintDebugValueComment(MI, OS);
      OutStreamer->emitRawText(StringRef(OS.str()));
    }
    return;

  case AArch64::EMITBKEY: {
      ExceptionHandling ExceptionHandlingType = MAI->getExceptionHandlingType();
      if (ExceptionHandlingType != ExceptionHandling::DwarfCFI &&
          ExceptionHandlingType != ExceptionHandling::ARM)
        return;

      if (getFunctionCFISectionType(*MF) == CFISection::None)
        return;

      OutStreamer->emitCFIBKeyFrame();
      return;
  }

  case AArch64::EMITMTETAGGED: {
    ExceptionHandling ExceptionHandlingType = MAI->getExceptionHandlingType();
    if (ExceptionHandlingType != ExceptionHandling::DwarfCFI &&
        ExceptionHandlingType != ExceptionHandling::ARM)
      return;

    if (getFunctionCFISectionType(*MF) != CFISection::None)
      OutStreamer->emitCFIMTETaggedFrame();
    return;
  }

  // Tail calls use pseudo instructions so they have the proper code-gen
  // attributes (isCall, isReturn, etc.). We lower them to the real
  // instruction here.
  case AArch64::TCRETURNri:
  case AArch64::TCRETURNriBTI:
  case AArch64::TCRETURNriALL: {
    MCInst TmpInst;
    TmpInst.setOpcode(AArch64::BR);
    TmpInst.addOperand(MCOperand::createReg(MI->getOperand(0).getReg()));
    EmitToStreamer(*OutStreamer, TmpInst);
    return;
  }
  case AArch64::TCRETURNdi: {
    if (LowerCangjieSpecificTCRETUR(*OutStreamer, SM, *MI))
      return;
    MCOperand Dest;
    MCInstLowering.lowerOperand(MI->getOperand(0), Dest);
    MCInst TmpInst;
    TmpInst.setOpcode(AArch64::B);
    TmpInst.addOperand(Dest);
    EmitToStreamer(*OutStreamer, TmpInst);
    return;
  }
  case AArch64::SpeculationBarrierISBDSBEndBB: {
    // Print DSB SYS + ISB
    MCInst TmpInstDSB;
    TmpInstDSB.setOpcode(AArch64::DSB);
    TmpInstDSB.addOperand(MCOperand::createImm(0xf));
    EmitToStreamer(*OutStreamer, TmpInstDSB);
    MCInst TmpInstISB;
    TmpInstISB.setOpcode(AArch64::ISB);
    TmpInstISB.addOperand(MCOperand::createImm(0xf));
    EmitToStreamer(*OutStreamer, TmpInstISB);
    return;
  }
  case AArch64::SpeculationBarrierSBEndBB: {
    // Print SB
    MCInst TmpInstSB;
    TmpInstSB.setOpcode(AArch64::SB);
    EmitToStreamer(*OutStreamer, TmpInstSB);
    return;
  }
  case AArch64::TLSDESC_CALLSEQ: {
    /// lower this to:
    ///    adrp  x0, :tlsdesc:var
    ///    ldr   x1, [x0, #:tlsdesc_lo12:var]
    ///    add   x0, x0, #:tlsdesc_lo12:var
    ///    .tlsdesccall var
    ///    blr   x1
    ///    (TPIDR_EL0 offset now in x0)
    const MachineOperand &MO_Sym = MI->getOperand(0);
    MachineOperand MO_TLSDESC_LO12(MO_Sym), MO_TLSDESC(MO_Sym);
    MCOperand Sym, SymTLSDescLo12, SymTLSDesc;
    MO_TLSDESC_LO12.setTargetFlags(AArch64II::MO_TLS | AArch64II::MO_PAGEOFF);
    MO_TLSDESC.setTargetFlags(AArch64II::MO_TLS | AArch64II::MO_PAGE);
    MCInstLowering.lowerOperand(MO_Sym, Sym);
    MCInstLowering.lowerOperand(MO_TLSDESC_LO12, SymTLSDescLo12);
    MCInstLowering.lowerOperand(MO_TLSDESC, SymTLSDesc);

    MCInst Adrp;
    Adrp.setOpcode(AArch64::ADRP);
    Adrp.addOperand(MCOperand::createReg(AArch64::X0));
    Adrp.addOperand(SymTLSDesc);
    EmitToStreamer(*OutStreamer, Adrp);

    MCInst Ldr;
    if (STI->isTargetILP32()) {
      Ldr.setOpcode(AArch64::LDRWui);
      Ldr.addOperand(MCOperand::createReg(AArch64::W1));
    } else {
      Ldr.setOpcode(AArch64::LDRXui);
      Ldr.addOperand(MCOperand::createReg(AArch64::X1));
    }
    Ldr.addOperand(MCOperand::createReg(AArch64::X0));
    Ldr.addOperand(SymTLSDescLo12);
    Ldr.addOperand(MCOperand::createImm(0));
    EmitToStreamer(*OutStreamer, Ldr);

    MCInst Add;
    if (STI->isTargetILP32()) {
      Add.setOpcode(AArch64::ADDWri);
      Add.addOperand(MCOperand::createReg(AArch64::W0));
      Add.addOperand(MCOperand::createReg(AArch64::W0));
    } else {
      Add.setOpcode(AArch64::ADDXri);
      Add.addOperand(MCOperand::createReg(AArch64::X0));
      Add.addOperand(MCOperand::createReg(AArch64::X0));
    }
    Add.addOperand(SymTLSDescLo12);
    Add.addOperand(MCOperand::createImm(AArch64_AM::getShiftValue(0)));
    EmitToStreamer(*OutStreamer, Add);

    // Emit a relocation-annotation. This expands to no code, but requests
    // the following instruction gets an R_AARCH64_TLSDESC_CALL.
    MCInst TLSDescCall;
    TLSDescCall.setOpcode(AArch64::TLSDESCCALL);
    TLSDescCall.addOperand(Sym);
    EmitToStreamer(*OutStreamer, TLSDescCall);

    MCInst Blr;
    Blr.setOpcode(AArch64::BLR);
    Blr.addOperand(MCOperand::createReg(AArch64::X1));
    EmitToStreamer(*OutStreamer, Blr);

    return;
  }

  case AArch64::JumpTableDest32:
  case AArch64::JumpTableDest16:
  case AArch64::JumpTableDest8:
    LowerJumpTableDest(*OutStreamer, *MI);
    return;

  case AArch64::FMOVH0:
  case AArch64::FMOVS0:
  case AArch64::FMOVD0:
    emitFMov0(*MI);
    return;

  case AArch64::MOPSMemoryCopyPseudo:
  case AArch64::MOPSMemoryMovePseudo:
  case AArch64::MOPSMemorySetPseudo:
  case AArch64::MOPSMemorySetTaggingPseudo:
    LowerMOPS(*OutStreamer, *MI);
    return;

  case TargetOpcode::STACKMAP:
    return LowerSTACKMAP(*OutStreamer, SM, *MI);

  case TargetOpcode::PATCHPOINT:
    return LowerPATCHPOINT(*OutStreamer, SM, *MI);

  case TargetOpcode::STATEPOINT:
    return LowerSTATEPOINT(*OutStreamer, SM, *MI);

  case TargetOpcode::FAULTING_OP:
    return LowerFAULTING_OP(*MI);

  case TargetOpcode::PATCHABLE_FUNCTION_ENTER:
    LowerPATCHABLE_FUNCTION_ENTER(*MI);
    return;

  case TargetOpcode::PATCHABLE_FUNCTION_EXIT:
    LowerPATCHABLE_FUNCTION_EXIT(*MI);
    return;

  case TargetOpcode::PATCHABLE_TAIL_CALL:
    LowerPATCHABLE_TAIL_CALL(*MI);
    return;

  case AArch64::HWASAN_CHECK_MEMACCESS:
  case AArch64::HWASAN_CHECK_MEMACCESS_SHORTGRANULES:
    LowerHWASAN_CHECK_MEMACCESS(*MI);
    return;

  case AArch64::SEH_StackAlloc:
    TS->emitARM64WinCFIAllocStack(MI->getOperand(0).getImm());
    return;

  case AArch64::SEH_SaveFPLR:
    TS->emitARM64WinCFISaveFPLR(MI->getOperand(0).getImm());
    return;

  case AArch64::SEH_SaveFPLR_X:
    assert(MI->getOperand(0).getImm() < 0 &&
           "Pre increment SEH opcode must have a negative offset");
    TS->emitARM64WinCFISaveFPLRX(-MI->getOperand(0).getImm());
    return;

  case AArch64::SEH_SaveReg:
    TS->emitARM64WinCFISaveReg(MI->getOperand(0).getImm(),
                               MI->getOperand(1).getImm());
    return;

  case AArch64::SEH_SaveReg_X:
    assert(MI->getOperand(1).getImm() < 0 &&
           "Pre increment SEH opcode must have a negative offset");
    TS->emitARM64WinCFISaveRegX(MI->getOperand(0).getImm(),
                                -MI->getOperand(1).getImm());
    return;

  case AArch64::SEH_SaveRegP:
    if (MI->getOperand(1).getImm() == 30 && MI->getOperand(0).getImm() >= 19 &&
        MI->getOperand(0).getImm() <= 28) {
      assert((MI->getOperand(0).getImm() - 19) % 2 == 0 &&
             "Register paired with LR must be odd");
      TS->emitARM64WinCFISaveLRPair(MI->getOperand(0).getImm(),
                                    MI->getOperand(2).getImm());
      return;
    }
    assert((MI->getOperand(1).getImm() - MI->getOperand(0).getImm() == 1) &&
            "Non-consecutive registers not allowed for save_regp");
    TS->emitARM64WinCFISaveRegP(MI->getOperand(0).getImm(),
                                MI->getOperand(2).getImm());
    return;

  case AArch64::SEH_SaveRegP_X:
    assert((MI->getOperand(1).getImm() - MI->getOperand(0).getImm() == 1) &&
            "Non-consecutive registers not allowed for save_regp_x");
    assert(MI->getOperand(2).getImm() < 0 &&
           "Pre increment SEH opcode must have a negative offset");
    TS->emitARM64WinCFISaveRegPX(MI->getOperand(0).getImm(),
                                 -MI->getOperand(2).getImm());
    return;

  case AArch64::SEH_SaveFReg:
    TS->emitARM64WinCFISaveFReg(MI->getOperand(0).getImm(),
                                MI->getOperand(1).getImm());
    return;

  case AArch64::SEH_SaveFReg_X:
    assert(MI->getOperand(1).getImm() < 0 &&
           "Pre increment SEH opcode must have a negative offset");
    TS->emitARM64WinCFISaveFRegX(MI->getOperand(0).getImm(),
                                 -MI->getOperand(1).getImm());
    return;

  case AArch64::SEH_SaveFRegP:
    assert((MI->getOperand(1).getImm() - MI->getOperand(0).getImm() == 1) &&
            "Non-consecutive registers not allowed for save_regp");
    TS->emitARM64WinCFISaveFRegP(MI->getOperand(0).getImm(),
                                 MI->getOperand(2).getImm());
    return;

  case AArch64::SEH_SaveFRegP_X:
    assert((MI->getOperand(1).getImm() - MI->getOperand(0).getImm() == 1) &&
            "Non-consecutive registers not allowed for save_regp_x");
    assert(MI->getOperand(2).getImm() < 0 &&
           "Pre increment SEH opcode must have a negative offset");
    TS->emitARM64WinCFISaveFRegPX(MI->getOperand(0).getImm(),
                                  -MI->getOperand(2).getImm());
    return;

  case AArch64::SEH_SetFP:
    TS->emitARM64WinCFISetFP();
    return;

  case AArch64::SEH_AddFP:
    TS->emitARM64WinCFIAddFP(MI->getOperand(0).getImm());
    return;

  case AArch64::SEH_Nop:
    TS->emitARM64WinCFINop();
    return;

  case AArch64::SEH_PrologEnd:
    TS->emitARM64WinCFIPrologEnd();
    return;

  case AArch64::SEH_EpilogStart:
    TS->emitARM64WinCFIEpilogStart();
    return;

  case AArch64::SEH_EpilogEnd:
    TS->emitARM64WinCFIEpilogEnd();
    return;
  }

  // Finally, do the automated lowerings for everything else.
  MCInst TmpInst;
  MCInstLowering.Lower(MI, TmpInst);
  EmitToStreamer(*OutStreamer, TmpInst);
}
// Note: emit specific inst should update inst size info in
// AArch64InstrInfo::getInstSizeInBytes for AArch64 at the same time
// adrp x9, xxx.CJStubGV
// add  x9, x9, :lo12:xxx.CJStubGV
// ldr  x9, [x9]
// mov  x10, #CallFrameSize
// stp  x9, x10, [rsp, #-CallFrameSize]!
void AArch64AsmPrinter::extendStackAndInsertFFIInfoForJmp(
    const MCOperand &SymOriAddr, const MCOperand &SymOriAddrLo12,
    unsigned CallFrameSize) {
  MCInst Adrp;
  Adrp.setOpcode(AArch64::ADRP);
  Adrp.addOperand(MCOperand::createReg(AArch64::X9));
  Adrp.addOperand(SymOriAddr);
  EmitToStreamer(*OutStreamer, Adrp);
  MCInst Add;

  Add.setOpcode(AArch64::ADDXri);
  Add.addOperand(MCOperand::createReg(AArch64::X9));
  Add.addOperand(MCOperand::createReg(AArch64::X9));
  Add.addOperand(SymOriAddrLo12);
  Add.addOperand(MCOperand::createImm(AArch64_AM::getShiftValue(0)));
  EmitToStreamer(*OutStreamer, Add);

  MCInst Ldr;
  Ldr.setOpcode(AArch64::LDRXui);
  Ldr.addOperand(MCOperand::createReg(AArch64::X9));
  Ldr.addOperand(MCOperand::createReg(AArch64::X9));
  Ldr.addOperand(MCOperand::createImm(0));
  EmitToStreamer(*OutStreamer, Ldr);

  MCInst Mov;
  Mov.setOpcode(AArch64::MOVZXi);
  Mov.addOperand(MCOperand::createReg(AArch64::X10));
  Mov.addOperand(MCOperand::createImm(CallFrameSize));
  Mov.addOperand(MCOperand::createImm(0));
  EmitToStreamer(*OutStreamer, Mov);

  MCInst Stp;
  Stp.setOpcode(AArch64::STPXpre);
  Stp.addOperand(MCOperand::createReg(AArch64::SP));
  Stp.addOperand(MCOperand::createReg(AArch64::X9));
  Stp.addOperand(MCOperand::createReg(AArch64::X10));
  Stp.addOperand(MCOperand::createReg(AArch64::SP));
  Stp.addOperand(MCOperand::createImm(-2));
  EmitToStreamer(*OutStreamer, Stp);
}
// Note: emit specific inst should update inst size info in
// AArch64InstrInfo::getInstSizeInBytes for AArch64 at the same time
void AArch64AsmPrinter::emitCangjieCallStubInstImpl(const MachineInstr *MI,
                                                    const Function *F,
                                                    const MachineOperand &MOSym,
                                                    unsigned Opcode) {
  auto *AddrGV = F->getParent()->getGlobalVariable(
      MOSym.getGlobal()->getName().str() + ".CJStubGV", true);
  MCOperand SymOriAddr = setGAAndLower(MOSym, AddrGV, AArch64II::MO_PAGE);
  MCOperand SymOriAddrLo12 =
      setGAAndLower(MOSym, AddrGV, AArch64II::MO_PAGEOFF | AArch64II::MO_NC);
  MCOperand SymNewAddr = setGAAndLower(MOSym, F);

  const auto *OriCalled = dyn_cast<Function>(MOSym.getGlobal());
  // push callee-addr and CallFrameSize to stack. This operation extends
  // the sp of the caller by 16 bytes and will be fixed in MCC_XXXStub
  MDNode *Metadata = OriCalled->getMetadata("CallFrameSizeForCJFFI");
  assert(Metadata != nullptr &&
         "FFI Func must have CallFrameSizeForCJFFI meta!");
  unsigned CallFrameSize =
      dyn_cast<ConstantInt>(
          dyn_cast<ConstantAsMetadata>(Metadata->getOperand(0).get())
              ->getValue())
          ->getSExtValue();
  // MCC_XXXStub expect the end of caller stack is like:
  // |  callee-addr                        |
  // |  param-stack-size (16 bytes align)  |
  extendStackAndInsertFFIInfoForJmp(SymOriAddr, SymOriAddrLo12, CallFrameSize);
  MCInst TemInst;
  if (Opcode == AArch64::TCRETURNdi) {
    Opcode = AArch64::B; // Branch directly for tailcall
  } else {
    assert(Opcode == AArch64::BL &&
           "Opcode should be BL or TCRETURNdi for Cangjie Call Stub");
  }
  TemInst.setOpcode(Opcode);
  TemInst.addOperand(SymNewAddr);
  EmitToStreamer(*OutStreamer, TemInst);
  SM.recordCJStackMap(*MI);
  return;
}

// Note: emit specific inst should update inst size info in
// AArch64InstrInfo::getInstSizeInBytes for AArch64 at the same time
//   ldr  x2, [x28, #AllocBufferOffset]        // get Alloc Buffer
//   ldr  x2, [x2]             // get region ptr
//   ldr  x3, [x2]             // x3 = allocPtr
//   ldr  x4, [x2, #8]         // x4 = limit
//   add  x5, x3, x1        // x5 = allocPtr + size
//   cmp  x5, x4               // allocPtr + size > limit ?
//   b.gt .LNewObjSlowPath
//   str  x0, [x3]             // allocPtr->KlassInfo = Klass
//   str  x5, [x2]             // region->allocPtr = allocPtr + size
//   mov  x0, x3               // ret/arg = allocPtr
//  <<< hasFinalizer
//   bl   MCC_OnFinalizerCreated
//  >>>
//   b    .LNewObjFin
// .LNewObjSlowPath
//   bl   MCC_NewObjectStub
// .LNewObjFin
void AArch64AsmPrinter::emitMccNewObjectForCopyGC(
    const ParamForEmitNewObj &Param) {
  using namespace AArch64;

  MCSymbol *LFast = OutContext.createTempSymbol("NewObjFastPath", true);
  MCSymbol *LSlow = OutContext.createTempSymbol("NewObjSlowPath", true);
  const MCSymbolRefExpr *SlowExpr = MCSymbolRefExpr::create(LSlow, OutContext);

  // 8: each imm represents 8 In MCInst.
  MCInst GetAllocBuffer = MCInstBuilder(LDRXui).addReg(X2).addReg(X28).addImm(
      getAllocBufferOffsetInCJTLS() / 8);
  // 0: offset of region Ptr in AllocBuffer is 0 bytes
  MCInst GetRegionPtr = MCInstBuilder(LDRXui).addReg(X2).addReg(X2).addImm(0);
  MCInst GetAllocPtr = MCInstBuilder(LDRXui).addReg(X3).addReg(X2).addImm(0);
  // 1: offset of limit in region is 8 bytes
  MCInst GetLimit = MCInstBuilder(LDRXui).addReg(X4).addReg(X2).addImm(1);
  MCInst CalNewAllocPtr =
      MCInstBuilder(ADDXrs).addReg(X5).addReg(X3).addReg(X1).addImm(0);
  MCInst CmpNewAllocPtrWithLimit =
      MCInstBuilder(SUBSXrs).addReg(XZR).addReg(X5).addReg(X4).addImm(0);
  MCInst BranchGToLSLow =
      MCInstBuilder(Bcc).addImm(AArch64CC::GT).addExpr(SlowExpr);
  MCInst SetKlass = MCInstBuilder(STRXui).addReg(X0).addReg(X3).addImm(0);
  MCInst StoreNewAllocPtr =
      MCInstBuilder(STRXui).addReg(X5).addReg(X2).addImm(0);
  MCInst CopyAllocPtrToX0 =
      MCInstBuilder(ORRXrs).addReg(X0).addReg(XZR).addReg(X3).addImm(0);
  MCInst BranchToFin = MCInstBuilder(B).addExpr(Param.FinExpr);
  OutStreamer->emitLabel(LFast);
  EmitToStreamer(*OutStreamer, GetAllocBuffer);
  EmitToStreamer(*OutStreamer, GetRegionPtr);
  EmitToStreamer(*OutStreamer, GetAllocPtr);
  EmitToStreamer(*OutStreamer, GetLimit);
  EmitToStreamer(*OutStreamer, CalNewAllocPtr);
  EmitToStreamer(*OutStreamer, CmpNewAllocPtrWithLimit);
  EmitToStreamer(*OutStreamer, BranchGToLSLow);
  EmitToStreamer(*OutStreamer, SetKlass);
  EmitToStreamer(*OutStreamer, StoreNewAllocPtr);
  EmitToStreamer(*OutStreamer, CopyAllocPtrToX0);
  if (Param.FastFuncName.equals("CJ_MCC_NewFinalizerFast")) {
    MCOperand FinalizerMC = setGAAndLower(
        Param.MOSym, Param.MI->getMF()->getFunction().getParent()->getFunction(
                         "CJ_MCC_OnFinalizerCreated"));
    EmitToStreamer(*OutStreamer,
                   MCInstBuilder(Param.Opcode).addOperand(FinalizerMC));
  }
  EmitToStreamer(*OutStreamer, BranchToFin);
  OutStreamer->emitLabel(LSlow);
  EmitToStreamer(*OutStreamer, Param.CallNewObjSlowPath);
  OutStreamer->emitLabel(Param.LFin);
}

void AArch64AsmPrinter::emitMccNewObjectFastPath(const MachineInstr *MI,
                                                 const MachineOperand &MOSym,
                                                 unsigned Opcode) {
  StringRef FastFuncName =
      (MOSym.getGlobal()->getName().find("Object") != StringRef::npos)
          ? "CJ_MCC_NewObjectFast"
          : "CJ_MCC_NewFinalizerFast";

  MCSymbol *LFinish = OutContext.createTempSymbol("NewObjFin", true);
  const MCSymbolRefExpr *FinExpr = MCSymbolRefExpr::create(LFinish, OutContext);
  MCOperand CallSlowPath;
  MCInstLowering.lowerOperand(MOSym, CallSlowPath);
  MCInst CallNewObjSlowPath = MCInstBuilder(Opcode).addOperand(CallSlowPath);
  ParamForEmitNewObj Param(MI, MOSym, Opcode, LFinish, FinExpr,
                           CallNewObjSlowPath, FastFuncName);
  emitMccNewObjectForCopyGC(Param);
  SM.recordCJStackMap(*MI);
}

void AArch64AsmPrinter::emitCJThrowException(const MachineInstr *MI,
                                             const MachineOperand &MOSym,
                                             unsigned Opcode) {
  MCOperand Call;
  MCInstLowering.lowerOperand(MOSym, Call);
  MCInst CallThrowException = MCInstBuilder(Opcode).addOperand(Call);
  EmitToStreamer(*OutStreamer, CallThrowException);
  StackMaps::CallsiteInfo CSInfo;
  SM.updateOrInsertFnInfo(CurrentFnSym, CSInfo);
}

// Note: emit specific inst should update inst size info in
// AArch64InstrInfo::getInstSizeInBytes for AArch64 at the same time
//   ldr  x4, [x28, #AllocBufferOffset]  // get Alloc Buffer
//   ldr  x4, [x4]             // get region ptr
//   ldr  x5, [x4]             // x5 = allocPtr
//   ldr  x6, [x4, #8]         // x6 = limit
//   add  x7, x5, x2           // x7 = allocPtr + size
//   cmp  x7, x6               // allocPtr + size > limit ?
//   b.gt .LNewArraySlowPath
//   str  x0, [x5]             // allocPtr->KlassInfo = Klass
//   str  x1, [x5, #8]         // allocPtr->Length = ArrayLength
//   str  x7, [x4]             // region->allocPtr = allocPtr + size
//   mov  x0, x5               // ret/arg = allocPtr
//   b    .LNewArrayFin
// .LNewArraySlowPath
//   bl   MCC_NewArray
// .LNewArrayFin
void AArch64AsmPrinter::emitCJNewArrayFastPath(const MachineInstr &MI,
                                               const MachineOperand &MOSym) {
  if (MI.getOpcode() != TargetOpcode::STATEPOINT) {
    report_fatal_error("New Obj Must be in Statepoint");
  }
  using namespace AArch64;

  MCSymbol *LFast = OutContext.createTempSymbol("NewArrayFastPath", true);
  MCSymbol *LSlow = OutContext.createTempSymbol("NewArraySlowPath", true);
  const MCSymbolRefExpr *SlowExpr = MCSymbolRefExpr::create(LSlow, OutContext);

  // 8: each imm represents 8 In MCInst.
  MCInst GetAllocBuffer = MCInstBuilder(LDRXui).addReg(X4).addReg(X28).addImm(
      getAllocBufferOffsetInCJTLS() / 8);
  // 0: offset of region Ptr in AllocBuffer is 0 bytes
  MCInst GetRegionPtr = MCInstBuilder(LDRXui).addReg(X4).addReg(X4).addImm(0);
  MCInst GetAllocPtr = MCInstBuilder(LDRXui).addReg(X5).addReg(X4).addImm(0);
  // 1: offset of limit in region is 8 bytes
  MCInst GetLimit = MCInstBuilder(LDRXui).addReg(X6).addReg(X4).addImm(1);
  MCInst CalNewAllocPtr =
      MCInstBuilder(ADDXrs).addReg(X7).addReg(X5).addReg(X2).addImm(0);
  MCInst CmpNewAllocPtrWithLimit =
      MCInstBuilder(SUBSXrs).addReg(XZR).addReg(X7).addReg(X6).addImm(0);
  MCInst BranchGToLSLow =
      MCInstBuilder(Bcc).addImm(AArch64CC::GT).addExpr(SlowExpr);
  MCInst SetKlass = MCInstBuilder(STRXui).addReg(X0).addReg(X5).addImm(0);
  MCInst StoreArrayLength =
      MCInstBuilder(STRXui).addReg(X1).addReg(X5).addImm(1);
  MCInst StoreNewAllocPtr =
      MCInstBuilder(STRXui).addReg(X7).addReg(X4).addImm(0);
  MCInst CopyAllocPtrToX0 =
      MCInstBuilder(ORRXrs).addReg(X0).addReg(XZR).addReg(X5).addImm(0);
  MCSymbol *LFinish = OutContext.createTempSymbol("NewArrayFin", true);
  const MCSymbolRefExpr *FinExpr = MCSymbolRefExpr::create(LFinish, OutContext);
  MCInst BranchToFin = MCInstBuilder(B).addExpr(FinExpr);
  MCOperand CallSlowPath;
  MCInstLowering.lowerOperand(MOSym, CallSlowPath);
  MCInst CallNewArraySlowPath = MCInstBuilder(BL).addOperand(CallSlowPath);
  OutStreamer->emitLabel(LFast);
  EmitToStreamer(*OutStreamer, GetAllocBuffer);
  EmitToStreamer(*OutStreamer, GetRegionPtr);
  EmitToStreamer(*OutStreamer, GetAllocPtr);
  EmitToStreamer(*OutStreamer, GetLimit);
  EmitToStreamer(*OutStreamer, CalNewAllocPtr);
  EmitToStreamer(*OutStreamer, CmpNewAllocPtrWithLimit);
  EmitToStreamer(*OutStreamer, BranchGToLSLow);
  EmitToStreamer(*OutStreamer, SetKlass);
  EmitToStreamer(*OutStreamer, StoreArrayLength);
  EmitToStreamer(*OutStreamer, StoreNewAllocPtr);
  EmitToStreamer(*OutStreamer, CopyAllocPtrToX0);
  EmitToStreamer(*OutStreamer, BranchToFin);
  OutStreamer->emitLabel(LSlow);
  EmitToStreamer(*OutStreamer, CallNewArraySlowPath);
  OutStreamer->emitLabel(LFinish);
  SM.recordCJStackMap(MI);
}

void AArch64AsmPrinter::emitCJNewArrayFastPath(const MachineOperand &MOSym) {
  using namespace AArch64;

  MCSymbol *LFast = OutContext.createTempSymbol("NewArrayFastPath", true);
  MCSymbol *LSlow = OutContext.createTempSymbol("NewArraySlowPath", true);
  const MCSymbolRefExpr *SlowExpr = MCSymbolRefExpr::create(LSlow, OutContext);

  // 8: each imm represents 8 In MCInst.
  MCInst GetAllocBuffer = MCInstBuilder(LDRXui).addReg(X4).addReg(X28).addImm(
      getAllocBufferOffsetInCJTLS() / 8);
  // 0: offset of region Ptr in AllocBuffer is 0 bytes
  MCInst GetRegionPtr = MCInstBuilder(LDRXui).addReg(X4).addReg(X4).addImm(0);
  MCInst GetAllocPtr = MCInstBuilder(LDRXui).addReg(X5).addReg(X4).addImm(0);
  // 1: offset of limit in region is 8 bytes
  MCInst GetLimit = MCInstBuilder(LDRXui).addReg(X6).addReg(X4).addImm(1);
  MCInst CalNewAllocPtr =
      MCInstBuilder(ADDXrs).addReg(X7).addReg(X5).addReg(X2).addImm(0);
  MCInst CmpNewAllocPtrWithLimit =
      MCInstBuilder(SUBSXrs).addReg(XZR).addReg(X7).addReg(X6).addImm(0);
  MCInst BranchGToLSLow =
      MCInstBuilder(Bcc).addImm(AArch64CC::GT).addExpr(SlowExpr);
  MCInst SetKlass = MCInstBuilder(STRXui).addReg(X0).addReg(X5).addImm(0);
  MCInst StoreArrayLength =
      MCInstBuilder(STRXui).addReg(X1).addReg(X5).addImm(1);
  MCInst StoreNewAllocPtr =
      MCInstBuilder(STRXui).addReg(X7).addReg(X4).addImm(0);
  MCInst CopyAllocPtrToX0 =
      MCInstBuilder(ORRXrs).addReg(X0).addReg(XZR).addReg(X5).addImm(0);
  MCSymbol *LFinish = OutContext.createTempSymbol("NewArrayFin", true);
  const MCSymbolRefExpr *FinExpr = MCSymbolRefExpr::create(LFinish, OutContext);
  MCInst BranchToFin = MCInstBuilder(B).addExpr(FinExpr);
  MCOperand CallSlowPath;
  MCInstLowering.lowerOperand(MOSym, CallSlowPath);
  MCInst CallNewArraySlowPath = MCInstBuilder(B).addOperand(CallSlowPath);
  OutStreamer->emitLabel(LFast);
  EmitToStreamer(*OutStreamer, GetAllocBuffer);
  EmitToStreamer(*OutStreamer, GetRegionPtr);
  EmitToStreamer(*OutStreamer, GetAllocPtr);
  EmitToStreamer(*OutStreamer, GetLimit);
  EmitToStreamer(*OutStreamer, CalNewAllocPtr);
  EmitToStreamer(*OutStreamer, CmpNewAllocPtrWithLimit);
  EmitToStreamer(*OutStreamer, BranchGToLSLow);
  EmitToStreamer(*OutStreamer, SetKlass);
  EmitToStreamer(*OutStreamer, StoreArrayLength);
  EmitToStreamer(*OutStreamer, StoreNewAllocPtr);
  EmitToStreamer(*OutStreamer, CopyAllocPtrToX0);
  EmitToStreamer(*OutStreamer, BranchToFin);
  OutStreamer->emitLabel(LSlow);
  EmitToStreamer(*OutStreamer, CallNewArraySlowPath);
  OutStreamer->emitLabel(LFinish);

  MCInst RetInst = MCInstBuilder(AArch64::RET)
                    .addReg(AArch64::LR);
  EmitToStreamer(*OutStreamer, RetInst);
}

void AArch64AsmPrinter::emitGetCJThreadId() {
  int64_t cjthreadIdOffset = 456;
  // 8: each imm represents 8 In MCInst.
  int64_t OffsetDivisor = 8;
  auto &Ctx = OutStreamer->getContext();
  MCSymbol *MILabel = Ctx.createTempSymbol("cjthread_id_end", true);
  const MCSymbolRefExpr *MILabelExpr = MCSymbolRefExpr::create(MILabel, OutContext);
  // ldr x9, [x28, #16]
  emitGetCJTLSData(getCJThreadOffsetInCJTLS());
  // cbz x9, .L_cjthread_id_end
  MCInst CbzInst;
  CbzInst.setOpcode(AArch64::CBZW);
  CbzInst.addOperand(MCOperand::createReg(AArch64::X9));
  CbzInst.addOperand(MCOperand::createExpr(MILabelExpr));
  OutStreamer->emitInstruction(CbzInst, getSubtargetInfo());
  // ldr x9, [x9, #456]
  MCInst LoadCJThreadInst;
  LoadCJThreadInst.setOpcode(AArch64::LDRXui);
  LoadCJThreadInst.addOperand(MCOperand::createReg(AArch64::X9));
  LoadCJThreadInst.addOperand(MCOperand::createReg(AArch64::X9));
  LoadCJThreadInst.addOperand(MCOperand::createImm(cjthreadIdOffset / OffsetDivisor));
  LoadCJThreadInst.addOperand(MCOperand::createImm(0));
  EmitToStreamer(*OutStreamer, LoadCJThreadInst);
  // .L_cjthread_id_end
  OutStreamer->emitLabel(MILabel);
  return;
}

void AArch64AsmPrinter::emitMetadataAddress() {
  const Triple &TT = getSubtargetInfo().getTargetTriple();
  MCSymbol *MetadataStart = OutContext.getOrCreateSymbol("__CJMetadataStart");
  MCContext &Ctx = MF->getContext();
  if (TT.isOSLinux()) {
    // adrp    x0, __CJMetadataStart
    // add     x0, x0, :lo12:__CJMetadataStart
    // bl      CJ_MRT_PreInitializePackage
    MachineOperand MOsym(
        MachineOperand::CreateMCSymbol(MetadataStart, AArch64II::MO_PAGE));
    MCOperand SymOriAddr;
    MCInstLowering.lowerOperand(MOsym, SymOriAddr);
    MachineOperand MOsymLo12(MachineOperand::CreateMCSymbol(
        MetadataStart, AArch64II::MO_PAGEOFF | AArch64II::MO_NC));
    MCOperand SymOriAddrLo12;
    MCInstLowering.lowerOperand(MOsymLo12, SymOriAddrLo12);
    MCInst Adrp =
        MCInstBuilder(AArch64::ADRP).addReg(AArch64::X0).addOperand(SymOriAddr);
    EmitToStreamer(*OutStreamer, Adrp);
    MCInst Add = MCInstBuilder(AArch64::ADDXri)
                     .addReg(AArch64::X0)
                     .addReg(AArch64::X0)
                     .addOperand(SymOriAddrLo12)
                     .addImm(0);
    EmitToStreamer(*OutStreamer, Add);
    MCSymbol *FuncSym =
        OutContext.getOrCreateSymbol("CJ_MRT_PreInitializePackage");
    MCInst BLToCall = MCInstBuilder(AArch64::BL)
                          .addExpr(MCSymbolRefExpr::create(FuncSym, Ctx));
    EmitToStreamer(*OutStreamer, BLToCall);
  } else if (TT.isOSBinFormatMachO()) {
    // adrp    x0, __CJMetadataStart@GOTPAGE
    // ldr     x0, [x0, __CJMetadataStart@GOTPAGEOFF]
    // ldr     x0, [x0]
    // bl      CJ_MRT_PreInitializePackage
    MachineOperand MOsym(MachineOperand::CreateMCSymbol(
        MetadataStart, AArch64II::MO_PAGE | AArch64II::MO_GOT));
    MCOperand SymOriAddr;
    MCInstLowering.lowerOperand(MOsym, SymOriAddr);
    MachineOperand MOsymLo12(MachineOperand::CreateMCSymbol(
        MetadataStart,
        AArch64II::MO_PAGEOFF | AArch64II::MO_NC | AArch64II::MO_GOT));
    MCOperand SymOriAddrLo12;
    MCInstLowering.lowerOperand(MOsymLo12, SymOriAddrLo12);

    MCInst Adrp =
        MCInstBuilder(AArch64::ADRP).addReg(AArch64::X0).addOperand(SymOriAddr);
    EmitToStreamer(*OutStreamer, Adrp);
    MCInst Ldr = MCInstBuilder(AArch64::LDRXui)
                     .addReg(AArch64::X0)
                     .addReg(AArch64::X0)
                     .addOperand(SymOriAddrLo12)
                     .addImm(0);
    EmitToStreamer(*OutStreamer, Ldr);
    MCInst Ldr2 = MCInstBuilder(AArch64::LDRXui)
                      .addReg(AArch64::X0)
                      .addReg(AArch64::X0)
                      .addImm(0);
    EmitToStreamer(*OutStreamer, Ldr2);
    MCSymbol *FuncSym =
        OutContext.getOrCreateSymbol("_CJ_MRT_PreInitializePackage");
    MCInst BLToCall = MCInstBuilder(AArch64::BL)
                          .addExpr(MCSymbolRefExpr::create(FuncSym, Ctx));
    EmitToStreamer(*OutStreamer, BLToCall);
  }
}

// Force static initialization.
extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeAArch64AsmPrinter() {
  RegisterAsmPrinter<AArch64AsmPrinter> X(getTheAArch64leTarget());
  RegisterAsmPrinter<AArch64AsmPrinter> Y(getTheAArch64beTarget());
  RegisterAsmPrinter<AArch64AsmPrinter> Z(getTheARM64Target());
  RegisterAsmPrinter<AArch64AsmPrinter> W(getTheARM64_32Target());
  RegisterAsmPrinter<AArch64AsmPrinter> V(getTheAArch64_32Target());
}
