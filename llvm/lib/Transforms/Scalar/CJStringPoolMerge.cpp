//===- CJStringPoolMerge.cpp - merge cangjie string literal buffers -------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Merge the per-string cjstring data buffers ($const_cjstring_data.*) that
// CodeGen emits (deferred pooling) into one shared byte pool, and repoint
// the string literals from {buffer, 0, len} to {mergedPool, newStart, len}.
//
// Buffer object layout (RawArray<UInt8> style):
//   { i8* klassInfo, i64 length, [N x i8] data }
// cjstring literal layout:
//   { i8 addrspace(1)* bufferPtr, i32 start, i32 length }
//
// Every buffer holds exactly one string and every literal initially has
// start == 0. A buffer whose uses all bottom out in cjstring literal
// initializers is packed into the shared pool (identical contents and
// substrings share storage) and each of its literals is repointed.
// The literal's first field keeps pointing at the start of a real RawArray
// object (the merged pool), so String.rawData() stays valid; the start
// field becomes a view offset into the pool, exactly like the legacy
// per-package pool layout.
//
// Repointing changes the start field, which is only safe if no consumer
// has baked the old (zero) start into plain integer instructions. To
// guarantee that, CodeGen emits cjstring literals as NON-constant globals:
// pre-link optimization cannot fold field loads, and this
// pass flips every cjstring literal back to constant once the final pool
// layout is fixed. Buffers with direct (non-literal) users are not merged.
//
// The pass runs exactly once per linked unit: non-LTO builds run it in the
// per-module pipeline (skipped when --cangjie-lto pre-opt is active), LTO
// builds run it in LTOBackend before the full/thin default pipeline.
//
// Only buffers emitted by the deferred-pooling CodeGen are mergeable; they
// carry the "cjstring_deferred" attribute. Legacy per-package pools from
// older compilers (e.g. prebuilt .bc) share the "cjstring_data" attribute
// and the "$const_cjstring_data." name prefix, but their literals have
// nonzero start offsets and possibly already-folded consumers, so they are
// left completely untouched.
//
// -cj-string-pool-merge=false turns the merge (and the repoint) off, but it
// does not undo the CodeGen change: the per-literal buffers stay and every
// literal keeps start == 0. It is a merge-off switch, not a revert to the
// legacy per-package pool.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "cj-string-pool-merge"

#include "llvm/Transforms/Scalar/CJStringPoolMerge.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/CJStringAttrs.h"
#include <algorithm>
#include <string>
#include <vector>

using namespace llvm;

STATISTIC(NumBuffersSeen, "Number of cjstring data buffers found");
STATISTIC(NumMerged, "Number of buffers merged into the shared pool");
STATISTIC(BytesBefore, "Total buffer bytes before merging");
STATISTIC(BytesAfter, "Shared pool bytes after merging");

static cl::opt<bool> CJStringPoolMergeEnable(
    "cj-string-pool-merge", cl::Hidden, cl::init(true),
    cl::desc("Merge cangjie cjstring data buffers into one shared pool. "
             "Disabling skips the merge but keeps the CodeGen-emitted "
             "per-literal buffers; it does not restore the legacy "
             "per-package pool layout"));

static cl::opt<bool> CJStringPoolMergeVerbose(
    "cj-string-pool-merge-verbose", cl::Hidden, cl::init(false),
    cl::desc("Print CJStringPoolMerge merge statistics"));

// RawArray buffer layout: {i8* klass, i64 length, [N x i8] data}.
constexpr unsigned BUFFER_FIELD_COUNT = 3;
constexpr unsigned BUFFER_DATA_FIELD = 2; // index of the byte-array field
// cjstring literal record layout: {i8 addrspace(1)* buffer, i32 start, i32 len}.
constexpr unsigned LITERAL_FIELD_COUNT = 3;
constexpr unsigned LITERAL_LEN_FIELD = 2; // index of the length field

