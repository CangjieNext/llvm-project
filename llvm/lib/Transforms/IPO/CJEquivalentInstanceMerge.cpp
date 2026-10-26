//===- CJEquivalentInstanceMerge.cpp - Merge equivalent CJ instances ------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// IntNative/Int64, UIntNative/UInt64 (64-bit targets), and Rune/Int32 have
// identical value representations and GC layouts. Their monomorphized bodies
// nevertheless reference distinct type-info globals, which prevents the
// identity-based FunctionComparator from merging otherwise identical bodies.
//
// This pass rewrites only two kinds of layout-only uses:
// 1. RawArray<T>.ti passed to CJ_MCC_NewArray*. RawArray is an internal
//    representation; the public Array<T> type info and every global metadata
//    initializer retain the original type identity.
// 2. <Type>.ti stored into the header of a PEA-produced stack object. The
//    object layout must match the type-info name and a whole-use scan must
//    prove that the object neither escapes nor has its type header read.
//
// No primitive type-info use, global initializer, reflection record, type
// test, or heap object allocation other than RawArray is rewritten. Candidate
// type infos must also agree on kind, flags, size, alignment, GC bitmap, and
// field offsets. After canonicalization MergeFunctions can share bodies. It
// still follows the ordinary IR address-significance rules: direct-only
// internal entries may disappear, while address-taken entries remain thunks.
//
// In the non-LTO pipeline the nested MergeFunctions run happens only when this
// pass changed a candidate use. Full LTO requests canonicalization only and
// uses the MergeFunctions pass already present in its pipeline.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/CJEquivalentInstanceMerge.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/IPO/MergeFunctions.h"
#include "llvm/Transforms/Scalar/CJFillMetadata.h"

using namespace llvm;

#define DEBUG_TYPE "cj-equivalent-instance-merge"

STATISTIC(NumRawArrayUses, "Number of RawArray type-info uses canonicalized");
STATISTIC(NumStackUses, "Number of stack-object type-info uses canonicalized");

static cl::opt<bool> EnableCJEquivalentInstanceMerge(
    "cj-equivalent-instance-merge", cl::Hidden, cl::init(true),
    cl::desc("Merge monomorphized functions differing only in layout-only "
             "type-info uses of equivalent Cangjie primitive types."));

