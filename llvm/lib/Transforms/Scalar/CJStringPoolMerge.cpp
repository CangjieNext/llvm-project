//===- CJStringPoolMerge.cpp - -----------------------------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// CJStringPoolMerge: backend pass that repacks per-literal cjstring byte
// buffers into shared pools with substring sharing.
//
// CodeGen (under full LTO) emits one content-addressed byte buffer per
// cjstring literal; dead literals and their private buffers are dropped
// during LTO linking (well before this pass runs). This pass greedily
// packs the surviving buffers (longest-first, like the legacy CodeGen pool)
// and repoints each surviving cjstring literal to its offset in the new pool.
//
// Pipeline placement: run inside runNewPMPasses (LTOBackend.cpp) BEFORE
// buildLTODefaultPipeline, so cjstring literal wrappers (the
// {poolPtr, offset, length} indirection) are still intact -- after
// SROA/InstCombine inlines the wrappers, direct GEP users appear that the
// byte-pool path cannot repoint. By pre-opt, dead buffers are already gone
// (dropped at link/internalize time), so this pass only sees live ones.
//
// Two merge paths:
//   1. Byte-pool path: buffers reached only via the literal's
//      {poolPtr, offset, length} indirection are packed into one
//      {klassInfo, len, [N x i8]} byte pool; literals are repointed.
//   2. Struct-nesting path (fallback): buffers whose direct handles are NOT
//      byte-pool-repointable (e.g. a GEP with compile-time indices that does
//      not fit the [0,2,off] data shape) become fields of one big struct
//      global; every direct user's pool-pointer ConstantExpr is repointed to
//      the corresponding struct field. AcquireRawData(ptr) returns ptr +
//      sizeof(MArray)=16 (it does NOT read the header), so most direct
//      handles (cast chains, [0,2,off] GEPs) ARE byte-pool-repointable and
//      only go to path 1; path 2 is the fallback for the rest (and does
//      fire on stdlib exception-message buffers in cross-package LTO).
//
// See requirement/ir_analysis/scenario2_stringpool_trim_design.md section 12
// and cangjie-const-pool-pass-design skill.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJStringPoolMerge.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace llvm;

// Attribute markers and naming convention emitted by Cangjie CodeGen
// (cangjie_compiler/src/CodeGen/Utils/Constants.h). The LLVM tree does not
// see those headers, so the strings are duplicated here.
static const char *const CJSTRING_DATA_ATTR = "cjstring_data";
static const char *const CJSTRING_LITERAL_ATTR = "cjstring_literal";
static const char *const CJSTRING_DATA_PREFIX = "$const_cjstring_data.";

namespace {
// Per-literal buffer record collected before repacking.
struct BufInfo {
  GlobalVariable *buf = nullptr; // the $const_cjstring_data.<hash> buffer
  GlobalVariable *lit = nullptr; // the $const_cjstring.<hash> literal
  std::string content;           // raw bytes of [N x i8]
  uint64_t length = 0;
  // Direct ConstantExpr handles that reference this buffer and are used by
  // Instructions (collected via collectDirectHandles). Empty for buffers
  // reached only through the literal indirection.
  SmallVector<Constant *, 4> directHandles;
};
} // namespace

// The buffer struct is { i8* klassInfo, i64 length, [N x i8] data }.
// Extract the data bytes. Returns false if the initializer shape is wrong.
static bool collectBufferContent(BufInfo &info) {
  auto *gv = info.buf;
  if (!gv->hasInitializer())
    return false;
  auto *init = dyn_cast<ConstantStruct>(gv->getInitializer());
  if (!init || init->getNumOperands() != 3)
    return false;
  auto *arr = dyn_cast<ConstantDataArray>(init->getOperand(2));
  if (!arr) {
    // A zero-length data field is emitted as `[0 x i8] zeroinitializer`,
    // which is not a ConstantDataArray. Treat it as empty content so the
    // buffer (e.g. the "" literal) still enters the merge pipeline and gets
    // removed instead of surviving as a standalone {ti, 0, [0 x i8]} global.
    auto *arrTy = dyn_cast<ArrayType>(init->getOperand(2)->getType());
    if (!arrTy || arrTy->getNumElements() != 0)
      return false;
    info.content.clear();
    info.length = 0;
    return true;
  }
  info.content = arr->getAsString().str();
  info.length = info.content.size();
  return true;
}

