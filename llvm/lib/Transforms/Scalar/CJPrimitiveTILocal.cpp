//===- CJPrimitiveTILocal.cpp -----------------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Primitive Cangjie type infos (Unit.ti, Int64.ti, ...) are defined once in
// std.core's native C source (typetemplate.c) and imported as undefined
// symbols by every other DSO. In a shared-library link each pointer to such
// a symbol inside a constant initializer (.cjmetadata type-info fields
// arrays, type-argument lists, ...) becomes a 24-byte ABS64 dynamic
// relocation, plus a .dynsym/.dynstr entry per referenced symbol.
//
// The body of a primitive type info is tiny and all-constant (name string,
// type kind, size, alignment; every pointer field is null), so this pass
// materializes a local copy of it in the referencing module:
//
//   @Int64.ti = external global %TypeInfo
// becomes
//   @Int64.ti = linkonce_odr hidden global %TypeInfo { ... }
//
// References then bind at static link time: metadata pointers become
// R_*_RELATIVE (RELR-packable) and the symbol disappears from .dynsym.
// Correctness relies on the runtime unifying duplicate type infos by name
// at registration (TypeInfoManager), which it has always done for the
// per-DSO copies of generic-instantiation type infos; type identity is
// UUID-based, not pointer-based.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJPrimitiveTILocal.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/Triple.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

#define DEBUG_TYPE "cj-primitive-ti-local"

STATISTIC(NumLocalized, "Number of primitive type infos localized");
STATISTIC(NumRefsConverted, "Number of initializer references converted");

static cl::opt<bool> EnableCJPrimitiveTILocal(
    "cj-primitive-ti-local", cl::init(true), cl::Hidden,
    cl::desc("Materialize hidden local bodies for imported primitive type "
             "infos to eliminate ABS64 dynamic relocations."));

// Localizing a copy costs 96 bytes of body plus its name string per DSO,
// while each removed ABS64 saves 24 bytes plus the .dynsym/.dynstr entry, so
// a symbol referenced fewer than 4 times in a package is cheaper to keep
// importing. At -O1 and above packages compile to a single LLVM module
// (APC splitting only kicks in at -O0 or with explicit --apc), so this
// module-level count is the package-level count for optimized builds.
static cl::opt<unsigned> MinRefsForLocal(
    "cj-primitive-ti-local-min-refs", cl::init(4), cl::Hidden,
    cl::desc("Minimum initializer reference count for localizing a primitive "
             "type info."));

namespace {
// TypeInfo field indices and field count, mirroring the layout used by cjc
// (TypeInfoFields in CGCommonDef.h) and by std.core's typetemplate.c.
constexpr unsigned TI_NAME = 0;
constexpr unsigned TI_TYPE_KIND = 1;
constexpr unsigned TI_SIZE = 4;
constexpr unsigned TI_ALIGN = 7;
constexpr unsigned TI_FIELD_COUNT = 18;
constexpr unsigned TI_BODY_ALIGN = 16;

// Zero means "pointer-sized": filled in from the DataLayout, matching the
// arch #ifdefs in typetemplate.c.
struct PrimitiveTIInfo {
  StringRef name; // Source-level type name, e.g. "Int64" for "Int64.ti".
  int8_t typeKind;
  uint32_t size;
  uint8_t align;
};

// Type-kind values are the UGTypeKind ABI shared with the Cangjie runtime
// (cangjie/Basic/UGTypeKind.h); they must stay in sync with typetemplate.c.
constexpr PrimitiveTIInfo PRIMITIVE_TI_TABLE[] = {
    {"Nothing", 0, 0, 1},  {"Unit", 1, 0, 1},       {"Bool", 2, 1, 1},
    {"Rune", 3, 4, 4},     {"UInt8", 4, 1, 1},      {"UInt16", 5, 2, 2},
    {"UInt32", 6, 4, 4},   {"UInt64", 7, 8, 8},     {"UIntNative", 8, 0, 0},
    {"Int8", 9, 1, 1},     {"Int16", 10, 2, 2},     {"Int32", 11, 4, 4},
    {"Int64", 12, 8, 8},   {"IntNative", 13, 0, 0}, {"Float16", 14, 2, 2},
    {"Float32", 15, 4, 4}, {"Float64", 16, 8, 8},   {"CString", 17, 0, 0},
};
constexpr size_t PRIMITIVE_TI_COUNT =
    sizeof(PRIMITIVE_TI_TABLE) / sizeof(PRIMITIVE_TI_TABLE[0]);

const PrimitiveTIInfo *lookupPrimitiveTI(StringRef gvName) {
  if (!gvName.endswith(".ti"))
    return nullptr;
  StringRef baseName = gvName.drop_back(3); // 3: strlen(".ti")
  for (const PrimitiveTIInfo &info : PRIMITIVE_TI_TABLE)
    if (info.name == baseName)
      return &info;
  return nullptr;
}

// Counts physical slots referencing Target inside a constant tree. Shared
// sub-constants are deliberately counted once per occurrence: each global
// initializer emits its own copy of the tree, so occurrences match the
// emitted relocation count.
unsigned countRefsInConstant(const Constant *C, const GlobalVariable *Target) {
  if (C == Target)
    return 1;
  unsigned count = 0;
  for (const Use &operand : C->operands()) {
    if (operand.get() == Target) {
      ++count;
    } else if (const auto *subC = dyn_cast<Constant>(operand.get())) {
      // Do not descend into other globals: their initializers are visited by
      // the module-level loop, recursing would count them twice.
      if (!isa<GlobalValue>(subC))
        count += countRefsInConstant(subC, Target);
    }
  }
  return count;
}

unsigned countInitializerRefs(const GlobalVariable *Target, const Module &M) {
  unsigned count = 0;
  for (const GlobalVariable &gv : M.globals()) {
    if (!gv.hasInitializer())
      continue;
    count += countRefsInConstant(gv.getInitializer(), Target);
  }
  return count;
}

void materializePrimitiveTI(GlobalVariable &gv, const PrimitiveTIInfo &info,
                            const DataLayout &dl) {
  Module &m = *gv.getParent();
  LLVMContext &ctx = m.getContext();
  auto *structTy = dyn_cast<StructType>(gv.getValueType());
  if (!structTy || structTy->getNumElements() != TI_FIELD_COUNT)
    return;

  uint32_t size = info.size;
  uint8_t align = info.align;
  if (size == 0 && info.name != "Unit" && info.name != "Nothing") {
    size = dl.getPointerSize();
    align = static_cast<uint8_t>(dl.getPointerABIAlignment(0).value());
  }

  Constant *nameStr = ConstantDataArray::getString(ctx, info.name);
  auto *nameGV = new GlobalVariable(m, nameStr->getType(), true,
                                    GlobalValue::PrivateLinkage, nameStr,
                                    info.name + ".name");
  nameGV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);

