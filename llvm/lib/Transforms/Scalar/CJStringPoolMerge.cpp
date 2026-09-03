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
// byte-pool path cannot repoint. Note the consequence: literals that only
// become dead during the LTO opt pipeline itself (e.g. dead after inlining)
// are still baked into the merged pool; only literals already dropped by
// link time (observed gone before the 0.preopt hook) are excluded.
//
// Safety invariant: a buffer that has referring literals is only treated
// as a per-literal buffer when one of them is a genuine wrapper
// {cast(buf), 0, N} (N == the buffer's data length). A legacy per-package
// pool has literals with offset != 0 / len != N, so it is never eligible;
// this keeps mixed links (e.g. a stale pooled bitcode archive in a
// full-LTO link) correct. A buffer with no referring literal at all is an
// orphan whose wrapper was folded away by the per-module opt; its direct
// handles are repointed by byte offset, which is content-safe.
//
// 64-bit only: the {i8*, i64, [N x i8]} buffer shape mirrors the runtime
// MArray layout whose data offset is sizeof(MArray) == 16 on 64-bit. On
// 32-bit targets sizeof(MArray) == 8 and this struct shape cannot model it,
// so the pass skips non-64-bit modules entirely (CodeGen also only emits
// per-literal buffers on 64-bit targets).
//
// Two merge paths:
//   1. Byte-pool path: buffers reached only via the literal's
//      {poolPtr, offset, length} indirection, direct handles that are pure
//      data pointers ([0,2,off] constant GEPs), and CastChain handles whose
//      users are all gcwrite String materializations are packed into one
//      {klassInfo, len, [N x i8]} byte pool; literals are repointed, and
//      for the gcwrite case the handle becomes a cast chain of the pool
//      base while the paired (off:len) store is rewritten from off=0 to the
//      pool offset -- restoring the legacy {pool, offset, length} shape
//      whose header is the pool's own (valid) one.
//   2. Struct-nesting path: buffers with any other CastChain handle (an
//      MArray base pointer whose header must stay valid -- the stdlib's
//      checked arrayGet loads the length field at base+8, so a mid-pool
//      base would leave a garbage header; observed as spurious
//      IndexOutOfBoundsException in String.toRuneArray) or non-[0,2,off]
//      constant GEPs become fields of one big struct global; every direct
//      user's pool-pointer ConstantExpr is repointed to the corresponding
//      struct field, and the owning literal (if any) is repointed to
//      {field, 0, len}. Each field is a verbatim copy of the old buffer,
//      so its header stays valid.
//
// See requirement/ir_analysis/scenario2_stringpool_trim_design.md section 12
// and cangjie-const-pool-pass-design skill.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJStringPoolMerge.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "cj-string-pool-merge"

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
  // For CastChain handles whose users are all gcwrite String
  // materializations: the paired (off:len) packed-i64 stores, collected by
  // collectOffsetFixups during classification. At repoint time the low 32
  // bits (the offset, 0 after the per-module fold) are rewritten to the
  // buffer's offset in the merged pool.
  SmallVector<StoreInst *, 4> offsetStores;
  // Named String constants referencing the buffer through a CastChain
  // handle (e.g. stdlib's @_CNbb... globals): the {handle, 0, len} record
  // initializers. Repointed to {poolBase, poolOff, len} at repoint time.
  SmallVector<ConstantStruct *, 2> stringRecords;
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

// True if `lit` is a genuine per-literal wrapper of `buf`, i.e. its
// initializer is exactly {cast-chain(buf), i32 0, i32 dataLen} where
// dataLen is the buffer's data-array length. This is the structural
// invariant that distinguishes a deferred per-literal buffer from a legacy
// per-package pool: legacy literals point into the pool with offset != 0
// and/or length != pool data length. Without this check a legacy pool in
// the same module (e.g. a stale pooled bitcode archive in a full-LTO link)
// would be mistaken for one giant literal and its first literal would be
// repointed to {newPool, 0, poolLen} -- silently corrupting the string.
static bool isPerLiteralWrapper(GlobalVariable *lit, GlobalVariable *buf,
                                uint64_t dataLen) {
  if (!refersThroughCast(lit, buf))
    return false;
  auto *init = cast<ConstantStruct>(lit->getInitializer());
  if (init->getNumOperands() != 3)
    return false;
  auto *off = dyn_cast<ConstantInt>(init->getOperand(1));
  auto *len = dyn_cast<ConstantInt>(init->getOperand(2));
  return off && len && off->isZero() && len->getZExtValue() == dataLen;
}