// True if `lit`'s initializer operand 0 is a cast ConstantExpr chain that
// bottoms out at `buf` (i.e. the literal references this buffer).
static bool refersThroughCast(GlobalVariable *lit, GlobalVariable *buf) {
  if (!lit->hasInitializer())
    return false;
  auto *init = dyn_cast<ConstantStruct>(lit->getInitializer());
  if (!init || init->getNumOperands() < 1)
    return false;
  Constant *C = init->getOperand(0)->stripPointerCasts();
  return C == buf;
}

// Find the cjstring literal global that references `info.buf`.
static bool findOwningLiteral(Module &M, BufInfo &info) {
  auto *buf = info.buf;
  for (GlobalVariable &lit : M.globals()) {
    if (!lit.hasAttribute(CJSTRING_LITERAL_ATTR))
      continue;
    if (!refersThroughCast(&lit, buf))
      continue;
    info.lit = &lit;
    return true;
  }
  return false;
}

// Greedy substring-sharing repack (mirrors CodeGen's rawData.find logic):
// process distinct contents in reverse-sorted order so a shorter string that
// is a substring of a longer one shares the longer's bytes.
static void repackSharedPool(std::vector<BufInfo> &bufs, std::string &merged,
                             std::map<std::string, uint64_t> &offsets) {
  // Collect distinct contents, then process in length-descending order so
  // that a shorter string which is a substring of a longer one shares the
  // longer's already-appended bytes (greedy reverse-append, mirroring
  // CodeGen's rawData.find). Note: std::set<std::string> orders by
  // lexicographic comparison, NOT by length, so rbegin()/rend() would only
  // share by coincidence; we must sort by length explicitly.
  std::set<std::string> distinctSet;
  for (const auto &b : bufs)
    distinctSet.emplace(b.content);
  std::vector<std::string> distinct(distinctSet.begin(), distinctSet.end());
  std::sort(distinct.begin(), distinct.end(),
            [](const std::string &a, const std::string &b) {
              if (a.size() != b.size())
                return a.size() > b.size();
              return a < b;
            });
  merged.clear();
  for (const auto &s : distinct) {
    auto pos = merged.find(s);
    if (pos == std::string::npos) {
      offsets[s] = merged.size();
      merged.append(s);
    } else {
      offsets[s] = pos;
    }
  }
}

// Build the new shared pool global { i8* klassInfo, i64 len, [N x i8] data }
// reusing the klassInfo from the first buffer being merged.
static GlobalVariable *buildSharedPool(Module &M, ArrayRef<BufInfo> bufs,
                                       const std::string &merged,
                                       uint64_t totalLen, unsigned counter) {
  auto &ctx = M.getContext();
  auto *buf0 = bufs.front().buf;
  auto *oldInit = cast<ConstantStruct>(buf0->getInitializer());
  auto *klassInfo = oldInit->getOperand(0);
  auto *strContent = ConstantDataArray::getString(ctx, merged, false);
  auto *i64Ty = Type::getInt64Ty(ctx);
  auto *i8PtrTy = Type::getInt8PtrTy(ctx);
  StructType *poolTy =
      StructType::get(ctx, {i8PtrTy, i64Ty, strContent->getType()});

  // Invariant: the byte-pool repoint path (repointHandleToBytePool,
  // CastChain case) relies on AcquireRawData(buf) == buf + sizeof(MArray)
  // landing on the data field (element 2). The runtime MArray layout
  // (StateWord(8) + length + padding, see cangjie_runtime
  // runtime/src/ObjectModel/MArray.h, GetContentOffset==sizeof(MArray))
  // must therefore place data at the same offset as this struct's element
  // 2. On 64-bit targets both are 16. Verify via DataLayout so a future
  // struct-shape change is caught instead of silently mis-offsetting every
  // repointed direct handle. See cangjie-const-pool-pass-design skill §1.4.
  constexpr uint64_t CJ_MARRAY_DATA_OFFSET = 16;
  const StructLayout *SL = M.getDataLayout().getStructLayout(poolTy);
  if (SL->getElementOffset(2) != CJ_MARRAY_DATA_OFFSET)
    report_fatal_error(
        "CJStringPoolMerge: pool data offset " + Twine(SL->getElementOffset(2)) +
            " != runtime sizeof(MArray)=" + Twine(CJ_MARRAY_DATA_OFFSET) +
            "; byte-pool repoint would mis-offset data",
        false);
  auto *poolInit = ConstantStruct::get(
      poolTy, {klassInfo, ConstantInt::getSigned(i64Ty, totalLen), strContent});
  std::string name =
      (Twine(CJSTRING_DATA_PREFIX) + "merged." + Twine(counter)).str();
  auto *pool = cast<GlobalVariable>(M.getOrInsertGlobal(name, poolTy));
  pool->setInitializer(poolInit);
  pool->setConstant(true);
  pool->addAttribute(CJSTRING_DATA_ATTR);
  pool->setLinkage(GlobalValue::PrivateLinkage);
  return pool;
}

