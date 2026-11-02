//===- llvm/IR/CJIntrinsics.h - Cangjie Intrinsic Wrappers ------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file contains utility functions and wrapper class to cangjie intrinsics.
// And define a set of arg-index of the cangjie intrinsic instruction.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_CJINTRINSICS_H
#define LLVM_IR_CJINTRINSICS_H

#include "llvm/IR/IntrinsicInst.h"

using namespace llvm;

namespace cangjie {

struct GCReadRef {
  enum { BaseObj, FieldPtr };
};

struct GCReadStaticRef {
  enum { Ptr };
};

struct GCWriteRef {
  enum { Val, BaseObj, FieldPtr };
};

struct GCWriteStaticRef {
  enum { Val, Ptr };
};

struct GCReadStruct {
  enum { Dst, BaseObj, Src, Size };
};

struct GCWriteStruct {
  enum { BaseObj, Dst, Src, Size };
};

struct GCReadStaticStruct {
  enum { Dst, Src, Size };
};

struct GCWriteStaticStruct {
  enum { Dst, Src, Size };
};

struct GCReadGeneric {
  enum { DstPtr, BaseObj, SrcPtr, Size };
};

struct GCWriteGeneric {
  enum { BaseObj, DstPtr, SrcPtr, Size };
};

struct AssignGeneric {
  enum { DstPtr, SrcPtr, TypeInfo };
};

struct AssignLocalGeneric {
  enum { DstPtr, SrcPtr, TypeInfo };
};

struct ArrayCopy {
  enum { DstObj, DstPtr, SrcObj, SrcPtr, Size };
};

struct AtomicLoad {
  enum { Obj, Field, Order };
};

struct AtomicStore {
  enum { Ref, Obj, Field, Order };
};

struct AtomicSwap {
  enum { Ref, Obj, Field, Order };
};

struct AtomicCompareSwap {
  enum { OldRef, NewRef, Obj, Field, SuccOrder, FailOrder };
};

Value *getBaseObj(const CallBase *CI);
Value *getValueArg(const CallBase *CI);
Value *getPointerArg(const CallBase *CI);
Value *getDest(const CallBase *CI);
Value *getSource(const CallBase *CI);
Value *getSize(const CallBase *CI);
Value *getAtomicOrder(const CallBase *CI);
} // namespace cangjie

#endif