namespace {

constexpr size_t TYPE_INFO_SUFFIX_SIZE = 3;     // 3 = strlen(".ti")
constexpr size_t OBJECT_STRUCT_FIELD_COUNT = 2; // 2 = type header + body
constexpr unsigned POINTER_SIZE_64 = 8;
constexpr unsigned GC_STRUCT_SOURCE_ARG = 2; // 2 = source operand
constexpr unsigned MAX_STACK_OBJECT_USES = 4096;
constexpr unsigned LAYOUT_FIELDS[] = {CIT_TYPE,   CIT_FLAG,  CIT_FIELD_NUM,
                                      CIT_SIZE,   CIT_GCTIB, CIT_ALIGN,
                                      CIT_OFFSETS};

StringRef getEquivalentPrimitive(StringRef name, const DataLayout &dl) {
  if (name == "Rune")
    return "Int32";
  if (dl.getPointerSize() != POINTER_SIZE_64)
    return {};
  if (name == "IntNative")
    return "Int64";
  if (name == "UIntNative")
    return "UInt64";
  return {};
}

const GlobalVariable *getConstantDataGlobal(const Constant *value,
                                            const DataLayout &dl) {
  int64_t offset = 0;
  Value *base = GetPointerBaseWithConstantOffset(const_cast<Constant *>(value),
                                                 offset, dl);
  auto *global = dyn_cast_or_null<GlobalVariable>(base);
  return global && offset == 0 && global->hasInitializer() ? global : nullptr;
}

bool hasSameConstantData(const Constant *from, const Constant *to,
                         const DataLayout &dl) {
  if (from == to)
    return true;
  const GlobalVariable *fromGlobal = getConstantDataGlobal(from, dl);
  const GlobalVariable *toGlobal = getConstantDataGlobal(to, dl);
  if (!fromGlobal || !toGlobal)
    return false;
  // LLVM constants are context-uniqued. Identical aggregate initializers
  // therefore compare equal here without following mutable pointer identity.
  return fromGlobal->getInitializer() == toGlobal->getInitializer();
}

bool hasSameRuntimeLayout(const GlobalVariable &from, const GlobalVariable &to,
                          const DataLayout &dl) {
  if (!from.hasInitializer() || !to.hasInitializer())
    return false;
  const auto *fromType = dyn_cast<StructType>(from.getValueType());
  const auto *toType = dyn_cast<StructType>(to.getValueType());
  if (!fromType || !toType || fromType->getNumElements() != CIT_MAX ||
      toType->getNumElements() != CIT_MAX)
    return false;
  const Constant *fromBody = from.getInitializer();
  const Constant *toBody = to.getInitializer();
  for (unsigned field : LAYOUT_FIELDS) {
    const Constant *fromField = fromBody->getAggregateElement(field);
    const Constant *toField = toBody->getAggregateElement(field);
    if (!fromField || !toField)
      return false;
    if (field == CIT_OFFSETS || field == CIT_GCTIB) {
      if (!hasSameConstantData(fromField, toField, dl))
        return false;
    } else if (fromField != toField) {
      return false;
    }
  }
  return true;
}

GlobalVariable *getCanonicalTypeInfo(GlobalVariable &from, Module &m) {
  StringRef name = from.getName();
  if (!name.endswith(".ti") || !name.endswith(">.ti"))
    return nullptr;
  const size_t open = name.find('<');
  if (open == StringRef::npos)
    return nullptr;
  const size_t close = name.size() - TYPE_INFO_SUFFIX_SIZE;
  StringRef argument = name.slice(open + 1, close - 1);
  StringRef canonical = getEquivalentPrimitive(argument, m.getDataLayout());
  if (canonical.empty())
    return nullptr;

  std::string canonicalName = name.take_front(open + 1).str();
  canonicalName += canonical;
  canonicalName += ">.ti";
  auto *to = m.getGlobalVariable(canonicalName);
  if (!to || to == &from || !hasSameRuntimeLayout(from, *to, m.getDataLayout()))
    return nullptr;
  return to;
}

GlobalVariable *getReferencedTypeInfo(Value *value) {
  return dyn_cast<GlobalVariable>(value->stripPointerCasts());
}

Constant *bitcastToType(GlobalVariable &ti, Type *type) {
  return ConstantExpr::getBitCast(&ti, type);
}

bool isRawArrayAllocation(const CallBase &cb) {
  const Function *callee = cb.getCalledFunction();
  return callee && cb.arg_size() > 0 &&
         callee->getName().startswith("CJ_MCC_NewArray");
}

bool canonicalizeRawArrayAllocation(CallBase &cb, Module &m) {
  if (!isRawArrayAllocation(cb))
    return false;
  auto *from = getReferencedTypeInfo(cb.getArgOperand(0));
  if (!from || !from->getName().startswith("RawArray<"))
    return false;
  GlobalVariable *to = getCanonicalTypeInfo(*from, m);
  if (!to)
    return false;
  cb.setArgOperand(0, bitcastToType(*to, cb.getArgOperand(0)->getType()));
  ++NumRawArrayUses;
  return true;
}

bool isLayoutOnlyCall(const CallBase &cb) {
  const Function *callee = cb.getCalledFunction();
  if (!callee)
    return false;
  StringRef name = callee->getName();
  if (name.startswith("llvm.lifetime.") || name.startswith("llvm.dbg.") ||
      name.startswith("llvm.mem") || name.startswith("llvm.cj.gc") ||
      name.startswith("llvm.cj.mem") ||
      name.startswith("llvm.experimental.gc."))
    return true;
  return name == "CJ_MCC_HandleSafepoint" || name == "CJ_MCC_InvokeGC";
}

bool isHeaderAccess(const Value *pointer, const Value &object,
                    const DataLayout &dl) {
  int64_t offset = 0;
  Value *base = GetPointerBaseWithConstantOffset(const_cast<Value *>(pointer),
                                                 offset, dl);
  // Callers only query pointers proven to be derived from object. If constant
  // folding cannot recover that exact base (for example through select/phi or
  // a variable GEP), the pointer may still address the header at run time.
  return base == &object ? offset == 0 : true;
}

class StackObjectSafety {
public:
  explicit StackObjectSafety(const DataLayout &dl) : dl(dl) {}