// Repoint each surviving literal to its offset in the shared pool, then erase
// the now-dead per-literal buffer. Verifies each literal's new offset points
// at its own bytes in the merged pool -- a safety net catching any repack
// corruption (e.g. a hash collision that two different contents resolved to
// the same offset, which the content-keyed offsets map should never permit).
static void repointAndErase(GlobalVariable *pool, ArrayRef<BufInfo> bufs,
                            const std::map<std::string, uint64_t> &offsets,
                            const std::string &merged) {
  auto &ctx = pool->getContext();
  auto *i32Ty = Type::getInt32Ty(ctx);
  auto *i8PtrTy = Type::getInt8PtrTy(ctx);
  auto *addr1I8PtrTy = Type::getInt8PtrTy(ctx, 1u);
  auto *poolPtr = ConstantExpr::getBitCast(pool, i8PtrTy);
  auto *poolPtrAddr1 = ConstantExpr::getAddrSpaceCast(poolPtr, addr1I8PtrTy);
  for (const auto &b : bufs) {
    uint64_t off = offsets.at(b.content);
    // Defensive: the literal must read back its own bytes from the pool.
    // Different contents mapping to the same offset would be a bug
    // (hash collision mis-handling, or a repack logic error).
    if (off + b.length > merged.size() ||
        merged.compare(off, b.length, b.content) != 0)
      report_fatal_error("CJStringPoolMerge: literal offset does not "
                         "point at its own content in the merged pool "
                         "(possible hash collision / repack error)",
                         false);
    auto *litTy = cast<StructType>(b.lit->getValueType());
    auto *newInit = ConstantStruct::get(
        litTy, {poolPtrAddr1, ConstantInt::getSigned(i32Ty, off),
                ConstantInt::getSigned(i32Ty, b.length)});
    b.lit->setInitializer(newInit);
    b.buf->removeDeadConstantUsers();
    if (b.buf->use_empty())
      b.buf->eraseFromParent();
  }
}

// Collect the set of ConstantExpr "handles" that reference `buf` and are
// used by Instructions. A handle is a cast/GEP ConstantExpr whose use-chain
// from buf reaches an Instruction. We capture the handle right where it is
// used by an Instruction (the leaf ConstantExpr in the chain), so replacing
// it rewires the instruction's operand.
static bool collectDirectHandles(GlobalVariable *buf,
                                 SmallVectorImpl<Constant *> &handles) {
  SmallVector<Constant *, 16> work;
  SmallPtrSet<Constant *, 32> seen;
  SmallPtrSet<Constant *, 32> reported;
  for (User *U : buf->users()) {
    auto *C = dyn_cast<Constant>(U);
    if (!C)
      return false; // a non-Constant direct user we can't handle
    if (seen.insert(C).second)
      work.push_back(C);
  }
  while (!work.empty()) {
    Constant *C = work.pop_back_val();
    for (User *U : C->users()) {
      if (auto *I = dyn_cast<Instruction>(U)) {
        // C is used directly by an instruction -> C is a handle.
        if (reported.insert(C).second)
          handles.push_back(C);
        continue;
      }
      auto *CE = dyn_cast<ConstantExpr>(U);
      if (!CE ||
          (!CE->isCast() && CE->getOpcode() != Instruction::GetElementPtr))
        continue;
      if (seen.insert(CE).second)
        work.push_back(CE);
    }
  }
  return true;
}