  auto nullOf = [structTy](unsigned fieldIdx) {
    return Constant::getNullValue(structTy->getElementType(fieldIdx));
  };
  auto intOf = [structTy](unsigned fieldIdx, uint64_t value) {
    return ConstantInt::get(structTy->getElementType(fieldIdx), value);
  };
  auto *i32Ty = Type::getInt32Ty(ctx);
  Constant *zeroIdx[] = {ConstantInt::get(i32Ty, 0),
                         ConstantInt::get(i32Ty, 0)};

  std::vector<Constant *> fields(TI_FIELD_COUNT);
  for (unsigned idx = 0; idx < TI_FIELD_COUNT; ++idx)
    fields[idx] = nullOf(idx);
  fields[TI_NAME] =
      ConstantExpr::getGetElementPtr(nameStr->getType(), nameGV, zeroIdx);
  fields[TI_TYPE_KIND] =
      intOf(TI_TYPE_KIND, static_cast<uint8_t>(info.typeKind));
  fields[TI_SIZE] = intOf(TI_SIZE, size);
  fields[TI_ALIGN] = intOf(TI_ALIGN, align);

  gv.setInitializer(ConstantStruct::get(structTy, fields));
  // Same dedup-and-hide scheme as static-GI type infos: LinkOnceODR merges
  // copies from parallel codegen modules within this DSO, hidden keeps the
  // symbol out of .dynsym and makes in-DSO references non-preemptible.
  gv.setLinkage(GlobalValue::LinkOnceODRLinkage);
  gv.setVisibility(GlobalValue::HiddenVisibility);
  // Match the alignment of the canonical definitions in typetemplate.c.
  gv.setAlignment(llvm::Align(TI_BODY_ALIGN));
}
} // namespace

PreservedAnalyses CJPrimitiveTILocal::run(Module &M,
                                          ModuleAnalysisManager &) const {
  if (!EnableCJPrimitiveTILocal)
    return PreservedAnalyses::all();
  if (!Triple(M.getTargetTriple()).isOSBinFormatELF())
    return PreservedAnalyses::all();

  bool changed = false;
  const DataLayout &dl = M.getDataLayout();
  for (GlobalVariable &gv : M.globals()) {
    if (!gv.isDeclarationForLinker())
      continue;
    const PrimitiveTIInfo *info = lookupPrimitiveTI(gv.getName());
    if (!info)
      continue;
    unsigned refs = countInitializerRefs(&gv, M);
    if (refs < MinRefsForLocal)
      continue;
    materializePrimitiveTI(gv, *info, dl);
    if (!gv.hasInitializer())
      continue;
    ++NumLocalized;
    NumRefsConverted += refs;
    changed = true;
  }
  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