  bool isUnobservable(const Value &object) {
    // Only a completed root analysis can be cached. A nested result may depend
    // on a provisional back edge in a recursive cycle and must not be reused by
    // a different root with additional unsafe operations.
    if (active.empty()) {
      auto cached = cache.find(&object);
      if (cached != cache.end())
        return cached->second;
    }
    if (active.contains(&object))
      return true;
    active.insert(&object);
    bool safe = scan(object);
    active.erase(&object);
    if (active.empty())
      cache[&object] = safe;
    return safe;
  }

private:
  bool isSafeGCUse(const CallBase &cb, unsigned argNo, const Value &value,
                   const Value &object) {
    switch (cb.getIntrinsicID()) {
    case Intrinsic::cj_gcread_static_ref:
      if (argNo == 0)
        return !isHeaderAccess(&value, object, dl);
      break;
    case Intrinsic::cj_gcread_ref:
    case Intrinsic::cj_gcread_weakref:
    case Intrinsic::cj_gcread_static_struct:
    case Intrinsic::cj_gcwrite_static_struct:
      if (argNo == 1)
        return !isHeaderAccess(&value, object, dl);
      break;
    case Intrinsic::cj_gcread_struct:
    case Intrinsic::cj_gcwrite_struct:
    case Intrinsic::cj_gcread_generic:
    case Intrinsic::cj_gcwrite_generic:
      if (argNo == GC_STRUCT_SOURCE_ARG)
        return !isHeaderAccess(&value, object, dl);
      break;
    default:
      break;
    }
    return true;
  }

  bool isSafeCall(const Use &use, const Value &value, const Value &object) {
    const auto &cb = cast<CallBase>(*use.getUser());
    if (!cb.isArgOperand(&use) || !cb.doesNotCapture(use.getOperandNo()))
      return false;
    const Function *callee = cb.getCalledFunction();
    if (isLayoutOnlyCall(cb)) {
      StringRef name = callee->getName();
      if (name.startswith("llvm.lifetime.") || name.startswith("llvm.dbg."))
        return true;
      // Copying or reading the header could expose its TypeInfo even when the
      // object pointer itself is nocapture. Payload accesses remain eligible.
      if ((isa<MemIntrinsic>(cb) || name.startswith("llvm.cj.mem")) &&
          isHeaderAccess(&value, object, dl))
        return false;
      unsigned argNo = cb.getArgOperandNo(&use);
      if (!isSafeGCUse(cb, argNo, value, object))
        return false;
      return true;
    }
    if (!callee || callee->isDeclaration() || !callee->hasLocalLinkage())
      return false;
    unsigned argNo = cb.getArgOperandNo(&use);
    if (argNo >= callee->arg_size())
      return false;
    return isUnobservable(*callee->getArg(argNo));
  }

  bool isSafeUse(const Use &use, const Value &value, const Value &object,
                 SmallVectorImpl<const Value *> &worklist) {
    const User *user = use.getUser();
    if (isa<CallBase>(user))
      return isSafeCall(use, value, object);
    if (isa<BitCastInst>(user) || isa<AddrSpaceCastInst>(user) ||
        isa<GetElementPtrInst>(user) || isa<PHINode>(user) ||
        isa<SelectInst>(user) || isa<FreezeInst>(user)) {
      worklist.push_back(cast<Value>(user));
      return true;
    }
    if (const auto *load = dyn_cast<LoadInst>(user))
      return !isHeaderAccess(load->getPointerOperand(), object, dl);
    if (const auto *store = dyn_cast<StoreInst>(user))
      return store->getValueOperand() != &value;
    return isa<ICmpInst>(user);
  }