// Build a nested struct global whose fields are the individual buffer structs
// (each {i8*, i64, [N x i8]} verbatim). Returns the global and fills
// `fieldIdx` mapping each buffer to its field index.
static GlobalVariable *
buildNestedStructPool(Module &M, ArrayRef<BufInfo> bufs, unsigned counter,
                      SmallVectorImpl<unsigned> &fieldIdx) {
  auto &ctx = M.getContext();
  std::vector<Type *> elemTys;
  std::vector<Constant *> elemInits;
  for (unsigned i = 0; i < bufs.size(); ++i) {
    auto *oldInit = cast<ConstantStruct>(bufs[i].buf->getInitializer());
    elemTys.push_back(oldInit->getType());
    elemInits.push_back(oldInit);
    fieldIdx.push_back(i);
  }
  auto *nestedTy = StructType::get(ctx, ArrayRef<Type *>(elemTys));
  auto *nestedInit =
      ConstantStruct::get(nestedTy, ArrayRef<Constant *>(elemInits));
  std::string name =
      (Twine(CJSTRING_DATA_PREFIX) + "nested." + Twine(counter)).str();
  auto *pool = cast<GlobalVariable>(M.getOrInsertGlobal(name, nestedTy));
  pool->setInitializer(nestedInit);
  pool->setConstant(true);
  pool->addAttribute(CJSTRING_DATA_ATTR);
  pool->setLinkage(GlobalValue::PrivateLinkage);
  return pool;
}

// ---- Handle decomposition & repointing ----
//
// A "handle" is a ConstantExpr that references a buffer and is used by an
// Instruction (captured at the leaf of the cast/GEP chain from the buffer).
// To repoint a buffer's handles to a new base (byte-pool data or a nested
// struct field), every handle is decomposed once into a structured shape,
// then rebuilt on the new base with the same cast/GEP skeleton.
//
// Handle shapes (mutually exclusive):
//   CastChain        - pure cast chain bottoming out at buf (bitcast /
//                      addrspacecast). Runtime treats the pointer as an
//                      MArray base and adds sizeof(MArray)=16 to reach data
//                      (AcquireRawData does NOT read the header). So the
//                      pointer's meaning is "+16 lands on data"; replacing
//                      buf with (pool_base + baseOffset) preserves that for
//                      every user (AcquireRawData / gcwrite / phi).
//   ConstGepIntoData - gep(buf, 0, 2, constOff) [+ outer casts]: indexes the
//                      data array at a compile-time offset. Repointable to
//                      gep(pool, 0, 2, baseOffset+constOff).
//   ConstGepOther    - a GEP with all-constant indices that bottoms out at
//                      buf but is not a [0,2,off] data index. Repointable by
//                      the nested path (rebuild the GEP on the field base).
//   RuntimeGep       - a GEP with at least one non-constant index. Cannot be
//                      statically rewritten -> the buffer must be skipped.
//   NotRepointable   - any other shape.
//
// The byte-pool path handles CastChain and ConstGepIntoData. The
// struct-nesting path handles CastChain, ConstGepIntoData and
// ConstGepOther. RuntimeGep / NotRepointable force the buffer to be skipped.

namespace {
struct HandleShape {
  enum Kind {
    NotRepointable,
    CastChain,
    ConstGepIntoData,
    ConstGepOther,
    RuntimeGep
  } kind = NotRepointable;
  // Casts wrapping the handle on the outside (outermost first). For
  // CastChain this is the whole chain; for GEP shapes these sit above the
  // GEP.
  SmallVector<Constant *, 4> outerCasts;
  // Casts between the GEP base and buf (innermost first), for GEP shapes.
  SmallVector<Constant *, 4> baseCasts;
  GEPOperator *gep = nullptr; // the GEP for GEP shapes
  uint64_t dataOff = 0;       // data-array offset for ConstGepIntoData
};
} // namespace