namespace {

struct BufferInfo {
  GlobalVariable *gv;
  Constant *klass; // operand 0 of the buffer initializer, already i8* typed
  std::string bytes;
  uint64_t base = 0; // offset of this buffer's content inside the shared pool
};

bool isStringBuffer(const GlobalVariable &gv) {
  // Only deferred-pooling buffers are mergeable. Legacy pools must not be
  // matched by name prefix or the older cjstring_data attribute.
  return gv.hasAttribute(CJSTRING_DEFERRED_ATTR);
}

bool isStringLiteral(const GlobalVariable &gv) {
  return gv.hasAttribute(CJSTRING_LITERAL_ATTR);
}

// A GV holding the std.core String record value (literal record or
// String-typed global variable initialized from a literal). The name is
// matched exactly: a prefix test would also accept hypothetical std.core
// structs named String*, and those must not be frozen into read-only memory.
bool isStringRecordGV(const GlobalVariable &gv) {
  auto *st = dyn_cast<StructType>(gv.getValueType());
  return st && st->hasName() && st->getName() == "record.std.core:String";
}

// Every use of the buffer must bottom out in cjstring literal initializers.
// BFS only follows cast/GEP ConstantExpr chains and constant aggregates; it
// stops at literal GlobalVariables and never recurses into their users
// (those are loads of the literal, not direct users of the buffer).
// Returns false for buffers with any direct (instruction) user, which are
// not mergeable.
bool collectLiterals(GlobalVariable *buf, SmallVectorImpl<GlobalVariable *> &lits) {
  SmallVector<Constant *> work;
  for (User *u : buf->users()) {
    auto *c = dyn_cast<Constant>(u);
    if (!c)
      return false;
    work.push_back(c);
  }
  SmallDenseSet<Constant *> seen;
  while (!work.empty()) {
    Constant *c = work.pop_back_val();
    if (!seen.insert(c).second)
      continue;
    if (auto *ce = dyn_cast<ConstantExpr>(c)) {
      if (!ce->isCast() && ce->getOpcode() != Instruction::GetElementPtr)
        return false;
    }
    for (User *u : c->users()) {
      if (auto *gv = dyn_cast<GlobalVariable>(u)) {
        if (!isStringLiteral(*gv) && !isStringRecordGV(*gv))
          return false;
        lits.push_back(gv);
        continue;
      }
      auto *cu = dyn_cast<Constant>(u);
      if (!cu)
        return false;
      work.push_back(cu);
    }
  }
  return true;
}

// Longest k such that the k-byte suffix of `merged` equals the k-byte
// prefix of `content`. Uses the Knuth-Morris-Pratt failure function of
// `content` plus one scan over the only bytes that can participate (the
// tail of `merged`), i.e. O(|content|) per call instead of the quadratic
// worst case of trying every candidate overlap length.
// https://en.wikipedia.org/wiki/Knuth%E2%80%93Morris%E2%80%93Pratt_algorithm
size_t kmpSuffixPrefixOverlap(StringRef merged, StringRef content) {
  size_t contentLen = content.size();
  // Invariant: callers skip empty buffers, so content is never empty;
  // merged is empty only for the very first buffer inserted into the pool.
  assert(contentLen != 0 &&
         "content must be non-empty (empty buffers are skipped)");
  if (contentLen == 0 || merged.empty())
    return 0;
  std::vector<size_t> fail(contentLen, 0);
  for (size_t i = 1, matched = 0; i < contentLen; ++i) {
    while (matched && content[i] != content[matched])
      matched = fail[matched - 1];
    if (content[i] == content[matched])
      ++matched;
    fail[i] = matched;
  }
  size_t tailLen = std::min(merged.size(), contentLen);
  StringRef tail = merged.substr(merged.size() - tailLen);
  size_t matched = 0;
  for (char ch : tail) {
    while (matched && ch != content[matched])
      matched = fail[matched - 1];
    if (ch == content[matched])
      ++matched;
  }
  return matched;
}

using MergeableList =
    std::vector<std::pair<BufferInfo *, SmallVector<GlobalVariable *>>>;

// Collect every cjstring data buffer that still carries a RawArray-style
// {klass, length, bytes} initializer.
std::vector<BufferInfo> collectBuffers(Module &M) {
  std::vector<BufferInfo> buffers;
  // Class info shared by the buffers merged below; enforced in the loop.
  Constant *klassInfo = nullptr;
  for (auto &gv : M.globals()) {
    if (!isStringBuffer(gv))
      continue;
    auto *init = dyn_cast_or_null<ConstantStruct>(gv.getInitializer());
    // Invariant: CJSTRING_DEFERRED_ATTR is emitted only by Cangjie CodeGen,
    // always on buffers of the RawArray shape {i8*, i64, [N x i8]}; bitcode
    // predating the attribute never carries it. The guard stays so foreign
    // IR degrades to skipping the buffer instead of crashing release builds.
    assert(init && init->getNumOperands() == BUFFER_FIELD_COUNT &&
           "cjstring_deferred buffer must be a {i8*, i64, [N x i8]} struct");
    if (!init || init->getNumOperands() != BUFFER_FIELD_COUNT)
      continue;
    Constant *data = init->getOperand(BUFFER_DATA_FIELD);
    BufferInfo info;
    if (auto *cda = dyn_cast<ConstantDataArray>(data)) {
      // Invariant: the data operand is an i8 array (RawArray<UInt8> data).
      assert(cda->isString() && "cjstring buffer data must be an i8 array");
      if (!cda->isString())
        continue;
      info.bytes = cda->getAsString().str();
    } else if (auto *caz = dyn_cast<ConstantAggregateZero>(data)) {
      // ConstantAggregateZero is not a ConstantDataArray (both derive from
      // ConstantData), so the branch above never sees the zero-filled form:
      // ConstantDataSequential::getImpl returns a zero aggregate directly for
      // all-zero data, which CodeGen reaches through
      // ConstantDataArray::getString. The element count is kept, so the empty
      // string arrives as a 0-element [0 x i8] and a single-NUL-byte literal
      // as a 1-element [1 x i8]. Their contents are exactly NUL bytes, so they
      // merge like any other deferred buffer; treating only the 0-element form
      // as mergeable left the other one behind as a standalone object with a
      // needless class-info relocation.
      auto *arr = dyn_cast<ArrayType>(caz->getType());
      if (!arr || !arr->getElementType()->isIntegerTy(8))
        continue;
      info.bytes.assign(arr->getNumElements(), '\0');
    } else {
      continue;
    }
    info.gv = &gv;
    info.klass = init->getOperand(0);
    // Invariant: CodeGen emits every deferred buffer as a RawArray<UInt8> over
    // one class info object, so a single merged pool can stand in for all of
    // them. This is deliberately not an assert: if two class info objects do
    // coexist (e.g. a prebuilt/foreign module merged alongside this one), a
    // buffer with the other class info is not malformed, it just cannot be
    // repointed into a pool whose class would then be wrong for its strings;
    // leaving it unmerged is correct, while aborting would not be.
    Constant *klass = info.klass->stripPointerCasts();
    if (klassInfo && klassInfo != klass)
      continue;
    klassInfo = klass;
    buffers.push_back(std::move(info));
  }
  return buffers;
}

// Keep only buffers used exclusively by cjstring literals (directly or via
// constant chains); buffers with instruction users are not mergeable.
MergeableList filterMergeable(std::vector<BufferInfo> &buffers) {
  MergeableList mergeable;
  for (auto &b : buffers) {
    if (b.gv->use_empty())
      continue;
    SmallVector<GlobalVariable *> lits;
    // lits can be empty when every use chain ends in dead constants (e.g.
    // an orphaned bitcast); such a buffer has nothing to repoint.
    if (collectLiterals(b.gv, lits) && !lits.empty())
      mergeable.emplace_back(&b, std::move(lits));
  }
  return mergeable;
}

// Pack the contents into one byte pool, longest first so identical contents
// and substrings share storage; sets BufferInfo::base for each buffer.
// start is an i32 in the literal record, so a pool beyond 4 GiB cannot be
// addressed: fall back to no merging in that (unrealistic) case.
std::string layoutMergedPool(MergeableList &mergeable) {
  std::sort(mergeable.begin(), mergeable.end(), [](const auto &a, const auto &b) {
    return a.first->bytes.size() != b.first->bytes.size()
               ? a.first->bytes.size() > b.first->bytes.size()
               : a.first->bytes < b.first->bytes;
  });
  std::string merged;
  for (auto &[b, lits] : mergeable) {
    if (b->bytes.empty()) {
      b->base = 0;
      continue;
    }
    size_t pos = merged.find(b->bytes);
    if (pos == std::string::npos) {
      size_t ov = kmpSuffixPrefixOverlap(merged, b->bytes);
      pos = merged.size() - ov;
      merged.append(b->bytes, ov, std::string::npos);
    }
    b->base = pos;
  }
  if (merged.size() > UINT32_MAX) {
    mergeable.clear();
    merged.clear();
  }
  return merged;
}

// Emit the shared pool with the same RawArray shape CodeGen emits.
GlobalVariable *emitMergedPool(Module &M, const MergeableList &mergeable,
                               const std::string &merged) {
  auto &ctx = M.getContext();
  auto *i8PtrTy = Type::getInt8PtrTy(ctx);
  auto *data = ConstantDataArray::getString(ctx, merged, false);
  auto *poolTy =
      StructType::get(ctx, {i8PtrTy, Type::getInt64Ty(ctx), data->getType()});
  // Precondition (enforced by collectBuffers): every mergeable buffer carries
  // the same class info, so a single pool value is correct for all of them.
  auto *poolInit = ConstantStruct::get(
      poolTy, {mergeable.front().first->klass,
               ConstantInt::getSigned(Type::getInt64Ty(ctx),
                                      static_cast<int64_t>(merged.size())),
               data});
  auto *mergedGV = new GlobalVariable(M, poolTy, true,
                                      GlobalValue::PrivateLinkage, poolInit,
                                      CJSTRING_DATA_PREFIX + std::string("merged"));
  mergedGV->addAttribute(CJSTRING_DATA_ATTR);
  uint64_t maxAlign = 0;
  for (auto &[b, lits] : mergeable)
    if (b->gv->getAlign())
      maxAlign = std::max(maxAlign, b->gv->getAlign()->value());
  if (maxAlign)
    mergedGV->setAlignment(Align(maxAlign));
  return mergedGV;
}

// Repoint one literal record to {mergedPool, base, len}: rewrite the
// record initializer, keeping the length field. Every literal collected by
// collectLiterals has a String-record initializer: aggregates reference the
// literal record GV rather than the buffer, and any other global holding a
// buffer reference makes the buffer unmergeable there.
void repointLiteral(Module &M, GlobalVariable *lit, GlobalVariable *mergedGV,
                    int64_t base) {
  auto &ctx = M.getContext();
  auto *i32Ty = Type::getInt32Ty(ctx);
  auto *st = dyn_cast<ConstantStruct>(lit->getInitializer());
  if (!st || st->getNumOperands() != LITERAL_FIELD_COUNT ||
      !isa<StructType>(st->getType()))
    return;
  auto *i8PtrTy = Type::getInt8PtrTy(ctx);
  Constant *ptr = ConstantExpr::getBitCast(mergedGV, i8PtrTy);
  if (st->getOperand(0)->getType() != ptr->getType())
    ptr = ConstantExpr::getAddrSpaceCast(ptr, st->getOperand(0)->getType());
  lit->setInitializer(ConstantStruct::get(
      cast<StructType>(st->getType()),
      {ptr, ConstantInt::getSigned(i32Ty, base),
       st->getOperand(LITERAL_LEN_FIELD)}));
}

void repointLiterals(Module &M, MergeableList &mergeable,
                     GlobalVariable *mergedGV) {
  for (auto &[b, lits] : mergeable)
    for (auto *lit : lits)
      repointLiteral(M, lit, mergedGV, b->base);
}

// Erase the merged buffers; never leave a duplicate behind.
unsigned eraseMergedBuffers(MergeableList &mergeable) {
  unsigned numErased = 0;
  for (auto &[b, lits] : mergeable) {
    b->gv->removeDeadConstantUsers();
    if (b->gv->use_empty()) {
      b->gv->eraseFromParent();
      ++numErased;
    } else if (CJStringPoolMergeVerbose) {
      errs() << "CJStringPoolMerge: " << b->gv->getName()
             << " still has users after merging, kept (duplicate!)\n";
    }
  }
  return numErased;
}

// Constness restore: literal records that were emitted as non-constant
// (marked with cjstring_literal) become constant again now that the pool
// layout is final. Only String-record globals are flipped: any other global
// that happens to carry the attribute (foreign IR, attribute-marked
// aggregates) is left untouched, so a store to such a global can never be
// turned into a store to read-only memory.
void restoreConstantLiterals(Module &M) {
  for (auto &gv : M.globals())
    if (isStringLiteral(gv) && isStringRecordGV(gv))
      gv.setConstant(true);
}

// Bytes a cjstring buffer spends on its header, i.e. the offset of its
// byte-array data field. The target DataLayout computes it (16 on LP64 and on
// 32-bit AAPCS ARM, whose i64 is 8-aligned; 12 for a 4-aligned i64), so the
// byte statistics are right on every target instead of assuming a 64-bit
// header. Only the statistics use this: the merge never depends on the header
// size because it rewrites the content-relative `start` and keeps the
// literal's first field pointing at a RawArray object start.
uint64_t bufferHeaderBytes(const DataLayout &DL, const BufferInfo &B) {
  auto *BufTy = dyn_cast<StructType>(B.gv->getValueType());
  if (!BufTy)
    return 0;
  return DL.getStructLayout(BufTy)->getElementOffset(BUFFER_DATA_FIELD);
}

} // end anonymous namespace

