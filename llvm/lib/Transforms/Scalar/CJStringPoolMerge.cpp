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
// After CodeGen emits per-literal content-addressed byte buffers and LTO
// GlobalDCE has removed dead literals + their buffers, this pass greedily
// packs the surviving buffers (longest-first, like the legacy CodeGen pool)
// and repoints each surviving cjstring literal to its offset in the new pool.
//
// Buffers whose data array is directly GEP'd by an Instruction (inlined
// stdlib StringBuilder/memcpy code) are skipped: their access bypasses the
// literal's {poolPtr, offset, length} indirection and cannot be repointed
// (same limitation as CJStringPoolTrim; see cangjie-const-pool-pass-design
// skill section 1).
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CJStringPoolMerge.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"

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
};
} // namespace

// Return true if any Instruction directly uses a ConstantExpr that
// (transitively through cast/GEP chains) references `gv`. Such buffers are
// GEP'd directly by inlined runtime code and cannot be repointed. We do NOT
// recurse into cjstring literal globals: those wrap the access and we
// repoint them ourselves.
static bool bufHasDirectInstructionUser(GlobalVariable *gv) {
  SmallVector<Constant *, 16> work;
  SmallPtrSet<Constant *, 32> seen;
  for (User *U : gv->users()) {
    auto *C = dyn_cast<Constant>(U);
    if (!C)
      return true;
    if (seen.insert(C).second)
      work.push_back(C);
  }
  while (!work.empty()) {
    Constant *C = work.pop_back_val();
    for (User *U : C->users()) {
      if (isa<Instruction>(U))
        return true;
      auto *CE = dyn_cast<ConstantExpr>(U);
      // Only follow cast and GEP ConstantExprs up the use chain.
      if (!CE || (!CE->isCast() &&
                  CE->getOpcode() != Instruction::GetElementPtr))
        continue;
      if (seen.insert(CE).second)
        work.push_back(CE);
    }
  }
  return false;
}

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
  if (!arr)
    return false;
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

// Collect all per-literal cjstring buffers that can be repointed, together
// with their owning literals. A buffer is repointable iff it has no direct
// Instruction user and a literal references it.
static void
collectRepointableBuffers(Module &M, std::vector<BufInfo> &out) {
  for (GlobalVariable &gv : M.globals()) {
    if (!gv.hasAttribute(CJSTRING_DATA_ATTR))
      continue;
    if (bufHasDirectInstructionUser(&gv))
      continue;
    BufInfo info;
    info.buf = &gv;
    if (!collectBufferContent(info))
      continue;
    if (!findOwningLiteral(M, info))
      continue;
    out.push_back(std::move(info));
  }
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
static void
repointAndErase(GlobalVariable *pool, ArrayRef<BufInfo> bufs,
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
      report_fatal_error("CJStringPoolMerge: literal offset does not point "
                         "at its own content in the merged pool (possible "
                         "hash collision / repack error)",
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

PreservedAnalyses CJStringPoolMerge::run(Module &M,
                                         ModuleAnalysisManager &) const {
  std::vector<BufInfo> bufs;
  collectRepointableBuffers(M, bufs);
  if (bufs.size() < 2)
    return PreservedAnalyses::all();
  std::string merged;
  std::map<std::string, uint64_t> offsets;
  repackSharedPool(bufs, merged, offsets);
  if (merged.empty())
    return PreservedAnalyses::all();
  GlobalVariable *pool = buildSharedPool(M, bufs, merged, merged.size(), 0);
  repointAndErase(pool, bufs, offsets, merged);
  return PreservedAnalyses::none();
}