// Decompose a handle that (directly or through casts) references `buf`.
// Returns shape.kind == NotRepointable if the handle does not bottom out at
// buf or has an unsupported shape.
static HandleShape decomposeHandle(Constant *handle, GlobalVariable *buf) {
  HandleShape s;
  // Peel outer casts.
  Constant *core = handle;
  while (auto *CE = dyn_cast<ConstantExpr>(core)) {
    if (CE->isCast()) {
      s.outerCasts.push_back(CE);
      core = CE->getOperand(0);
    } else {
      break;
    }
  }
  // No GEP: must be a pure cast chain ending at buf.
  auto *gepCE = dyn_cast<ConstantExpr>(core);
  if (!gepCE || gepCE->getOpcode() != Instruction::GetElementPtr) {
    if (core == buf)
      s.kind = HandleShape::CastChain;
    return s;
  }
  // GEP shape: peel base casts, base must bottom out at buf.
  s.gep = cast<GEPOperator>(gepCE);
  Constant *base = dyn_cast<Constant>(s.gep->getOperand(0));
  if (!base)
    return s;
  while (auto *CE = dyn_cast<ConstantExpr>(base)) {
    if (CE->isCast()) {
      s.baseCasts.push_back(CE);
      base = CE->getOperand(0);
    } else {
      break;
    }
  }
  if (base != buf)
    return s;
  // Classify by indices: must start with 0; field 2 => data array.
  bool allConst = true;
  for (Value *idxV : s.gep->indices())
    if (!isa<ConstantInt>(idxV)) {
      allConst = false;
      break;
    }
  if (!allConst) {
    s.kind = HandleShape::RuntimeGep;
    return s;
  }
  auto idx0 = s.gep->idx_begin();
  if (s.gep->getNumIndices() < 2 ||
      cast<ConstantInt>(*idx0)->getSExtValue() != 0 ||
      cast<ConstantInt>(*(idx0 + 1))->getSExtValue() != 2) {
    s.kind = HandleShape::ConstGepOther;
    return s;
  }
  if (s.gep->getNumIndices() >= 3)
    s.dataOff = cast<ConstantInt>(*(idx0 + 2))->getZExtValue();
  s.kind = HandleShape::ConstGepIntoData;
  return s;
}

// Replay a cast chain (innermost first) on top of `base`, returning the
// resulting Constant. `casts` is ordered outermost-first (as collected by
// decomposeHandle), so iterate in reverse.
static Constant *replayCasts(ArrayRef<Constant *> casts, Constant *base) {
  Constant *result = base;
  for (auto it = casts.rbegin(), end = casts.rend(); it != end; ++it) {
    auto *CE = cast<ConstantExpr>(*it);
    result = ConstantExpr::getCast(CE->getOpcode(), result, CE->getType());
  }
  return result;
}

// Given an old handle that references `oldBuf`, build a new ConstantExpr
// referencing field `fieldIdx` of `nestedPool`, preserving the handle's
// access shape (outer casts +, for GEP shapes, the original GEP indices). The
// new base is gep(nestedPool, 0, fieldIdx); base casts are replayed on it so
// the rebuilt GEP base has the same type/addrspace as the original, then the
// original GEP indices are replayed, then outer casts.
static Constant *repointHandleToNested(const HandleShape &shape,
                                       GlobalVariable *nestedPool,
                                       unsigned fieldIdx) {
  auto &ctx = nestedPool->getContext();
  auto *i32Ty = Type::getInt32Ty(ctx);
  Constant *fieldGEP = ConstantExpr::getGetElementPtr(
      nestedPool->getValueType(), nestedPool,
      ArrayRef<Constant *>{ConstantInt::getSigned(i32Ty, 0),
                           ConstantInt::get(i32Ty, fieldIdx)},
      true);
  Constant *newBase = fieldGEP;
  if (shape.gep) {
    // Replay base casts (innermost first) so newBase matches the original
    // GEP base operand's type/addrspace.
    newBase = replayCasts(shape.baseCasts, newBase);
    // Rebuild the GEP with the original indices on the cast-adjusted base.
    SmallVector<Constant *, 4> idx;
    for (Value *idxV : shape.gep->indices())
      idx.push_back(cast<Constant>(idxV));
    newBase = ConstantExpr::getGetElementPtr(shape.gep->getSourceElementType(),
                                             newBase, idx, true);
  }
  return replayCasts(shape.outerCasts, newBase);
}

