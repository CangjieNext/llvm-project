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
// CodeGen emits (deferred pooling) into a single shared byte pool.
//
// Buffer object layout (RawArray<UInt8> style):
//   { i8* klassInfo, i64 length, [N x i8] data }
// cjstring literal layout:
//   { i8 addrspace(1)* bufferPtr, i32 start, i32 length }
//
// Every buffer holds exactly one string and every literal has start == 0.
// Buffer contents are packed into the shared pool (identical contents and
// substrings share storage), then each buffer global is RAUW'd with a
// same-typed alias constant bitcast(gep(i8, sharedPool), base) pointing at
// its relocated content. This transparently fixes up every use — cjstring
// literals, llvm.cj.gcwrite.ref handles and memcpy GEPs alike.
//
// Why alias instead of repointing literals to {sharedPool, newStart}: with
// LTO, the per-module pre-opt pipeline has already folded the (all-zero)
// start fields into plain integer instructions before this pass runs at
// LTOBackend, so changing a literal's start would leave stale baked zeros
// behind. Aliasing never changes any offset, so it is immune to baked
// start values and also composes correctly if a module is merged twice.
//
// Correctness assumption: buffer bytes are only ever read through the +16
// data convention (CJ_MCC_AcquireRawData is ptr + sizeof(MArray) and never
// inspects the header), so a relocated buffer does not need its 16-byte
// header preserved.
//
// The pass runs exactly once per linked unit: non-LTO builds run it in the
// per-module pipeline (skipped when --cangjie-lto pre-opt is active), full
// LTO builds run it in LTOBackend before buildLTODefaultPipeline.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "cj-string-pool-merge"

#include "llvm/Transforms/Scalar/CJStringPoolMerge.h"

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
  if (!CJStringPoolMergeEnable)
    return PreservedAnalyses::all();

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
  if (buffers.size() < 2)
    return PreservedAnalyses::all();

  // Pack all buffer contents, longest first so identical contents and
  // substrings share storage.
  std::vector<BufferInfo *> live;
  for (auto &b : buffers)
    if (!b.gv->use_empty())
      live.push_back(&b);
  std::sort(live.begin(), live.end(),
            [](const BufferInfo *a, const BufferInfo *b) {
              return a->bytes.size() != b->bytes.size()
                         ? a->bytes.size() > b->bytes.size()
                         : a->bytes < b->bytes;
            });
  std::string merged;
  for (auto *b : live) {
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
  if (merged.size() > UINT32_MAX || live.empty())
    return PreservedAnalyses::all();

  uint64_t before = 0;
  for (auto *b : live)
    before += b->bytes.size() + 16;
  BytesBefore += before;
  BytesAfter += merged.size() + 16;

  // Emit the shared pool with the same RawArray shape CodeGen emits.
  auto &ctx = M.getContext();
  auto *i8Ty = Type::getInt8Ty(ctx);
  auto *i8PtrTy = Type::getInt8PtrTy(ctx);
  auto *data = ConstantDataArray::getString(ctx, merged, false);
  auto *poolTy = StructType::get(ctx, {i8PtrTy, Type::getInt64Ty(ctx),
                                       data->getType()});
  auto *poolInit = ConstantStruct::get(
      poolTy, {live.front()->klass,
               ConstantInt::getSigned(Type::getInt64Ty(ctx),
                                      static_cast<int64_t>(merged.size())),
               data});
  auto *mergedGV = new GlobalVariable(
      M, poolTy, true, GlobalValue::PrivateLinkage, poolInit,
      CJSTRING_DATA_PREFIX + std::string("merged"));
  mergedGV->addAttribute(CJSTRING_DATA_ATTR);
  uint64_t maxAlign = 0;
  for (auto *b : live)
    if (b->gv->getAlign())
      maxAlign = std::max(maxAlign, b->gv->getAlign()->value());
  if (maxAlign)
    mergedGV->setAlignment(Align(maxAlign));

  // Replace every buffer with a same-typed alias into the shared pool.
  Constant *mergedI8 = ConstantExpr::getBitCast(mergedGV, i8PtrTy);
  for (auto *b : live) {
    Constant *gep = ConstantExpr::getGetElementPtr(
        i8Ty, mergedI8, ConstantInt::get(Type::getInt64Ty(ctx), b->base));
    Constant *alias = ConstantExpr::getBitCast(gep, b->gv->getType());
    b->gv->replaceAllUsesWith(alias);
  }

  // Erase the old buffers; never leave a duplicate behind.
  unsigned numErased = 0;
  for (auto &b : buffers) {
    b.gv->removeDeadConstantUsers();
    if (b.gv->use_empty()) {
      b.gv->eraseFromParent();
      ++numErased;
    } else {
      errs() << "CJStringPoolMerge: " << b.gv->getName()
             << " still has users after merging, kept (duplicate!)\n";
    }
  }

  if (CJStringPoolMergeVerbose)
    errs() << "CJStringPoolMerge: buffers=" << buffers.size()
           << " merged=" << numErased << " bytes " << before << " -> "
           << merged.size() + 16 << "\n";
  return numErased ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
