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
// guarantee that, CodeGen emits cjstring literals as NON-constant globals
// (a black hole): pre-link optimization cannot fold field loads, and this
// pass flips every cjstring literal back to constant once the final pool
// layout is fixed. Buffers with direct (non-literal) users are not merged.
//
// The pass runs exactly once per linked unit: non-LTO builds run it in the
// per-module pipeline (skipped when --cangjie-lto pre-opt is active), full
// LTO builds run it in LTOBackend before buildLTODefaultPipeline.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "cj-string-pool-merge"

#include "llvm/Transforms/Scalar/CJStringPoolMerge.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
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
    cl::desc("Merge cangjie cjstring data buffers into one shared pool"));

static cl::opt<bool> CJStringPoolMergeVerbose(
    "cj-string-pool-merge-verbose", cl::Hidden, cl::init(false),
    cl::desc("Print CJStringPoolMerge merge statistics"));

static const char *CJSTRING_DATA_ATTR = "cjstring_data";
static const char *CJSTRING_DATA_PREFIX = "$const_cjstring_data.";
static const char *CJSTRING_LITERAL_ATTR = "cjstring_literal";
static const char *CJSTRING_LITERAL_PREFIX = "$const_cjstring.";

namespace {

struct BufferInfo {
  GlobalVariable *gv;
  Constant *klass; // operand 0 of the buffer initializer, already i8* typed
  std::string bytes;
  uint64_t base = 0; // offset of this buffer's content inside the shared pool
};

bool isStringBuffer(const GlobalVariable &gv) {
  return gv.hasAttribute(CJSTRING_DATA_ATTR) ||
         gv.getName().startswith(CJSTRING_DATA_PREFIX);
}

bool isStringLiteral(const GlobalVariable &gv) {
  return gv.hasAttribute(CJSTRING_LITERAL_ATTR) ||
         gv.getName().startswith(CJSTRING_LITERAL_PREFIX);
}

// A GV holding a String record value (literal record or String-typed global
// variable initialized from a literal).
bool isStringRecordGV(const GlobalVariable &gv) {
  auto *st = dyn_cast<StructType>(gv.getValueType());
  return st && st->hasName() &&
         st->getName().startswith("record.std.core:String");
}

// Every use of the buffer must bottom out in cjstring literal initializers.
// BFS only follows cast/GEP ConstantExpr chains and constant aggregates; it
// stops at literal GlobalVariables and never recurses into their users
// (those are loads of the literal, not direct users of the buffer).
// Returns false for buffers with any direct (instruction) user, which are
// not mergeable.
bool collectLiterals(GlobalVariable *buf, SmallVectorImpl<GlobalVariable *> &lits) {
  SmallVector<Constant *, 8> work;
  for (User *u : buf->users()) {
    auto *c = dyn_cast<Constant>(u);
    if (!c)
      return false;
    work.push_back(c);
  }
  SmallPtrSet<Constant *, 8> seen;
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

// Replace `from` by `to` (same type) inside the constant expression tree.
// Returns the input unchanged when `from` does not occur.
Constant *substituteConstant(Constant *c, Constant *from, Constant *to) {
  if (c == from)
    return to;
  auto *ce = dyn_cast<ConstantExpr>(c);
  if (!ce || (!ce->isCast() && ce->getOpcode() != Instruction::GetElementPtr))
    return c;
  SmallVector<Constant *, 4> ops;
  bool changed = false;
  for (auto &op : ce->operands()) {
    auto *nc = substituteConstant(cast<Constant>(op), from, to);
    changed |= nc != op.get();
    ops.push_back(nc);
  }
  if (!changed)
    return c;
  Type *srcTy = ce->getOpcode() == Instruction::GetElementPtr
                    ? ce->getOperand(0)->getType()->getPointerElementType()
                    : nullptr;
  return ce->getWithOperands(ops, ce->getType(), false, srcTy);
}

// Longest k such that the k-byte suffix of `merged` equals the k-byte
// prefix of `content`.
size_t suffixPrefixOverlap(StringRef merged, StringRef content) {
  size_t maxK = std::min(merged.size(), content.size());
  for (size_t k = maxK; k > 0; --k) {
    if (merged.substr(merged.size() - k) == content.substr(0, k))
      return k;
  }
  return 0;
}

} // end anonymous namespace

PreservedAnalyses CJStringPoolMerge::run(Module &M,
                                         ModuleAnalysisManager &) const {
  if (!CJStringPoolMergeEnable) {
    // Still restore constness: CodeGen emitted the literals as non-constant
    // black holes and expects this pass to flip them back.
    for (auto &gv : M.globals())
      if (isStringLiteral(gv))
        gv.setConstant(true);
    return PreservedAnalyses::none();
  }

  std::vector<BufferInfo> buffers;
  for (auto &gv : M.globals()) {
    if (!isStringBuffer(gv))
      continue;
    auto *init = dyn_cast_or_null<ConstantStruct>(gv.getInitializer());
    auto *cda = init && init->getNumOperands() == 3
                    ? dyn_cast<ConstantDataArray>(init->getOperand(2))
                    : nullptr;
    if (!cda || !cda->isString())
      continue;
    BufferInfo info;
    info.gv = &gv;
    info.klass = init->getOperand(0);
    info.bytes = cda->getAsString().str();
    buffers.push_back(std::move(info));
  }
  NumBuffersSeen += buffers.size();

  // Keep only buffers used exclusively by literals.
  std::vector<std::pair<BufferInfo *, SmallVector<GlobalVariable *, 4>>>
      mergeable;
  for (auto &b : buffers) {
    if (b.gv->use_empty())
      continue;
    SmallVector<GlobalVariable *, 4> lits;
    if (collectLiterals(b.gv, lits) && !lits.empty())
      mergeable.emplace_back(&b, std::move(lits));
  }

  // Pack the contents, longest first so identical contents and substrings
  // share storage.
  std::sort(mergeable.begin(), mergeable.end(),
            [](const auto &a, const auto &b) {
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
      size_t ov = suffixPrefixOverlap(merged, b->bytes);
      pos = merged.size() - ov;
      merged.append(b->bytes, ov, std::string::npos);
    }
    b->base = pos;
  }
  if (merged.size() > UINT32_MAX)
    mergeable.clear();

  uint64_t before = 0;
  for (auto &[b, lits] : mergeable)
    before += b->bytes.size() + 16;
  BytesBefore += before;
  BytesAfter += merged.size() + 16;

  auto &ctx = M.getContext();
  if (!mergeable.empty()) {
    // Emit the shared pool with the same RawArray shape CodeGen emits.
    auto *i8Ty = Type::getInt8Ty(ctx);
    auto *i8PtrTy = Type::getInt8PtrTy(ctx);
    auto *data = ConstantDataArray::getString(ctx, merged, false);
    auto *poolTy = StructType::get(ctx, {i8PtrTy, Type::getInt64Ty(ctx),
                                         data->getType()});
    auto *poolInit = ConstantStruct::get(
        poolTy, {mergeable.front().first->klass,
                 ConstantInt::getSigned(Type::getInt64Ty(ctx),
                                        static_cast<int64_t>(merged.size())),
                 data});
    auto *mergedGV = new GlobalVariable(
        M, poolTy, true, GlobalValue::PrivateLinkage, poolInit,
        CJSTRING_DATA_PREFIX + std::string("merged"));
    mergedGV->addAttribute(CJSTRING_DATA_ATTR);
    uint64_t maxAlign = 0;
    for (auto &[b, lits] : mergeable)
      if (b->gv->getAlign())
        maxAlign = std::max(maxAlign, b->gv->getAlign()->value());
    if (maxAlign)
      mergedGV->setAlignment(Align(maxAlign));

    // Repoint every literal-associated global. Two kinds:
    //  - String record literals {ptr, start, len}: set {mergedPool, base, len}.
    //  - GC root-holder globals ($const_cjstring.*.N holding a bare pointer
    //    to the buffer object): repoint the held pointer to the pool start
    //    (a real object), preserving the constant-expression shape.
    auto *i32Ty = Type::getInt32Ty(ctx);
    for (auto &[b, lits] : mergeable) {
      Constant *mergedAsBufTy = ConstantExpr::getBitCast(mergedGV, b->gv->getType());
      for (auto *lit : lits) {
        auto *oldInit = dyn_cast_or_null<ConstantStruct>(lit->getInitializer());
        bool isRecord = oldInit && oldInit->getNumOperands() == 3 &&
                        isa<StructType>(oldInit->getType());
        if (isRecord) {
          Constant *ptr = ConstantExpr::getBitCast(mergedGV, i8PtrTy);
          if (oldInit->getOperand(0)->getType() != ptr->getType())
            ptr = ConstantExpr::getAddrSpaceCast(ptr,
                                                 oldInit->getOperand(0)->getType());
          lit->setInitializer(ConstantStruct::get(
              cast<StructType>(oldInit->getType()),
              {ptr, ConstantInt::getSigned(i32Ty, b->base),
               oldInit->getOperand(2)}));
          continue;
        }
        if (auto *oldC = lit->getInitializer())
          if (auto *newInit = substituteConstant(oldC, b->gv, mergedAsBufTy))
            if (newInit != oldC)
              lit->setInitializer(newInit);
        // GlobalOpt SRA may have scalarized a literal record into per-field
        // slots named <literal>.0/.1/.2 (object pointer / start / length).
        // The .0 slot was repointed above; keep the .1 (start) slot in sync.
        if (lit->getName().endswith(".0")) {
          auto *s1 = dyn_cast_or_null<GlobalVariable>(
              M.getNamedValue(lit->getName().drop_back(2).str() + ".1"));
          if (s1)
            if (auto *ci = dyn_cast_or_null<ConstantInt>(s1->getInitializer()))
              if (ci->isZero())
                s1->setInitializer(ConstantInt::getSigned(i32Ty, b->base));
        }
      }
    }
    NumMerged += mergeable.size();
  }

  // Erase the merged buffers; never leave a duplicate behind.
  unsigned numErased = 0;
  for (auto &[b, lits] : mergeable) {
    b->gv->removeDeadConstantUsers();
    if (b->gv->use_empty()) {
      b->gv->eraseFromParent();
      ++numErased;
    } else {
      errs() << "CJStringPoolMerge: " << b->gv->getName()
             << " still has users after merging, kept (duplicate!)\n";
    }
  }

  // Black-hole teardown: literal records that were emitted as non-constant
  // (marked with cjstring_literal) become constant again now that the pool
  // layout is final. Mutable String globals (no attribute) and GC
  // root-holder globals (not records) stay untouched.
  for (auto &gv : M.globals())
    if (isStringLiteral(gv) && isStringRecordGV(gv))
      gv.setConstant(true);

  if (CJStringPoolMergeVerbose)
    errs() << "CJStringPoolMerge: buffers=" << buffers.size()
           << " merged=" << mergeable.size() << " erased=" << numErased
           << " bytes " << before << " -> " << merged.size() + 16 << "\n";
  return PreservedAnalyses::none();
}