// Given an old handle that references `buf`, build a new ConstantExpr into
// `pool`'s data array at `baseOffset` (the buffer's offset within the pool),
// preserving the handle's type/addrspace via outer casts.
//   CastChain        -> gep(i8, bitcast(pool), baseOffset) [+ outer casts]:
//                       runtime does base+16 to reach data, so base+16 lands
//                       on this buffer's data inside the pool (mirrors the
//                       legacy AcquireRawData(pool)+add offset pattern, but
//                       relies on the +16 convention instead of inserting an
//                       add).
//   ConstGepIntoData -> rebuild gep(bufTy, baseCasts(buf-at-baseOffset),
//                       originalIndices) [+ outer casts]: a bufTy-typed base
//                       into the pool is cast up via baseCasts (matching the
//                       original GEP base's addrspace), then the GEP is
//                       rebuilt with the original indices (baseOffset folded
//                       via the base, dataOff via the original index). See
//                       the in-body comment for why the base must be
//                       bufTy-typed and baseCasts replayed (a direct GEP on
//                       pool would land in addrspace 0, mismatching the
//                       original GEP result type).
static Constant *repointHandleToBytePool(const HandleShape &shape,
                                         GlobalVariable *pool,
                                         uint64_t baseOffset) {
  auto &ctx = pool->getContext();
  auto *i8Ty = Type::getInt8Ty(ctx);
  auto *i64Ty = Type::getInt64Ty(ctx);
  Constant *newBase;
  if (shape.kind == HandleShape::CastChain) {
    // bitcast(pool -> i8* in addrspace 0), then gep i8 by baseOffset. i8 GEP
    // with a non-negative index is always legal; the original innermost cast
    // (buf -> i8*/i8 addrspace(1)*) is replaced by this GEP, and the outer
    // casts are replayed on top to preserve the handle's exact type.
    Constant *poolI8 = ConstantExpr::getBitCast(pool, i8Ty->getPointerTo(0));
    newBase = ConstantExpr::getGetElementPtr(
        i8Ty, poolI8, ConstantInt::get(i64Ty, baseOffset), true);
  } else {
    // ConstGepIntoData. The original handle is
    //   outerCasts(gep(bufTy, baseCasts(buf), 0, 2, dataOff, ...))
    // so the GEP result has the type/addrspace of baseCasts(buf) (typically
    // addrspace 1 -- e.g. stdlib memcpy handles are gep(bufTy,
    // addrspacecast(buf), 0, 2, off) with baseCasts=[addrspacecast]).
    // Building the GEP directly on pool (addrspace 0) would yield addrspace
    // 0 and break the using instruction's operand type. Mirror the nested
    // path: build a bufTy-typed base pointing at this buffer's offset within
    // the pool, replay baseCasts on it, then rebuild the GEP with the
    // original indices (baseOffset is folded via the base, dataOff via the
    // original index). The base is bitcast(gep(i8, bitcast(pool), baseOffset)
    // to bufTy*) -- a type pun safe because the original GEP only indexes
    // field 2 (data), and pool's data field sits at the same
    // +sizeof(MArray)=16 offset as buf's (enforced by buildSharedPool).
    Type *bufTy = shape.gep->getSourceElementType();
    Constant *poolI8 = ConstantExpr::getBitCast(pool, i8Ty->getPointerTo(0));
    Constant *bufAt = ConstantExpr::getGetElementPtr(
        i8Ty, poolI8, ConstantInt::get(i64Ty, baseOffset), true);
    newBase = ConstantExpr::getBitCast(bufAt, bufTy->getPointerTo(0));
    // Replay base casts (innermost first) so newBase matches the original
    // GEP base operand's type/addrspace.
    newBase = replayCasts(shape.baseCasts, newBase);
    // Rebuild the GEP with the original indices on the cast-adjusted base.
    SmallVector<Constant *, 4> idx;
    for (Value *idxV : shape.gep->indices())
      idx.push_back(cast<Constant>(idxV));
    newBase = ConstantExpr::getGetElementPtr(bufTy, newBase, idx, true);
  }
  return replayCasts(shape.outerCasts, newBase);
}