// Build a buf -> referring-literals map in one scan over the module's
// globals (O(#globals) total, instead of scanning all globals per buffer).
// Every literal carrying CJSTRING_LITERAL_ATTR whose initializer operand 0
// bottoms out at the buffer is recorded; the caller decides whether any of
// them is a genuine per-literal wrapper (see isPerLiteralWrapper).
static std::unordered_map<GlobalVariable *, SmallVector<GlobalVariable *, 2>>
buildOwnerMap(Module &M) {
  std::unordered_map<GlobalVariable *, SmallVector<GlobalVariable *, 2>>
      ownerOf;
  for (GlobalVariable &lit : M.globals()) {
    if (!lit.hasAttribute(CJSTRING_LITERAL_ATTR) || !lit.hasInitializer())
      continue;
    auto *init = dyn_cast<ConstantStruct>(lit.getInitializer());
    if (!init || init->getNumOperands() != 3)
      continue;
    auto *buf = dyn_cast<GlobalVariable>(
        init->getOperand(0)->stripPointerCasts());
    if (!buf)
      continue;
    ownerOf[buf].push_back(&lit);
  }
  return ownerOf;
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
  // (StateWord + length + padding, see cangjie_runtime
  // runtime/src/ObjectModel/MArray.h, GetContentOffset==sizeof(MArray))
  // must therefore place data at the same offset as this struct's element
  // 2. sizeof(MArray) is 16 on 64-bit and 8 on 32-bit; the {i8*, i64, ...}
  // buffer shape can only model it on 64-bit, so run() skips non-64-bit
  // modules entirely and only the 64-bit value can reach here. Keep the
  // DataLayout verification so a future struct-shape change is caught
  // instead of silently mis-offsetting every repointed direct handle.
  // See cangjie-const-pool-pass-design skill §1.4.
  const uint64_t marrayDataOffset = 2 * M.getDataLayout().getPointerSize();
  const StructLayout *SL = M.getDataLayout().getStructLayout(poolTy);
  if (SL->getElementOffset(2) != marrayDataOffset)
    report_fatal_error(
        "CJStringPoolMerge: pool data offset " + Twine(SL->getElementOffset(2)) +
            " != runtime sizeof(MArray)=" + Twine(marrayDataOffset) +
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

// One step of a flattened GEP chain: the source element type and the raw
// index operands.
struct GepStep {
  Type *srcTy;
  SmallVector<Value *, 4> indices;
};

// Decompose `v` (stripping casts) into its root value and the sequence of
// GEP steps from the root outward.
static Value *flattenAddr(Value *v, SmallVectorImpl<GepStep> &steps) {
  while (true) {
    if (auto *g = dyn_cast<GEPOperator>(v)) {
      GepStep st;
      st.srcTy = g->getSourceElementType();
      for (Value *i : g->indices())
        st.indices.push_back(i);
      steps.push_back(std::move(st));
      v = g->getPointerOperand();
      continue;
    }
    auto *op = dyn_cast<Operator>(v);
    if (op && Instruction::isCast(op->getOpcode())) {
      v = op->getOperand(0);
      continue;
    }
    break;
  }
  std::reverse(steps.begin(), steps.end());
  return v;
}

// Byte delta between last indices `lastA` and `lastB` into the same
// container (the GEP source type walked through `prefix` indices). `ok` is
// set on success.
static int64_t lastIndexByteDelta(const DataLayout &DL, Type *srcTy,
                                  ArrayRef<Value *> prefix, Value *lastA,
                                  Value *lastB, bool &ok) {
  ok = false;
  auto *ca = dyn_cast<ConstantInt>(lastA);
  auto *cb = dyn_cast<ConstantInt>(lastB);
  if (!ca || !cb)
    return 0;
  // The first index walks through srcTy itself; subsequent indices walk
  // into the pointee.
  Type *container = srcTy;
  for (size_t k = 1; k < prefix.size(); ++k) {
    if (auto *st = dyn_cast<StructType>(container)) {
      auto *ci = dyn_cast<ConstantInt>(prefix[k]);
      if (!ci)
        return 0;
      container = st->getElementType(ci->getZExtValue());
    } else if (auto *at = dyn_cast<ArrayType>(container)) {
      container = at->getElementType(); // array index value is irrelevant
    } else {
      return 0;
    }
  }
  ok = true;
  if (auto *st = dyn_cast<StructType>(container)) {
    const StructLayout *sl = DL.getStructLayout(st);
    return (int64_t)sl->getElementOffset(cb->getZExtValue()) -
           (int64_t)sl->getElementOffset(ca->getZExtValue());
  }
  return (cb->getSExtValue() - ca->getSExtValue()) *
         (int64_t)DL.getTypeAllocSize(container);
}

// True if `cand` denotes the address exactly 8 bytes after `slot`. Both
// addresses may contain variable indices; their GEP/cast chains must be
// identical except for a constant +8 byte difference at the end (either a
// trailing gep(i8, slot, 8) step, or a last constant index 8 bytes larger).
static bool addrMatchesSlotPlus8(const DataLayout &DL, Value *slot,
                                 Value *cand) {
  SmallVector<GepStep, 4> as, bs;
  if (flattenAddr(slot, as) != flattenAddr(cand, bs))
    return false;
  if (bs.size() < as.size() || bs.size() > as.size() + 1)
    return false;
  // Common prefix steps must be identical (except, for equal-length chains,
  // the very last index).
  for (size_t i = 0; i < as.size(); ++i) {
    if (as[i].srcTy != bs[i].srcTy ||
        as[i].indices.size() != bs[i].indices.size())
      return false;
    size_t last = as[i].indices.size() - 1;
    size_t cmpN = (i + 1 == as.size() && bs.size() == as.size()) ? last
                                                                 : last + 1;
    for (size_t j = 0; j < cmpN; ++j)
      if (as[i].indices[j] != bs[i].indices[j])
        return false;
  }
  if (bs.size() == as.size() + 1) {
    // cand = gep(i8, <slot address>, 8)
    const GepStep &ex = bs.back();
    auto *ci =
        ex.indices.size() == 1 ? dyn_cast<ConstantInt>(ex.indices[0]) : nullptr;
    return ex.srcTy->isIntegerTy(8) && ci && ci->getSExtValue() == 8;
  }
  if (as.empty())
    return false;
  const GepStep &la = as.back(), &lb = bs.back();
  bool ok = false;
  int64_t delta = lastIndexByteDelta(DL, la.srcTy,
                                     ArrayRef<Value *>(la.indices).drop_back(),
                                     la.indices.back(), lb.indices.back(), ok);
  return ok && delta == 8;
}

// Find the packed (off:len) i64 store paired with a String.myData slot: a
// store of the constant (length << 32) | 0, located exactly 8 bytes after
// slotPtr within the same base object. The full value must equal
// length<<32 (not just low 32 bits zero): a stack/heap slot can be reused
// for several Strings over its lifetime, and the length disambiguates which
// materialization the store belongs to. Returns non-null only when exactly
// one store matches (an ambiguous pairing would corrupt unrelated data).
static StoreInst *findPackedStore(const DataLayout &DL, Value *slotPtr,
                                  Function *F, uint64_t length) {
  StoreInst *found = nullptr;
  uint64_t want = length << 32;
  for (Instruction &I : instructions(F)) {
    auto *SI = dyn_cast<StoreInst>(&I);
    if (!SI)
      continue;
    auto *ci = dyn_cast<ConstantInt>(SI->getValueOperand());
    if (!ci || ci->getValue().getZExtValue() != want)
      continue;
    if (!addrMatchesSlotPlus8(DL, slotPtr, SI->getPointerOperand()))
      continue;
    if (found)
      return nullptr; // ambiguous: more than one candidate
    found = SI;
  }
  return found;
}

// True for the runtime raw-data accessors: they use the pointer transiently
// via ptr+sizeof(MArray) (no header read, the pointer never persists as a
// String.myData), so repointing them into the middle of the byte pool keeps
// the "+16 lands on data" convention intact.
static bool isRawDataAccessor(CallBase *CB) {
  Function *callee = CB->getCalledFunction();
  if (!callee)
    return false;
  StringRef n = callee->getName();
  return n == "CJ_MCC_AcquireRawData" || n == "CJ_MCC_ReleaseRawData";
}

// True if every use of `c` bottoms out at global initializers through
// constant aggregates (structs/arrays/vectors) only -- such constants can be
// rebuilt with a rewritten operand. A ConstantExpr user (code-reachable) or
// any non-constant user fails the check.
static bool aggBottomsAtGlobals(Constant *c) {
  for (User *u : c->users()) {
    if (isa<GlobalVariable>(u))
      continue;
    auto *cc = dyn_cast<Constant>(u);
    if (!cc || isa<ConstantExpr>(cc))
      return false;
    if (!aggBottomsAtGlobals(cc))
      return false;
  }
  return true;
}

// A CastChain handle may be repointed into the byte pool only when every one
// of its users fits one of these shapes (anything else -- a phi, a select,
// an unknown call -- would observe a changed pointer and forces the nested
// path):
//   - gcwrite String materialization: paired (off:len) store found and
//     recorded for offset fixup; the handle becomes the pool base.
//   - plain-store String materialization (no barrier): same pairing, with
//     the store's own pointer operand as the slot.
//   - AcquireRawData/ReleaseRawData call: transient data-only use; the
//     handle becomes the mid-pool base (pool+baseOffset, +16 lands on data).
//   - a {handle, 0, len} String record constant bottoming out at globals
//     (e.g. @_CNbb... named constants): repointed to {pool, poolOff, len}.
//   - the owning literal wrapper's initializer (handled by the literal
//     repoint, which runs before handle repointing).
// Returns false if any user does not fit.
static bool collectOffsetFixups(const DataLayout &DL, Constant *handle,
                                BufInfo &info,
                                SmallPtrSetImpl<StoreInst *> &claimed) {
  SmallPtrSet<StoreInst *, 4> seen;
  auto recordStore = [&](StoreInst *SI) -> bool {
    if (!seen.insert(SI).second)
      return true; // same store via another slot of this string: idempotent
    if (claimed.contains(SI))
      return false; // another buffer already claims this store: ambiguous
    info.offsetStores.push_back(SI);
    return true;
  };
  for (User *U : handle->users()) {
    if (auto *CB = dyn_cast<CallBase>(U)) {
      if (CB->arg_size() < 2 || CB->getArgOperand(0) != handle)
        return false;
      if (isRawDataAccessor(CB))
        continue;
      Function *callee = CB->getCalledFunction();
      if (!callee || !callee->getName().startswith("llvm.cj.gcwrite"))
        return false;
      // The slot address is arg2 for gcwrite.ref(value, arr, slot) and arg1
      // for gcwrite.static.ref(value, slot); try every non-handle argument.
      StoreInst *SI = nullptr;
      for (unsigned a = 1; a < CB->arg_size() && !SI; ++a)
        SI = findPackedStore(DL, CB->getArgOperand(a), CB->getFunction(),
                             info.length);
      if (!SI || !recordStore(SI))
        return false;
      continue;
    }
    if (auto *SI = dyn_cast<StoreInst>(U)) {
      if (SI->getValueOperand() != handle)
        return false;
      StoreInst *packed = findPackedStore(DL, SI->getPointerOperand(),
                                          SI->getFunction(), info.length);
      if (!packed || !recordStore(packed))
        return false;
      continue;
    }
    if (auto *CS = dyn_cast<ConstantStruct>(U)) {
      if (info.lit && CS == info.lit->getInitializer())
        continue;
      if (CS->getNumOperands() != 3 || CS->getOperand(0) != handle)
        return false;
      auto *offC = dyn_cast<ConstantInt>(CS->getOperand(1));
      auto *lenC = dyn_cast<ConstantInt>(CS->getOperand(2));
      if (!offC || !offC->isZero() || !lenC ||
          lenC->getValue().getZExtValue() != info.length)
        return false;
      if (!aggBottomsAtGlobals(CS))
        return false;
      info.stringRecords.push_back(CS);
      continue;
    }
    return false;
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
//                      addrspacecast). The pointer is used as an MArray
//                      base (e.g. stored into String.myData): the runtime
//                      reaches data via base+sizeof(MArray)=16, and the
//                      header MUST stay valid -- checked arrayGet in the
//                      stdlib (e.g. String.toRuneArray's short-string
//                      path) loads the length field at base+8 for its
//                      bounds check. Repointing such a handle into the
//                      middle of a byte pool would leave a garbage header
//                      (observed: spurious IndexOutOfBoundsException when
//                      the 8 preceding bytes read as a small/negative
//                      i64). Instead, when every user is a gcwrite String
//                      materialization, the handle is repointed to the
//                      pool base and the paired (off:len) store's offset
//                      field is rewritten to the pool offset, restoring
//                      the legacy {pool, offset, length} shape (header =
//                      the pool's own, valid). CastChain handles with any
//                      other user go to the nested path (per-field valid
//                      headers).
//   ConstGepIntoData - gep(buf, 0, 2, constOff) [+ outer casts]: indexes the
//                      data array at a compile-time offset. The result is a
//                      raw data pointer, never an MArray base, so no header
//                      is ever read through it. Repointable to
//                      gep(pool, 0, 2, baseOffset+constOff).
//   ConstGepOther    - a GEP with all-constant indices that bottoms out at
//                      buf but is not a [0,2,off] data index. Repointable by
//                      the nested path (rebuild the GEP on the field base).
//   RuntimeGep       - a GEP with at least one non-constant index. Cannot be
//                      statically rewritten -> the buffer must be skipped.
//   NotRepointable   - any other shape.
//
// The byte-pool path handles ConstGepIntoData and gcwrite-materialized
// CastChain. The struct-nesting path handles CastChain, ConstGepIntoData
// and ConstGepOther. RuntimeGep / NotRepointable force the buffer to be
// skipped.

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
// Only ConstGepIntoData handles reach the byte-pool path (guaranteed by the
// eligibility check in run()): the original handle is
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
static Constant *repointHandleToBytePool(const HandleShape &shape,
                                         GlobalVariable *pool,
                                         uint64_t baseOffset) {
  auto &ctx = pool->getContext();
  auto *i8Ty = Type::getInt8Ty(ctx);
  auto *i64Ty = Type::getInt64Ty(ctx);
  if (shape.kind != HandleShape::ConstGepIntoData)
    report_fatal_error("CJStringPoolMerge: only ConstGepIntoData handles may "
                       "be repointed into the byte pool",
                       false);
  Type *bufTy = shape.gep->getSourceElementType();
  Constant *poolI8 = ConstantExpr::getBitCast(pool, i8Ty->getPointerTo(0));
  Constant *bufAt = ConstantExpr::getGetElementPtr(
      i8Ty, poolI8, ConstantInt::get(i64Ty, baseOffset), true);
  Constant *newBase = ConstantExpr::getBitCast(bufAt, bufTy->getPointerTo(0));
  // Replay base casts (innermost first) so newBase matches the original
  // GEP base operand's type/addrspace.
  newBase = replayCasts(shape.baseCasts, newBase);
  // Rebuild the GEP with the original indices on the cast-adjusted base.
  SmallVector<Constant *, 4> idx;
  for (Value *idxV : shape.gep->indices())
    idx.push_back(cast<Constant>(idxV));
  newBase = ConstantExpr::getGetElementPtr(bufTy, newBase, idx, true);
  return replayCasts(shape.outerCasts, newBase);
}

// Collect the root globals whose initializers (transitively) contain `c`.
// Callers must have validated with aggBottomsAtGlobals first.
static void collectRootGlobals(Constant *c,
                               SmallVectorImpl<GlobalVariable *> &roots) {
  SmallVector<Constant *, 8> work{c};
  SmallPtrSet<Constant *, 8> seen{c};
  while (!work.empty()) {
    Constant *cur = work.pop_back_val();
    for (User *u : cur->users()) {
      if (auto *gv = dyn_cast<GlobalVariable>(u)) {
        roots.push_back(gv);
        continue;
      }
      auto *cc = cast<Constant>(u);
      if (seen.insert(cc).second)
        work.push_back(cc);
    }
  }
}

// Rebuild the aggregate `c`, replacing the sub-constant `oldCS` with
// `newCS`. Returns `c` unchanged when `oldCS` does not occur inside.
static Constant *rewriteStringRecord(Constant *c, Constant *oldCS,
                                     Constant *newCS) {
  if (c == oldCS)
    return newCS;
  SmallVector<Constant *, 8> ops;
  bool changedOp = false;
  for (Use &op : c->operands()) {
    auto *oc = cast<Constant>(op.get());
    Constant *nc = rewriteStringRecord(oc, oldCS, newCS);
    ops.push_back(nc);
    changedOp |= nc != oc;
  }
  if (!changedOp)
    return c;
  if (auto *cs = dyn_cast<ConstantStruct>(c))
    return ConstantStruct::get(cs->getType(), ops);
  if (auto *ca = dyn_cast<ConstantArray>(c))
    return ConstantArray::get(ca->getType(), ops);
  return ConstantVector::get(ops);
}

// Repoint every direct handle of `b` into `pool` at the buffer's base
// offset, then erase the old buffer. CastChain handles are rewritten per
// user: gcwrite/plain-store String materializations get the pool base (its
// header is valid) plus a fixup of the paired (off:len) store from off=0 to
// off=baseOffset; raw-data accessor calls get the mid-pool base
// (pool+baseOffset, preserving the "+16 lands on data" convention);
// {handle, 0, len} String record constants are rebuilt as
// {poolBase, baseOffset, len}. ConstGepIntoData handles become raw data
// pointers into the pool (repointHandleToBytePool).
static void repointDirectHandlesToPool(GlobalVariable *pool, BufInfo &b,
                                       uint64_t baseOffset) {
  auto &ctx = pool->getContext();
  for (Constant *handle : b.directHandles) {
    HandleShape shape = decomposeHandle(handle, b.buf);
    if (shape.kind == HandleShape::CastChain) {
      // Replay the cast chain on the pool base. The innermost cast
      // originally took buf's struct type; bitcast/addrspacecast from the
      // pool's struct pointer is equally legal.
      Constant *baseH = replayCasts(shape.outerCasts, pool);
      Constant *midH = nullptr;
      auto getMidH = [&]() -> Constant * {
        if (midH)
          return midH;
        auto *i8Ty = Type::getInt8Ty(ctx);
        auto *i64Ty = Type::getInt64Ty(ctx);
        Constant *poolI8 =
            ConstantExpr::getBitCast(pool, i8Ty->getPointerTo(0));
        Constant *mid = ConstantExpr::getGetElementPtr(
            i8Ty, poolI8, ConstantInt::get(i64Ty, baseOffset), true);
        ArrayRef<Constant *> casts = shape.outerCasts;
        if (!casts.empty()) {
          // Replace the innermost cast (buf -> i8*): the mid-pool GEP
          // already yields i8*.
          auto *inner = cast<ConstantExpr>(casts.back());
          casts = casts.drop_back();
          if (inner->getType() != mid->getType())
            mid = ConstantExpr::getCast(inner->getOpcode(), mid,
                                        inner->getType());
        }
        midH = replayCasts(casts, mid);
        return midH;
      };
      // Rebuild {handle, 0, len} String record constants as
      // {poolBase, baseOffset, len} (must happen before touching the
      // instruction uses: constants cannot be mutated in place).
      for (ConstantStruct *CS : b.stringRecords) {
        if (CS->getOperand(0) != handle)
          continue;
        Constant *newCS = ConstantStruct::get(
            CS->getType(),
            {baseH, ConstantInt::get(Type::getInt32Ty(ctx), baseOffset),
             CS->getOperand(2)});
        SmallVector<GlobalVariable *, 4> roots;
        collectRootGlobals(CS, roots);
        for (GlobalVariable *gv : roots)
          gv->setInitializer(
              rewriteStringRecord(gv->getInitializer(), CS, newCS));
      }
      for (Use &U : llvm::make_early_inc_range(handle->uses())) {
        User *usr = U.getUser();
        if (auto *CB = dyn_cast<CallBase>(usr)) {
          Function *callee = CB->getCalledFunction();
          if (callee && callee->getName().startswith("llvm.cj.gcwrite"))
            U.set(baseH);
          else
            U.set(getMidH());
          continue;
        }
        if (isa<StoreInst>(usr))
          U.set(baseH);
        // Any other user shape was rejected during classification; a stray
        // constant user would keep the old buffer alive (duplication, not
        // miscompile).
      }
      continue;
    }
    if (shape.kind != HandleShape::ConstGepIntoData)
      continue;
    Constant *newH = repointHandleToBytePool(shape, pool, baseOffset);
    handle->replaceAllUsesWith(newH);
  }
  // Offset fixup for gcwrite/plain-store String materializations: the
  // paired (off:len) i64 stores had off=0; set off = baseOffset so the
  // materialized String reads {poolBase, baseOffset, length}.
  for (StoreInst *SI : b.offsetStores) {
    auto *ci = cast<ConstantInt>(SI->getValueOperand());
    uint64_t v = ci->getValue().getZExtValue();
    SI->setOperand(0, ConstantInt::get(ci->getType(), v | baseOffset));
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
  // Path 2 (struct-nesting): buffers with CastChain handles (MArray base
  // pointers whose header must stay valid) or ConstGepOther handles. This
  // path DOES trigger in practice -- a full-LTO build of stdlib + user
  // program produces a surviving $const_cjstring_data.nested.<n> pool (e.g.
  // stdlib exception message buffers reached via non-[0,2,off] constant
  // GEPs, and literal buffers whose wrappers were folded to cast chains).
  // Note each nested field keeps its own klassInfo pointer, so this path
  // adds one .rela.dyn entry per field under PIE; it is a size trade-off,
  // not a win.
  LLVM_DEBUG(errs() << "CJStringPoolMerge: path 2 (struct-nesting) merged "
                    << eligible.size() << " buffers into " << nested->getName()
                    << " (fallback path)\n");
  // Repoint every handle, then erase the old buffers.
  for (unsigned i = 0; i < eligible.size(); ++i) {
    BufInfo *b = eligible[i];
    for (Constant *h : b->directHandles) {
      HandleShape s = decomposeHandle(h, b->buf);
      Constant *newH = repointHandleToNested(s, nested, i);
      h->replaceAllUsesWith(newH);
    }
    // Repoint the owning literal wrapper (if any) to this field: the field
    // is a verbatim copy of the old buffer with a valid header, so the
    // literal becomes {field, 0, len}. Without this the literal would keep
    // the old buffer alive and its content would be duplicated.
    if (b->lit) {
      auto &ctx = M.getContext();
      auto *i32Ty = Type::getInt32Ty(ctx);
      auto *i8PtrTy = Type::getInt8PtrTy(ctx);
      Constant *fieldPtr = ConstantExpr::getGetElementPtr(
          nested->getValueType(), nested,
          ArrayRef<Constant *>{ConstantInt::getSigned(i32Ty, 0),
                               ConstantInt::get(i32Ty, i)},
          true);
      Constant *fieldI8 = ConstantExpr::getBitCast(fieldPtr, i8PtrTy);
      Constant *fieldAddr1 =
          ConstantExpr::getAddrSpaceCast(fieldI8, Type::getInt8PtrTy(ctx, 1u));
      auto *litTy = cast<StructType>(b->lit->getValueType());
      b->lit->setInitializer(ConstantStruct::get(
          litTy, {fieldAddr1, ConstantInt::getSigned(i32Ty, 0),
                  ConstantInt::getSigned(i32Ty, b->length)}));
    }
    b->buf->removeDeadConstantUsers();
    if (b->buf->use_empty())
      b->buf->eraseFromParent();
  }
}

PreservedAnalyses CJStringPoolMerge::run(Module &M,
                                         ModuleAnalysisManager &) const {
  // 64-bit only: the {i8*, i64, [N x i8]} buffer shape mirrors the runtime
  // MArray whose data offset sizeof(MArray) is 16 on 64-bit but 8 on
  // 32-bit, which this struct shape cannot model. Skip non-64-bit modules
  // entirely (CodeGen only emits per-literal buffers on 64-bit anyway, so
  // this is a defensive no-op there).
  if (M.getDataLayout().getPointerSize() != 8)
    return PreservedAnalyses::all();

  // Collect every cjstring_data buffer. Direct handles are collected once
  // per buffer into BufInfo.directHandles. Each handle is decomposed once;
  // the shape decides the buffer's fate:
  //   - all handles ConstGepIntoData, or CastChain with only
  //     gcwrite-materialization users (offset stores fixable)
  //                                                    -> byte pool (path 1)
  //   - any other CastChain / ConstGepOther (but none
  //     RuntimeGep/NotRepointable)                     -> nested (path 2)
  //   - any RuntimeGep/NotRepointable                  -> skip whole buffer
  // (A CastChain handle is an MArray base pointer whose header must stay
  // valid -- checked arrayGet in the stdlib loads the length field at
  // base+8. With gcwrite-only users the handle is repointed to the pool
  // base and the paired offset store rewritten, which keeps the pool's own
  // valid header; anything else would observe a garbage header mid-pool.)
  // Buffers with no direct handles go straight to the byte pool (path 1).
  //
  // A buffer that has referring literals is only eligible when one of them
  // is a genuine per-literal wrapper ({buf, 0, dataLen}); anything else --
  // notably a legacy per-package pool, whose literals have offset != 0 --
  // is left wholly untouched (see isPerLiteralWrapper). A buffer with NO
  // referring literal is an orphan whose wrapper was folded away by the
  // per-module opt, leaving only direct handles; merging it by content is
  // safe because handle repointing preserves byte offsets (and it cannot
  // be a legacy pool with live literals -- those would refer to it).
  auto ownerOf = buildOwnerMap(M);
  std::vector<BufInfo> bytePackable;
  std::vector<BufInfo> directUser; // fallback for struct-nesting
  // Packed (off:len) stores already claimed by a byte-pool-bound buffer: a
  // second buffer pairing to the same store would be ambiguous (slot reuse
  // across different strings), so it must fall back to the nested path.
  SmallPtrSet<StoreInst *, 32> claimedOffsetStores;
  for (GlobalVariable &gv : M.globals()) {
    if (!gv.hasAttribute(CJSTRING_DATA_ATTR))
      continue;
    BufInfo info;
    info.buf = &gv;
    if (!collectBufferContent(info))
      continue;
    auto ownerIt = ownerOf.find(&gv);
    if (ownerIt != ownerOf.end()) {
      for (GlobalVariable *lit : ownerIt->second)
        if (isPerLiteralWrapper(lit, &gv, info.length)) {
          info.lit = lit;
          break;
        }
      if (!info.lit) {
        // Literals refer to this buffer but none is a per-literal wrapper:
        // a legacy per-package pool (or foreign shape). Leave it wholly
        // untouched -- merging would rewrite its first literal to span the
        // whole pool (silent miscompile) or duplicate its content.
        continue;
      }
    }
    // No referring literal: orphan buffer with (at most) direct handles.
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
      // handle. Safe to merge + repoint literal + erase buffer. (An orphan
      // with no handles at all is dead weight; leave it for GlobalDCE.)
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
        // MArray base pointer: a mid-pool base would leave a garbage header
        // (checked arrayGet reads the length field at base+8). Byte-pool
        // repoint is allowed only when every user is a gcwrite String
        // materialization whose paired (off:len) store can be rewritten to
        // the pool offset -- restoring the legacy {pool, offset, length}
        // shape with the pool's own valid header. Otherwise the buffer goes
        // to the nested path (per-field valid headers).
        if (!collectOffsetFixups(M.getDataLayout(), h, info,
                                 claimedOffsetStores))
          byteOk = false;
        break;
      case HandleShape::ConstGepIntoData:
        break; // raw data pointer; ok for both paths
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
    if (byteOk) {
      for (StoreInst *SI : info.offsetStores)
        claimedOffsetStores.insert(SI);
      bytePackable.push_back(std::move(info));
    } else if (nestedOk)
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
      // now-dead direct-user buffers (those with an owning literal were
      // already repointed above; orphans have only their direct handles).
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
