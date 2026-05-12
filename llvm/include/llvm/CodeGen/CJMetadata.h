//===- CodeGen/CJMetadata.h - Self-Defined CJMetadata Section ---*- C++ -*-===//
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

#ifndef LLVM_CODEGEN_CJMETADATA_H
#define LLVM_CODEGEN_CJMETADATA_H
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Triple.h"
#include "llvm/CodeGen/StackMapEncode.h"
#include "llvm/IR/GlobalValue.h"

#include <cstdint>

namespace llvm {
class AsmPrinter;
class Function;
class GCStrategy;
class MCStreamer;
class MCSymbol;
class MCSection;
class MCExpr;
class StackMaps;
class GlobalVariable;
class Module;

enum CJMetadataTable {
  // RW Section
  SDKVersionIdx,
  MethodInfoTableIdx,
  StringPoolDictTableIdx,
  StringPoolTableIdx,
  StackMapTableIdx, // StackMap Table
  GCTibTableIdx,
  GCRootsTableIdx,
  TypeInfoTableIdx,
  TypeTemplateIdx,
  TypeFieldsIdx,
  TypeExtTableIdx,
  StaticGIIdx,
  GlobalInitFuncIdx,
  MTableIdx,
  GCFlagsIdx,
  ReflectPkgInfoIdx,
  ReflectGVInfoIdx,
  ReflectGIIdx,
  InnerTypeExtensionsIdx,
  OuterTypeExtensionsIdx,
  CJMetadataMax
};

struct TableDesc {
  MCSection *TableSection;
};

class CJMetadataInfo {
public:
  CJMetadataInfo(AsmPrinter &AP, StackMaps &SM);
  ~CJMetadataInfo() = default;
  void recordCurrentFunc();
  void emitCJMetadata(Module &M);

private:
  void init();
  void emitDatas(const MCSymbol *FuncName, const MCSymbol *FuncBegin,
                 const MCSymbol *FuncEnd, unsigned FuncNumber);
  void emitSDKVersion();
  void emitMethodInfoTable();
  void emitGlobalInitFuncTable();
  void emitStackTraceInfo(const MCSymbol *FuncSym, const MCSymbol *DescSym);
  void emitEHTableOffset(unsigned FuncNumber);
  void emitStringPoolDictTable();
  void emitStringPoolTable();
  void emitStackMaps();
  void emitGCTibTable();
  void emitGCRoots();

  void emitGVToSection(SmallVectorImpl<const GlobalVariable *> &);
  void emitGVWithSubExpr(SmallVectorImpl<const GlobalVariable *> &, MCSection *,
                         StringRef);

  void emitTypeInfo();
  void emitTypeTemplate();
  void emitTypeExt();
  void emitMTable();
  void emitStaticGenericTI();
  void emitInnerTypeExtensions();
  void emitOuterTypeExtensions();
  void emitGCFlags();
  void emitReflectInfo();
  void emitReflectGenericTI();
  void emitSubExpr(StringRef Label, const GlobalValue *GV);
  const MCExpr *getGVRefSymbol(const GlobalValue *GV);
  const MCExpr *getOrInsertStrPoolOffset(std::string &Str,
                                         const MCSymbol *DescSym,
                                         bool MachONeedNoOffset = false);

  void recordExternalMethod();
  void recordMetadata();
  void recordGlobalVariable(const GlobalVariable *GV);
  void recordKlass(const GlobalVariable *GV);
  bool shouldSkipReflectMetadata() const;

  uint64_t addMangledStr(std::string &Str);
  void splitStr(std::string &Str, std::vector<uint64_t> &CompressedCode);
  Module *M;
  AsmPrinter &AP;
  StackMaps &SM;
  MCStreamer &OS;
  MCContext &Context;
  const Triple &TT;
  bool IsMachO = TT.isOSBinFormatMachO();
  TableDesc TD[CJMetadataMax];
  // <FuncName, FuncBegin symbol, FuncEndSymbol>
  SmallVector<std::tuple<const MCSymbol *, const MCSymbol *, const MCSymbol *,
                         unsigned>,
              100>
      MethodTable;
  SmallVector<std::tuple<const MCSymbol *, const MCSymbol *, const MCSymbol *,
                         unsigned>,
              100>
      InitMethodTable;
  SmallVector<std::tuple<const MCSymbol *, const MCSymbol *, const MCSymbol *,
                         unsigned, const Function *>,
              100>
      ComdatMethodTable;
  SmallVector<const MCExpr *, 100> GCRootTable;
  SmallVector<const GlobalVariable *, 100> InternalTITable;
  SmallVector<const GlobalVariable *, 100> ExternalTITable;
  SmallVector<const GlobalVariable *, 100> TIFieldsTable;
  SmallVector<const GlobalVariable *, 100> MTableTable;
  SmallVector<const GlobalVariable *, 100> TypeExtTable;
  SmallVector<const GlobalVariable *, 100> GCTibTable;
  SmallVector<const GlobalValue *, 100> StaticGenericTI;
  SmallVector<const GlobalVariable *, 50> ReflectGeneticTI;
  SmallVector<const Function *, 100> ExternalMethod;
  SmallVector<const GlobalVariable *, 100> ReflectStrGV;
  SmallVector<const GlobalVariable *, 100> ReflectPkgInfoGV;
  SmallVector<const GlobalVariable *, 100> ReflectOtherGV;
  SmallVector<const GlobalVariable *, 100> OuterTypeExtensions;
  SmallVector<const GlobalVariable *, 16> InnerTypeExtensions;
  SmallVector<const GlobalVariable *, 100> TypeTemplateGVTable;
  std::unordered_map<std::string, MCSymbol *> String2Symbol;
  CompressedInfo::ItemIdxHelper<std::string> SplitedMangledStr;
  MCSymbol *StrPoolDictOffsetsSym;
  uint64_t StrPoolIdx;
  uint32_t FuncPtrSize;
};
} // namespace llvm
#endif // LLVM_CODEGEN_CJMETADATA_H