// Repoint every direct handle of `b` into `pool`'s data array at the buffer's
// base offset. The caller erases the old buffer afterwards.
static void repointDirectHandlesToPool(GlobalVariable *pool, BufInfo &b,
                                       uint64_t baseOffset) {
  for (Constant *handle : b.directHandles) {
    HandleShape shape = decomposeHandle(handle, b.buf);
    // Only CastChain / ConstGepIntoData reach the byte-pool path (guaranteed
    // by the eligibility check in run()).
    if (shape.kind != HandleShape::CastChain &&
        shape.kind != HandleShape::ConstGepIntoData)
      continue;
    Constant *newH = repointHandleToBytePool(shape, pool, baseOffset);
    handle->replaceAllUsesWith(newH);
  }
}
// Merge buffers whose direct handles are not byte-pool-repointable but are
// all nested-repointable, into one nested-struct pool. A buffer with any
// un-repointable handle is skipped whole (a partial rewrite would leave the
// old buffer referenced -> duplicate).
static void mergeDirectUserBuffers(Module &M, std::vector<BufInfo> &bufs,
                                   unsigned &counter) {
  // Eligible: buffers whose direct handles are all repointable by the
  // nested path (CastChain / ConstGepIntoData / ConstGepOther). Handles
  // already collected in BufInfo.directHandles by run(); a buffer with any
  // RuntimeGep / NotRepointable handle is skipped whole (a partial rewrite
  // would leave the old buffer referenced -> duplicate).
  SmallVector<BufInfo *, 8> eligible;
  for (auto &b : bufs) {
    if (b.directHandles.empty())
      continue;
    bool ok = true;
    for (Constant *h : b.directHandles) {
      HandleShape s = decomposeHandle(h, b.buf);
      if (s.kind == HandleShape::RuntimeGep ||
          s.kind == HandleShape::NotRepointable) {
        ok = false;
        break;
      }
    }
    if (ok)
      eligible.push_back(&b);
  }
  if (eligible.size() < 2)
    return;
  // Build the nested struct pool.
  std::vector<BufInfo> eligibleBufs;
  for (BufInfo *b : eligible)
    eligibleBufs.push_back(*b);
  SmallVector<unsigned, 8> fieldIdx;
  GlobalVariable *nested =
      buildNestedStructPool(M, eligibleBufs, counter++, fieldIdx);
  // Path 2 (struct-nesting): fallback for buffers whose direct handles are
  // not byte-pool-repointable (e.g. ConstGepOther). This path DOES trigger
  // in practice -- a full-LTO build of stdlib + user program produces a
  // surviving $const_cjstring_data.nested.<n> pool (e.g. stdlib exception
  // message buffers reached via non-[0,2,off] constant GEPs). Log loudly so
  // a CI/regression run that hits it is not silently trusted -- the
  // repointed handles should be manually verified when this fires on new IR.
  errs() << "CJStringPoolMerge: path 2 (struct-nesting) merged "
         << eligible.size() << " buffers into " << nested->getName()
         << " (fallback path -- verify the repointed handles)\n";
  // Repoint every handle, then erase the old buffers.
  for (unsigned i = 0; i < eligible.size(); ++i) {
    BufInfo *b = eligible[i];
    for (Constant *h : b->directHandles) {
      HandleShape s = decomposeHandle(h, b->buf);
      Constant *newH = repointHandleToNested(s, nested, i);
      h->replaceAllUsesWith(newH);
    }
    b->buf->removeDeadConstantUsers();
    if (b->buf->use_empty())
      b->buf->eraseFromParent();
  }
}

