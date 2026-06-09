//===- CJMetadata.cpp - Self-Defined CJMetadata Section ----------*- C++-*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file generates cangjie self-defined CJMetadata section.
//
//===----------------------------------------------------------------------===//
#include "llvm/CodeGen/CJMetadata.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/GCMetadata.h"
#include "llvm/CodeGen/StackMaps.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCSectionCOFF.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCSectionMachO.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Target/TargetMachine.h"

namespace llvm {
static cl::opt<bool> NoStackTraceInfo("no-stacktrace-info", cl::init(false),
                                      cl::NotHidden,
                                      cl::desc("disable cj stack trace info"));
enum class StackTraceFormat { Simple, Default, All };
static cl::opt<StackTraceFormat> StackTraceFormatFlag(
    "stack-trace-format", cl::init(StackTraceFormat::Default), cl::NotHidden,
    cl::desc("The format of cj stack trace"),
    cl::values(clEnumValN(StackTraceFormat::Simple, "simple",
                          "with only filename and linenumber"),
               clEnumValN(StackTraceFormat::Default, "default",
                          "without generic info"),
               clEnumValN(StackTraceFormat::All, "all", "with all info")));

extern cl::opt<bool> DisableGCSupport;
extern cl::opt<bool> EnableBarrierOnly;
extern cl::opt<bool> EnableSafepointOnly;
extern cl::opt<bool> EnableStackGrow;

// StrPoolDictSplitSize is defined through elapsed time test on dict loading
// and string decoding. The maximum size of 5000 can keep the elapsed time below
// 1 millisecond, which is a desired cost. When the string pool dict size
// is between 1000 and 5000, the split size will be correspondingly calculated
// by function getStrPoolDictSplitSize().
static uint32_t StrPoolDictSplitMaxSize = 5000;
static uint32_t StrPoolDictSplitMinSize = 1000;

namespace {
const std::string StrPool(".Lstr_pool.");
const std::string StrPoolDict(".Lstr_pool_dict.");
} // namespace
CJMetadataInfo::CJMetadataInfo(AsmPrinter &AP, StackMaps &SM)
    : AP(AP), SM(SM), OS(*AP.OutStreamer), Context(AP.OutContext),
      TT(AP.TM.getTargetTriple()) {}

void processStructTypeOffsets(StructType *ST, SmallVectorImpl<uint64_t> &Refs,
                              const DataLayout &DL, const GCStrategy *GS,
                              uint64_t BaseOff) {
  for (uint32_t STNum = 0; STNum < ST->getNumElements(); STNum++) {
    if (auto PT = dyn_cast<PointerType>(ST->getElementType(STNum))) {
      if (*GS->isGCManagedPointer(PT)) {
        Refs.push_back(DL.getStructLayout(ST)->getElementOffset(STNum) +
                       BaseOff);
      }
    } else if (auto EST = dyn_cast<StructType>(ST->getElementType(STNum))) {
      uint64_t DerivedOffset =
          DL.getStructLayout(ST)->getElementOffset(STNum) + BaseOff;
      processStructTypeOffsets(EST, Refs, DL, GS, DerivedOffset);
    }
  }
}

void CJMetadataInfo::init() {
  const MCObjectFileInfo *MOFI = Context.getObjectFileInfo();
  TD[SDKVersionIdx].TableSection = MOFI->getCJSDKVersionSection();
  TD[MethodInfoTableIdx].TableSection = MOFI->getCJMethodInfoSection();
  TD[GlobalInitFuncIdx].TableSection = MOFI->getCJGlobalInitFuncSection();
  TD[StringPoolDictTableIdx].TableSection = MOFI->getCJStringPoolDictSection();
  TD[StringPoolTableIdx].TableSection = MOFI->getCJStringPoolSection();
  TD[StackMapTableIdx].TableSection = MOFI->getCJStackMapSection();
  TD[GCTibTableIdx].TableSection = MOFI->getCJGCTibSection();
  TD[GCRootsTableIdx].TableSection = MOFI->getCJGCRootSection();
  TD[TypeInfoTableIdx].TableSection = MOFI->getTypeInfoSection();
  TD[TypeTemplateIdx].TableSection = MOFI->getCJTypeTemplateSection();
  TD[TypeFieldsIdx].TableSection = MOFI->getCJTypeFieldsSection();
  TD[TypeExtTableIdx].TableSection = MOFI->getTypeExtSection();
  TD[MTableIdx].TableSection = MOFI->getCJMtableSection();
  TD[StaticGIIdx].TableSection = MOFI->getCJStaticGenericTISection();
  TD[GCFlagsIdx].TableSection = MOFI->getCJGCFlagsSection();
  TD[ReflectPkgInfoIdx].TableSection = MOFI->getCJReflectPkgInfoSection();
  TD[ReflectGVInfoIdx].TableSection = MOFI->getCJReflectGVSection();
  TD[ReflectGIIdx].TableSection = MOFI->getCJReflectGenericTISection();
  TD[InnerTypeExtensionsIdx].TableSection =
      MOFI->getCJInnerTypeExtensionsSection();
  TD[OuterTypeExtensionsIdx].TableSection =
      MOFI->getCJOuterTypeExtensionsSection();
  String2Symbol.clear();
  SplitedMangledStr.Items.clear();
  StrPoolDictOffsetsSym = Context.getOrCreateSymbol(".Lstr_pool_dict_offsets");
  StrPoolIdx = 0;
  FuncPtrSize = SM.isARM() ? 4 : 8; // method pc size, arm: 4 byte, other: 8 byte
}

void CJMetadataInfo::recordKlass(const GlobalVariable *GV) {
  if (!GV->hasInitializer()) {
    ExternalTITable.push_back(GV); // external , undef classmeta
    return;
  }
  InternalTITable.push_back(GV);
}

void CJMetadataInfo::recordGlobalVariable(const GlobalVariable *GV) {
  if (!GV->isCJMeta())
    return;

  auto HandleGVAttribute = [this](const GlobalVariable *GV) -> void {
    if (GV->hasAttribute("CFileReflect")) {
      if (GV->getName().endswith(".packageInfo")) {
        ReflectPkgInfoGV.push_back(GV);
      } else if (GV->getName().endswith(".reflectStr")) {
        ReflectStrGV.push_back(GV);
      } else {
        ReflectOtherGV.push_back(GV);
      }
      return;
    }

    if (GV->isCJTypeTemplate()) {
      TypeTemplateGVTable.push_back(GV);
      return;
    }

    if (GV->isCJReflectGeneticTI()) {
      ReflectGeneticTI.push_back(GV);
      return;
    }

    if (GV->isCJTypeInfo()) {
      recordKlass(GV);
      return;
    }

    if (GV->isCJTypeName() || GV->isCJTIOffsets() || GV->isCJTITypeArgs() ||
        GV->isCJTIFields() || GV->isCJTTFieldsFns() ||
        GV->isCJReflectUpperBounds() || GV->isCJFunctableTable()) {
      TIFieldsTable.push_back(GV);
      return;
    }

    if (GV->isCJMTable()) {
      MTableTable.push_back(GV);
      return;
    }

    if (GV->isCJTypeExt()) {
      TypeExtTable.push_back(GV);
      return;
    }

    if (GV->isCJGCTib()) {
      GCTibTable.push_back(GV);
      return;
    }

    if (GV->isCJStaticGenericTI()) {
      assert(GV->hasInitializer() && "StaticGenericTI has no Initializer!");
      const Constant *C = GV->getInitializer();
      for (unsigned Idx = 0; Idx < C->getNumOperands(); Idx++)
        StaticGenericTI.push_back(cast<GlobalValue>(C->getOperand(Idx)));
      return;
    }

    if (GV->isCJInnerTypeExtensions()) {
      assert(GV->hasInitializer());
      InnerTypeExtensions.push_back(GV);
      return;
    }

    if (GV->isCJOuterTypeExtensions()) {
      assert(GV->hasInitializer());
      auto *C = GV->getInitializer();
      for (unsigned I = 0; I < C->getNumOperands(); ++I)
        OuterTypeExtensions.push_back(cast<GlobalVariable>(C->getOperand(I)));
      return;
    }
  };

  HandleGVAttribute(GV);

  if (!GV->hasExactDefinition() && AP.TM.getRelocationModel() == Reloc::PIC_ &&
      !GV->hasWeakODRLinkage())
    return;

  // Func can have different GC Strategy, but GV has only one GC Strategy.
  GCStrategy *GS = AP.getAnalysisIfAvailable<GCModuleInfo>()->getGCStrategy(
      StringRef("cangjie"));

  // GCRoots
  if (auto PT = dyn_cast<PointerType>(GV->getValueType())) {
    if (*GS->isGCManagedPointer(PT)) {
      GCRootTable.push_back(getGVRefSymbol(GV));
    }
  }

  // also stack map use.
  if (StructType *ST = dyn_cast<StructType>(GV->getValueType())) {
    // 8: Initial size of RefOffsets.
    SmallVector<uint64_t, 8> RefOffsets;
    processStructTypeOffsets(ST, RefOffsets, GV->getParent()->getDataLayout(),
                             GS, 0);
    if (RefOffsets.size() == 0)
      return;

    const MCExpr *GVRef = getGVRefSymbol(GV);
    for (const auto Off : RefOffsets) {
      const MCExpr *GVOff = MCBinaryExpr::createAdd(
          GVRef, MCConstantExpr::create(Off, Context), Context);
      GCRootTable.push_back(GVOff);
    }
  }
}

void CJMetadataInfo::recordMetadata() {
  for (const auto &G : M->globals())
    recordGlobalVariable(&G);

  recordExternalMethod();
}

void CJMetadataInfo::recordExternalMethod() {
  for (const auto &F : M->functions())
    if (F.isDeclaration() && F.hasCangjieGC())
      ExternalMethod.push_back(&F);
}

void CJMetadataInfo::recordCurrentFunc() {
  const Function &F = AP.MF->getFunction();
  if (!F.hasCangjieGC() || F.hasFnAttribute("leaf-function"))
    return;

  // pc + methodinfo. stackmap symbol
  if (F.hasComdat()) {
    ComdatMethodTable.push_back(
        std::make_tuple(AP.CurrentFnSym, AP.getFunctionBegin(),
                        AP.getFunctionEnd(), AP.MF->getFunctionNumber(), &F));
  } else if (F.hasFnAttribute("cjinit")) {
    InitMethodTable.push_back(
        std::make_tuple(AP.CurrentFnSym, AP.getFunctionBegin(),
                        AP.getFunctionEnd(), AP.MF->getFunctionNumber()));
  } else {
    MethodTable.push_back(
        std::make_tuple(AP.CurrentFnSym, AP.getFunctionBegin(),
                        AP.getFunctionEnd(), AP.MF->getFunctionNumber()));
  }
}

const MCExpr *CJMetadataInfo::getGVRefSymbol(const GlobalValue *GV) {
  if (const GlobalAlias *GA = dyn_cast<GlobalAlias>(GV)) {
    // Directly use the alias symbol itself
    MCSymbol *GASymbol = AP.TM.getSymbol(GV);
    return MCSymbolRefExpr::create(GASymbol, Context);
  }
  MCSymbol *GVSymbol = AP.TM.getSymbol(GV);
  if (GV->hasExactDefinition()) {
    MCSymbol *RefSym =
        Context.getOrCreateSymbol(Twine(".LRef.") + GVSymbol->getName());
    return MCSymbolRefExpr::create(RefSym, Context);
  }
  return MCSymbolRefExpr::create(GVSymbol, Context);
}

const MCExpr *CJMetadataInfo::getOrInsertStrPoolOffset(std::string &Str,
                                                       const MCSymbol *DescSym,
                                                       bool MachONeedNoOffset) {
  auto Itr = String2Symbol.find(Str);
  MCSymbol *StrPoolSym = nullptr;
  if (Itr == String2Symbol.end()) {
    StrPoolSym =
        Context.getOrCreateSymbol(StrPool + std::to_string(StrPoolIdx++));
    String2Symbol[Str] = StrPoolSym;
  } else {
    StrPoolSym = Itr->second;
  }
  assert(StrPoolSym && ".Lstr_pool symbol should not be null");
  if (MachONeedNoOffset) {
    return MCSymbolRefExpr::create(StrPoolSym, Context);
  }
  return MCBinaryExpr::createSub(MCSymbolRefExpr::create(StrPoolSym, Context),
                                 MCSymbolRefExpr::create(DescSym, Context),
                                 Context);
}

uint64_t CJMetadataInfo::addMangledStr(std::string &Str) {
  return SplitedMangledStr.getOrInsertIndex(Str) + 1;
}

bool isCurrentCharDigit(char C) { return C >= '0' && C <= '9'; }

void CJMetadataInfo::splitStr(std::string &Str,
                              std::vector<uint64_t> &CompressedCode) {
  if (Str.empty())
    return;

  uint32_t CurIdx = 0;
  uint32_t StrLen = Str.length();
  std::string TmpStr;
  while (CurIdx < StrLen) {
    if (!isCurrentCharDigit(Str[CurIdx])) {
      TmpStr += Str[CurIdx];
      CurIdx++;
      continue;
    }
    if (!TmpStr.empty()) {
      CompressedCode.push_back(addMangledStr(TmpStr));
      TmpStr.clear();
    }
    bool NeedNestedSplit = false;
    if (Str[CurIdx] == '0') {
      NeedNestedSplit = true;
    }
    std::string NumStr;
    while (CurIdx < StrLen && isCurrentCharDigit(Str[CurIdx])) {
      NumStr += Str[CurIdx];
      ++CurIdx;
    }
    uint32_t IdentifyLen = atoi(NumStr.c_str());
    if (IdentifyLen == 0) {
      CompressedCode.push_back(addMangledStr(NumStr));
      continue;
    }
    if (IdentifyLen > StrLen - CurIdx) {
      CompressedCode.push_back(addMangledStr(NumStr));
      continue;
    }
    TmpStr = Str.substr(CurIdx, IdentifyLen);
    CurIdx += IdentifyLen;
    for (uint32_t i = 0; i < TmpStr.size(); i++) {
      if (isCurrentCharDigit(TmpStr[i])) {
        NeedNestedSplit = true;
        break;
      }
    }
    if (NeedNestedSplit) {
      CompressedCode.push_back(addMangledStr(NumStr));
      splitStr(TmpStr, CompressedCode);
    } else {
      auto Comb = NumStr + TmpStr;
      CompressedCode.push_back(addMangledStr(Comb));
    }
    TmpStr.clear();
    continue;
  }
  if (!TmpStr.empty()) {
    CompressedCode.push_back(addMangledStr(TmpStr));
  }
}

// ULEB: The highest bit of each byte is used to indicate whether the encoding
// is complete, and the remaining 7 bits are used to represent the integer
// value.
void ulebEncodeSignleStr(uint64_t Val, std::string &Result) {
  do {
    uint8_t Byte = Val & 0x7f;
    Val >>= 7;
    if (Val != 0) {
      Byte |= 0x80;
    }
    Result.push_back(Byte);
  } while (Val != 0);
}

std::string ulebEncode(std::vector<std::uint64_t> &Strs) {
  std::string StrCode;
  for (auto StrIndex : Strs) {
    ulebEncodeSignleStr(StrIndex, StrCode);
  }
  return StrCode;
}

void CJMetadataInfo::emitStackTraceInfo(const MCSymbol *FuncSym,
                                        const MCSymbol *DescSym) {
  Function *Func = nullptr;
  if (IsMachO) {
    // 1: FuncSym name is _xxx in macos, so begin from 1 to get xxx.
    Func = M->getFunction(FuncSym->getName().substr(1));
  } else {
    Func = M->getFunction(FuncSym->getName());
  }
  unsigned StrSize = 4; // string offset size, 4 bytes
  if (NoStackTraceInfo || !Func) {
    OS.emitIntValue(0, StrSize);
    OS.emitIntValue(0, StrSize);
    OS.emitIntValue(0, StrSize);
    OS.emitIntValue(0, StrSize);
    return;
  }

  // set default value "" for  dir and fileName
  std::string DirStr("");
  std::string FileNameStr("");
  std::string MethodNameStr("");
  if (!Func->hasFnAttribute("cj_stack_trace_omit")) {
    MethodNameStr = FuncSym->getName().str();
    DISubprogram *SP = Func->getSubprogram();
    if (SP != nullptr) {
      DIFile *File = SP->getFile();
      if (File != nullptr) {
        DirStr = File->getDirectory().str();
        FileNameStr = File->getFilename().str();
      }
    }
  }
  if (StackTraceFormatFlag == StackTraceFormat::Simple)
    MethodNameStr = "";
  std::vector<uint64_t> MethodCompressedCode;
  splitStr(MethodNameStr, MethodCompressedCode);
  std::string MethodNameStrIndex = ulebEncode(MethodCompressedCode);

  std::vector<uint64_t> DirCompressedCode;
  splitStr(DirStr, DirCompressedCode);
  std::string DirStrIndex = ulebEncode(DirCompressedCode);

  std::vector<uint64_t> FileCompressedCode;
  splitStr(FileNameStr, FileCompressedCode);
  std::string FileNameStrIndex = ulebEncode(FileCompressedCode);

  OS.emitValue(getOrInsertStrPoolOffset(MethodNameStrIndex, DescSym), StrSize);
  OS.emitValue(getOrInsertStrPoolOffset(DirStrIndex, DescSym), StrSize);
  OS.emitValue(getOrInsertStrPoolOffset(FileNameStrIndex, DescSym), StrSize);

  if (MethodCompressedCode.empty() && DirCompressedCode.empty() &&
      FileCompressedCode.empty()) {
    OS.emitIntValue(0, 4);
  } else {
    OS.emitValue(
        MCBinaryExpr::createSub(
            MCSymbolRefExpr::create(StrPoolDictOffsetsSym, AP.OutContext),
            MCSymbolRefExpr::create(DescSym, AP.OutContext), AP.OutContext),
        4);
  }
  return;
}

void CJMetadataInfo::emitEHTableOffset(unsigned FuncNumber) {
  MCSymbol *CurPCSymbol = Context.createTempSymbol();
  OS.emitLabel(CurPCSymbol);
  MCSymbol *EHTableSymbol =
      Context.lookupSymbol(Twine("CJ_except_table") + Twine(FuncNumber));
  if (EHTableSymbol != nullptr) {
    if (IsMachO) {
      const MCExpr *EHTableAddr =
          MCSymbolRefExpr::create(EHTableSymbol, Context);
      // 8: ehtable addr size, 8 bytes
      OS.emitValue(EHTableAddr, 8);
    } else {
      const MCExpr *EHTableOffset = MCBinaryExpr::createSub(
          MCSymbolRefExpr::create(EHTableSymbol, Context),
          MCSymbolRefExpr::create(CurPCSymbol, Context), Context);
      // 4: ehtable offset size, 4 bytes
      OS.emitValue(EHTableOffset, 4);
    }
  } else {
    // MachO is 8 bytes, others are 4 bytes
    IsMachO ? OS.emitIntValue(0, 8) : OS.emitIntValue(0, 4);
  }
}

void CJMetadataInfo::emitDatas(const MCSymbol *FuncName,
                               const MCSymbol *FuncBegin,
                               const MCSymbol *FuncEnd, unsigned FuncNumber) {
  // Emit MethodInfo symbol
  MCSymbol *DescSymbol = nullptr;
  if (IsMachO) {
    Metadata *MD = M->getModuleFlag("Cangjie_PACKAGE_ID");
    if (MD == nullptr)
      report_fatal_error("There is not cangjie package id in module!");
    StringRef PACKAGEID = dyn_cast<MDString>(MD)->getString();
    DescSymbol = Context.getOrCreateSymbol(".Lmethod_desc." + PACKAGEID + "." +
                                           FuncName->getName());
    OS.emitSymbolAttribute(DescSymbol, MCSA_Global);
  } else {
    DescSymbol =
        Context.getOrCreateSymbol(".Lmethod_desc." + FuncName->getName());
  }
  OS.emitLabel(DescSymbol);
  // Emit StackMap offset
  MCSymbol *SMSymbol =
      Context.lookupSymbol(".Lstack_map." + FuncName->getName());
  if (SMSymbol == nullptr) {
    // 4: StackMap offset size, 4 bytes
    OS.emitIntValue(0, 4);
  } else {
    const MCExpr *MethodStackMapOffset = MCBinaryExpr::createSub(
        MCSymbolRefExpr::create(SMSymbol, Context),
        MCSymbolRefExpr::create(DescSymbol, Context), Context);
    OS.emitValue(MethodStackMapOffset, 4);
  }
  // Emit Function size.
  if (LLVM_UNLIKELY(FuncBegin == nullptr)) {
    // 4: FuncSize size, 4 bytes
    OS.emitIntValue(0, 4);
  } else {
    const MCExpr *FuncSize = MCBinaryExpr::createSub(
        MCSymbolRefExpr::create(FuncEnd, Context),
        MCSymbolRefExpr::create(FuncBegin, Context), Context);
    // 4: FuncSize size, 4 bytes
    OS.emitValue(FuncSize, 4);
  }
  // Emit methodName/directory/filename symbol.
  emitStackTraceInfo(FuncName, DescSymbol);
  // Emit EHTable offset.
  // Because the front is 64-bit aligned, it must be placed behind
  // the emitstacktraceinfo.
  emitEHTableOffset(FuncNumber);
}

void CJMetadataInfo::emitMethodInfoTable() {
  if (InitMethodTable.empty() && MethodTable.empty() &&
      ComdatMethodTable.empty())
    return;

  if (TT.isOSBinFormatMachO()) {
    OS.switchSection(Context.getMachOSection("__TEXT", "__cjinit_func", 0,
                                             SectionKind::getExecuteOnly()));
    OS.emitLabel(Context.getOrCreateSymbol(".Lcjinit_end"));
  }

  OS.switchSection(TD[MethodInfoTableIdx].TableSection);
  if (TT.isOSBinFormatMachO())
    // 8: align size, 8 bytes
    OS.emitValueToAlignment(8);
  // Emit Method info section.
  for (const auto &Method : MethodTable) {
    // 0: FuncName symbol, 1 : FuncBegin symbol
    emitDatas(std::get<0>(Method), std::get<1>(Method),
              // 2: FuncEnd symbol, 3: funcNumber
              std::get<2>(Method), std::get<3>(Method));
  }
  // Emit cj-init Method info section.
  for (const auto &Method : InitMethodTable) {
    // 0: FuncName symbol, 1 : FuncBegin symbol
    emitDatas(std::get<0>(Method), std::get<1>(Method),
              // 2: FuncEnd symbol, 3: funcNumber
              std::get<2>(Method), std::get<3>(Method));
  }
  // Emit Comdat Method info section.
  for (const auto &Method : ComdatMethodTable) {
    const Function *F = std::get<4>(Method);
    StringRef Group = F->getComdat()->getName();
    MCSection *CJComdatMethodInfoSection = nullptr;
    if (TT.isOSBinFormatELF()) {
      CJComdatMethodInfoSection = Context.getELFSection(
          ".cjmetadata.methodinfo." + Group, ELF::SHT_PROGBITS,
          ELF::SHF_ALLOC | ELF::SHF_WRITE | ELF::SHF_GROUP, 0, Group, false);
    } else if (TT.isOSBinFormatCOFF()) {
      CJComdatMethodInfoSection = Context.getCOFFSection(
          ".cjmthd.",
          COFF::IMAGE_SCN_CNT_INITIALIZED_DATA | COFF::IMAGE_SCN_MEM_READ |
              COFF::IMAGE_SCN_MEM_WRITE,
          SectionKind::getReadOnly());
    } else if (TT.isOSBinFormatMachO()) {
      CJComdatMethodInfoSection = Context.getMachOSection(
          "__CJ_METADATA", "__cjmethodinfo_", 0, SectionKind::getReadOnly());
    } else {
      report_fatal_error("unsupport object format!");
    }
    OS.switchSection(CJComdatMethodInfoSection);
    emitDatas(std::get<0>(Method), std::get<1>(Method), std::get<2>(Method),
              std::get<3>(Method));
  }
}

void CJMetadataInfo::emitGCRoots() {
  if (GCRootTable.empty())
    return;

  OS.switchSection(TD[GCRootsTableIdx].TableSection);
  if (TT.isOSBinFormatMachO())
    // 8: align size, 8 bytes
    OS.emitValueToAlignment(8);

  for (const auto GCRoot : GCRootTable) {
    OS.emitValue(GCRoot, FuncPtrSize);
  }
}

void CJMetadataInfo::emitTypeInfo() {
  for (const auto *TI : InternalTITable)
    AP.emitGlobalVariable(TI);
  for (const auto *TIFields : TIFieldsTable)
    AP.emitGlobalVariable(TIFields);
}

void CJMetadataInfo::emitTypeTemplate() {
  for (const auto *TT : TypeTemplateGVTable)
    AP.emitGlobalVariable(TT);
}

void CJMetadataInfo::emitTypeExt() {
  OS.switchSection(TD[TypeExtTableIdx].TableSection);
  for (const auto *typeExt : TypeExtTable) {
    AP.emitGlobalVariable(typeExt);
  }
}

void CJMetadataInfo::emitMTable() {
  if (MTableTable.empty())
    return;

  for (const auto *MTable : MTableTable) {
    AP.emitGlobalVariable(MTable);
  }
}

void CJMetadataInfo::emitGCFlags() {
  OS.switchSection(TD[GCFlagsIdx].TableSection);
  if (DisableGCSupport || EnableSafepointOnly) {
    OS.emitIntValue(0, 1); // 1: width of uint
  } else {
    OS.emitIntValue(1, 1);
  }

  if (DisableGCSupport || EnableBarrierOnly) {
    OS.emitIntValue(0, 1);
  } else {
    OS.emitIntValue(1, 1);
  }

  if (EnableStackGrow) {
    OS.emitIntValue(1, 1);
  } else {
    OS.emitIntValue(0, 1);
  }
  OS.addBlankLine();
}

void CJMetadataInfo::emitReflectInfo() {
  if (!ReflectPkgInfoGV.empty()) {
    for (const auto *const PkgGV : ReflectPkgInfoGV)
      AP.emitGlobalVariable(PkgGV);
  }
  if (!ReflectOtherGV.empty() || !ReflectStrGV.empty()) {
    for (const auto *const OtherGV : ReflectOtherGV)
      AP.emitGlobalVariable(OtherGV);

    for (const auto *const StrGV : ReflectStrGV)
      AP.emitGlobalVariable(StrGV);
  }
}

void CJMetadataInfo::emitReflectGenericTI() {
  for (const auto *RGI : ReflectGeneticTI)
    AP.emitGlobalVariable(RGI);
}

void CJMetadataInfo::emitGCTibTable() {
  if (GCTibTable.empty())
    return;

  for (const auto *GCTib : GCTibTable)
    AP.emitGlobalVariable(GCTib);
}

void CJMetadataInfo::emitSubExpr(StringRef Label, const GlobalValue *GV) {
  MCSymbol *CurSymbol = Context.createTempSymbol(Label);
  OS.emitLabel(CurSymbol);
  const MCExpr *CurRefSym = MCSymbolRefExpr::create(CurSymbol, Context);
  const MCExpr *GVRefSym = getGVRefSymbol(GV);
  const MCExpr *Offset = nullptr;
  if (IsMachO) {
    // The subtraction(second operand) in the MAC-O address cannot be a private
    // symbol. We use `gvSym - curSym` to perform the subtraction operation.
    Offset = MCBinaryExpr::createSub(CurRefSym, GVRefSym, Context);
  } else {
    // Emit generic typeinfo offset: long xxx - generic_ti
    Offset = MCBinaryExpr::createSub(GVRefSym, CurRefSym, Context);
  }
  OS.emitValue(Offset, 4);
}

void CJMetadataInfo::emitInnerTypeExtensions() {
  if (InnerTypeExtensions.empty())
    return;

  for (const auto *GV : InnerTypeExtensions)
    AP.emitGlobalVariable(GV);
}

void CJMetadataInfo::emitOuterTypeExtensions() {
  if (OuterTypeExtensions.empty())
    return;
  OS.switchSection(TD[OuterTypeExtensionsIdx].TableSection);
  for (auto *EED : OuterTypeExtensions)
    emitSubExpr("ext_eds", EED);
}

void CJMetadataInfo::emitStaticGenericTI() {
  if (StaticGenericTI.empty())
    return;
  OS.switchSection(TD[StaticGIIdx].TableSection);
  for (const auto *GenericTI : StaticGenericTI)
    emitSubExpr("generic_ti", GenericTI);
}

void CJMetadataInfo::emitGlobalInitFuncTable() {
  if (InitMethodTable.empty() && MethodTable.empty())
    return;

  OS.switchSection(TD[GlobalInitFuncIdx].TableSection);
  if (TT.isOSBinFormatMachO())
    // 8: align size, 8 bytes
    OS.emitValueToAlignment(8);
  NamedMDNode *PkgInitFuncMD = M->getNamedMetadata("pkg_init_func");
  std::set<std::string> GlobalInitFuncNameSet;
  std::string GlobalInitFuncName;
  if (PkgInitFuncMD != nullptr) {
    for (unsigned I = 0; I < PkgInitFuncMD->getNumOperands(); I++) {
      MDTuple *PkgInitFuncTuple =
          dyn_cast<MDTuple>(PkgInitFuncMD->getOperand(I));
      GlobalInitFuncName =
          dyn_cast<MDString>(PkgInitFuncTuple->getOperand(0))->getString().str();
      if (IsMachO)
        GlobalInitFuncName = "_" + GlobalInitFuncName;
      GlobalInitFuncNameSet.insert(GlobalInitFuncName);
    }
  }
  auto EmitData = [&](const MCSymbol *FuncSym, const MCSymbol *FuncBegin) {
    std::string FuncName = FuncSym->getName().str();
    if (!GlobalInitFuncNameSet.count(FuncName))
      return;

    OS.emitSymbolValue(FuncBegin, FuncPtrSize);
    if (IsMachO) {
      FuncName.push_back('\0');
      OS.emitBytes(FuncName);
      // Pad to 8-byte alignment so runtime can iterate entries with aligned reads,
      // This does not affect backward compatibility with older runtimes that use unaligned reads.
      unsigned EntrySize = FuncPtrSize + FuncName.size();
      unsigned Padding = (8 - (EntrySize % 8)) % 8;
      if (Padding > 0)
        OS.emitZeros(Padding);
    }
  };
  for (auto &Method : InitMethodTable)
    EmitData(std::get<0>(Method), std::get<1>(Method));

  for (auto &Method : MethodTable)
    EmitData(std::get<0>(Method), std::get<1>(Method));

  OS.addBlankLine();
}

void CJMetadataInfo::emitSDKVersion() {
  auto Version = M->getGlobalVariable("cj.sdk.version", true);
  if (Version == nullptr || Version->isDeclaration())
    return;

  OS.switchSection(TD[SDKVersionIdx].TableSection);
  if (TT.isOSBinFormatMachO())
    // 8: align size, 8 bytes
    OS.emitValueToAlignment(8);
  OS.emitValue(getGVRefSymbol(Version), FuncPtrSize);
}

void CJMetadataInfo::emitStackMaps() {
  OS.switchSection(TD[StackMapTableIdx].TableSection);
  SM.emitCangjieStackMaps(OS);
}

uint32_t getStrPoolDictSplitSize(uint32_t TotalSize) {
  // 10: supposed that TotalSize = SplitSize * n and SplitSize = 10 * n.
  uint32_t DesiredSize = std::sqrt(TotalSize / 10) * 10;
  if (DesiredSize <= StrPoolDictSplitMinSize)
    return StrPoolDictSplitMinSize;

  if (DesiredSize >= StrPoolDictSplitMaxSize)
    return StrPoolDictSplitMaxSize;

  // 100: round the result to be a multiple of one hundred.
  return DesiredSize / 100 * 100;
}

void CJMetadataInfo::emitStringPoolDictTable() {
  if (SplitedMangledStr.Items.empty())
    return;

  OS.switchSection(TD[StringPoolDictTableIdx].TableSection);
  // 8: align size, 8 bytes
  OS.emitValueToAlignment(8);
  // Emit stack trace format flag.
  OS.emitInt8(static_cast<int8_t>(StackTraceFormatFlag.getValue()));

  OS.emitLabel(StrPoolDictOffsetsSym);
  // 4: string pool dict total size, 4 bytes
  OS.emitIntValue(SplitedMangledStr.Items.size(), 4);
  uint32_t StrPoolDictSplitSize =
      getStrPoolDictSplitSize(SplitedMangledStr.Items.size());
  uint32_t NumDicts =
      (SplitedMangledStr.Items.size() - 1) / StrPoolDictSplitSize + 1;
  for (uint32_t I = 1; I <= NumDicts; ++I) {
    MCSymbol *StrPoolDictIdxSym =
        Context.getOrCreateSymbol(StrPoolDict + std::to_string(I));
    // 4: string pool dict offset, 4 bytes
    OS.emitValue(MCBinaryExpr::createSub(
                     MCSymbolRefExpr::create(StrPoolDictIdxSym, Context),
                     MCSymbolRefExpr::create(StrPoolDictOffsetsSym, Context),
                     Context),
                 4);
  }

  uint32_t StrPoolDictIdx = 0;
  for (size_t I = 0; I < SplitedMangledStr.Items.size(); ++I) {
    // Split string pool dict by labels.
    if (I % StrPoolDictSplitSize == 0) {
      MCSymbol *StrPoolDictIdxSym = AP.OutContext.getOrCreateSymbol(
          StrPoolDict + std::to_string(++StrPoolDictIdx));
      OS.emitLabel(StrPoolDictIdxSym);
    }
    std::string Str = SplitedMangledStr.Items[I];
    Str.append(",");
    OS.emitBytes(StringRef(Str.c_str(), Str.length()));
  }
  OS.emitBytes(StringRef("\0", 1));
  OS.addBlankLine();
}

void CJMetadataInfo::emitStringPoolTable() {
  if (String2Symbol.empty())
    return;

  // method name items
  OS.switchSection(TD[StringPoolTableIdx].TableSection);
  // 8: align size, 8 bytes
  OS.emitValueToAlignment(8);
  for (auto Itr = String2Symbol.begin(), ItrEnd = String2Symbol.end();
       Itr != ItrEnd; ++Itr) {
    OS.emitLabel(Itr->second);
    // append '\0' then emit stringRef
    OS.emitBytes(StringRef(Itr->first.c_str(), Itr->first.length() + 1));
  }
  OS.addBlankLine();
}

void CJMetadataInfo::emitCJMetadata(Module &M) {
  CJMetadataInfo::M = &M;
  recordMetadata();
  init();
  // RW Section
  emitSDKVersion();
  emitStackMaps();
  emitMethodInfoTable();
  emitGlobalInitFuncTable();
  emitStringPoolDictTable();
  emitStringPoolTable();
  emitGCTibTable();
  emitGCRoots();
  emitTypeTemplate();
  emitTypeInfo();
  emitMTable();
  emitInnerTypeExtensions();
  emitOuterTypeExtensions();
  emitStaticGenericTI();
  emitGCFlags();
  emitReflectInfo();
  emitReflectGenericTI();
  emitTypeExt();
  OS.addBlankLine();
}
} // namespace llvm