PreservedAnalyses CJStringPoolMerge::run(Module &M,
                                         ModuleAnalysisManager &) const {
  // Merging off: CodeGen has already emitted one buffer per string and the
  // literal records as non-constant globals, so there is nothing to undo here.
  // Skip the packing/repoint entirely, but still restore constness -- the
  // records are kept non-constant until relayout and rely on this pass
  // flipping them back.
  // The result keeps every per-literal buffer and every start == 0 literal:
  // it is the deferred layout without pooling, not the legacy per-package
  // pool (which had nonzero start offsets into a single package-wide buffer).
  if (!CJStringPoolMergeEnable) {
    restoreConstantLiterals(M);
    return PreservedAnalyses::none();
  }

  auto buffers = collectBuffers(M);
  NumBuffersSeen += buffers.size();
  auto mergeable = filterMergeable(buffers);
  std::string merged = layoutMergedPool(mergeable);

  uint64_t before = 0;
  for (auto &[b, lits] : mergeable)
    before += b->bytes.size() + bufferHeaderBytes(M.getDataLayout(), *b);
  BytesBefore += before;
  uint64_t after = 0;
  if (!mergeable.empty())
    after = merged.size() +
            bufferHeaderBytes(M.getDataLayout(), *mergeable.front().first);
  BytesAfter += after;

  if (!mergeable.empty()) {
    auto *mergedGV = emitMergedPool(M, mergeable, merged);
    repointLiterals(M, mergeable, mergedGV);
    NumMerged += mergeable.size();
  }
  unsigned numErased = eraseMergedBuffers(mergeable);
  restoreConstantLiterals(M);

  if (CJStringPoolMergeVerbose)
    errs() << "CJStringPoolMerge: buffers=" << buffers.size()
           << " merged=" << mergeable.size() << " erased=" << numErased
           << " bytes " << before << " -> " << after << "\n";
  return PreservedAnalyses::none();
}