PreservedAnalyses CJStringPoolMerge::run(Module &M,
                                         ModuleAnalysisManager &) const {
  // Collect every cjstring_data buffer. Direct handles are collected once
  // per buffer into BufInfo.directHandles. Each handle is decomposed once;
  // the shape decides the buffer's fate:
  //   - all handles CastChain/ConstGepIntoData        -> byte pool (path 1)
  //   - all handles CastChain/ConstGepIntoData/
  //     ConstGepOther (none RuntimeGep/NotRepointable) -> nested (path 2)
  //   - any RuntimeGep/NotRepointable                 -> skip whole buffer
  // Buffers with no direct handles go straight to the byte pool (path 1).
  std::vector<BufInfo> bytePackable;
  std::vector<BufInfo> directUser; // fallback for struct-nesting
  for (GlobalVariable &gv : M.globals()) {
    if (!gv.hasAttribute(CJSTRING_DATA_ATTR))
      continue;
    BufInfo info;
    info.buf = &gv;
    if (!collectBufferContent(info))
      continue;
    findOwningLiteral(M, info);
    SmallVector<Constant *, 4> handles;
    bool hasLiveInstrUser = !collectDirectHandles(&gv, handles);
    if (hasLiveInstrUser) {
      // A live Instruction uses buf directly (not through a ConstantExpr we
      // can repoint). We cannot rewrite that user, so the buffer must
      // survive as-is. Skip it entirely: do NOT merge its content into the
      // pool -- otherwise the literal would be repointed to the pool while
      // the live instruction keeps the old buffer alive, duplicating the
      // content in the binary (the "duplicate, not no-op" failure mode,
      // see cangjie-const-pool-pass-design skill §2).
      continue;
    }
    if (handles.empty()) {
      // Pure indirection: buf is reached only via its literal's
      // {poolPtr, offset, length} wrapper, with no direct instruction
      // handle. Safe to merge + repoint literal + erase buffer.
      if (info.lit)
        bytePackable.push_back(std::move(info));
      continue;
    }
    // Decompose every handle once and classify the buffer.
    bool byteOk = true;   // all handles byte-pool-repointable
    bool nestedOk = true; // all handles nested-repointable
    for (Constant *h : handles) {
      HandleShape s = decomposeHandle(h, &gv);
      switch (s.kind) {
      case HandleShape::CastChain:
      case HandleShape::ConstGepIntoData:
        break; // ok for both paths
      case HandleShape::ConstGepOther:
        byteOk = false; // only nested can rebuild arbitrary const GEPs
        break;
      case HandleShape::RuntimeGep:
      case HandleShape::NotRepointable:
        byteOk = false;
        nestedOk = false;
        break;
      }
      if (!byteOk && !nestedOk)
        break;
    }
    info.directHandles = std::move(handles);
    if (byteOk)
      bytePackable.push_back(std::move(info));
    else if (nestedOk)
      directUser.push_back(std::move(info));
    // else: skip (mixed un-repointable) -- buffer survives as-is
  }

  bool changed = false;
  unsigned counter = 0;

  // Path 1: byte-pool merge. Buffers reached via literal indirection AND
  // direct-user buffers whose handles are byte-pool-repointable both merge
  // into one shared byte pool sharing a single ti.
  if (bytePackable.size() >= 2) {
    std::string merged;
    std::map<std::string, uint64_t> offsets;
    repackSharedPool(bytePackable, merged, offsets);
    if (!merged.empty()) {
      GlobalVariable *pool =
          buildSharedPool(M, bytePackable, merged, merged.size(), counter++);
      // Repoint literal wrappers and erase the old buffers (works for both
      // pure-indirection buffers and direct-user buffers that also have an
      // owning literal).
      std::vector<BufInfo> litOnly;
      for (auto &b : bytePackable)
        if (b.lit)
          litOnly.push_back(b);
      if (!litOnly.empty())
        repointAndErase(pool, litOnly, offsets, merged);
      // Repoint direct handles into the pool's data array, then erase the
      // now-dead direct-user buffers that had no owning literal.
      for (auto &b : bytePackable) {
        if (b.directHandles.empty())
          continue;
        repointDirectHandlesToPool(pool, b, offsets.at(b.content));
        b.buf->removeDeadConstantUsers();
        if (b.buf->use_empty())
          b.buf->eraseFromParent();
      }
      changed = true;
    }
  }

  // Path 2: struct-nesting merge for buffers whose direct handles are not
  // byte-pool-repointable but are nested-repointable.
  if (directUser.size() >= 2) {
    mergeDirectUserBuffers(M, directUser, counter);
    changed = true;
  }

  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