  bool scan(const Value &object) {
    SmallVector<const Value *> worklist;
    SmallDenseSet<const Value *> visited;
    worklist.push_back(&object);
    while (!worklist.empty()) {
      const Value *value = worklist.pop_back_val();
      if (!visited.insert(value).second)
        continue;
      if (visited.size() > MAX_STACK_OBJECT_USES)
        return false;
      for (const Use &use : value->uses())
        if (!isSafeUse(use, *value, object, worklist))
          return false;
    }
    return true;
  }

  const DataLayout &dl;
  DenseMap<const Value *, bool> cache;
  SmallDenseSet<const Value *> active;
};

// A stack header may be canonicalized only if the object cannot escape and no
// operation can observe its dynamic type. Direct internal callees are checked
// recursively; a nocapture attribute alone is insufficient because the callee
// could still read the header or copy it elsewhere.

AllocaInst *getMatchingStackObject(StoreInst &store, const GlobalVariable &from,
                                   const DataLayout &dl) {
  int64_t offset = 0;
  Value *base =
      GetPointerBaseWithConstantOffset(store.getPointerOperand(), offset, dl);
  auto *object = dyn_cast_or_null<AllocaInst>(base);
  if (!object || offset != 0 || !object->getMetadata("ea_val"))
    return nullptr;
  auto *objectType = dyn_cast<StructType>(object->getAllocatedType());
  if (!objectType || objectType->getNumElements() != OBJECT_STRUCT_FIELD_COUNT)
    return nullptr;

  auto *headerType = dyn_cast<PointerType>(objectType->getElementType(0));
  auto *tiType =
      headerType ? dyn_cast<StructType>(headerType->getElementType()) : nullptr;
  auto *bodyType = dyn_cast<StructType>(objectType->getElementType(1));
  if (!tiType || tiType->getName() != "TypeInfo" || !bodyType)
    return nullptr;
  std::string expectedBody = "ObjLayout.";
  expectedBody += from.getName().drop_back(TYPE_INFO_SUFFIX_SIZE);
  if (bodyType->getName() != expectedBody)
    return nullptr;
  return object;
}

bool canonicalizeStackHeader(StoreInst &store, Module &m,
                             StackObjectSafety &safety) {
  auto *from = getReferencedTypeInfo(store.getValueOperand());
  if (!from)
    return false;
  const DataLayout &dl = m.getDataLayout();
  AllocaInst *object = getMatchingStackObject(store, *from, dl);
  if (!object)
    return false;
  GlobalVariable *to = getCanonicalTypeInfo(*from, m);
  if (!to)
    return false;

  if (!safety.isUnobservable(*object))
    return false;
  store.setOperand(0, bitcastToType(*to, store.getValueOperand()->getType()));
  ++NumStackUses;
  return true;
}

bool canonicalizeTypeInfoUses(Module &m) {
  bool changed = false;
  StackObjectSafety safety(m.getDataLayout());
  for (Function &f : m) {
    if (f.isDeclaration())
      continue;
    for (Instruction &instruction : instructions(f)) {
      if (auto *cb = dyn_cast<CallBase>(&instruction)) {
        changed |= canonicalizeRawArrayAllocation(*cb, m);
        continue;
      }
      if (auto *store = dyn_cast<StoreInst>(&instruction))
        changed |= canonicalizeStackHeader(*store, m, safety);
    }
  }
  return changed;
}

} // namespace

PreservedAnalyses
CJEquivalentInstanceMerge::run(Module &m, ModuleAnalysisManager &am) const {
  if (!EnableCJEquivalentInstanceMerge)
    return PreservedAnalyses::all();
  if (!canonicalizeTypeInfoUses(m))
    return PreservedAnalyses::all();
  if (RunMergeFunctions)
    MergeFunctionsPass().run(m, am);
  return PreservedAnalyses::none();
}
