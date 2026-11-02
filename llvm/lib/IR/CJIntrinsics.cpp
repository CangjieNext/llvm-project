//===- CJIntrinsics.cpp - Cangjie Intrinsic Wrappers ------------*- C++ -*-===//
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

#include "llvm/IR/CJIntrinsics.h"

using namespace llvm;
using namespace cangjie;

namespace cangjie {
Value *getBaseObj(const CallBase *CI) {
  switch (CI->getIntrinsicID()) {
  default:
    report_fatal_error("Unsupported intrinsic type");
    return nullptr;
  case Intrinsic::cj_gcwrite_ref:
    return CI->getArgOperand(GCWriteRef::BaseObj);
  case Intrinsic::cj_gcwrite_struct:
    return CI->getArgOperand(GCWriteStruct::BaseObj);
  case Intrinsic::cj_gcread_ref:
    return CI->getArgOperand(GCReadRef::BaseObj);
  case Intrinsic::cj_gcread_struct:
    return CI->getArgOperand(GCReadStruct::BaseObj);
  case Intrinsic::cj_gcwrite_generic:
    return CI->getArgOperand(GCWriteGeneric::BaseObj);
  case Intrinsic::cj_gcread_generic:
    return CI->getArgOperand(GCReadGeneric::BaseObj);
  }
}

Value *getValueArg(const CallBase *CI) {
  switch (CI->getIntrinsicID()) {
  default:
    report_fatal_error("Unsupported intrinsic type");
    return nullptr;
  case Intrinsic::cj_gcwrite_ref:
    return CI->getArgOperand(GCWriteRef::Val);
  case Intrinsic::cj_gcwrite_static_ref:
    return CI->getArgOperand(GCWriteStaticRef::Val);
  }
}

Value *getPointerArg(const CallBase *CI) {
  switch (CI->getIntrinsicID()) {
  default:
    report_fatal_error("Unsupported intrinsic type");
    return nullptr;
  case Intrinsic::cj_gcwrite_ref:
    return CI->getArgOperand(GCWriteRef::FieldPtr);
  case Intrinsic::cj_gcwrite_static_ref:
    return CI->getArgOperand(GCWriteStaticRef::Ptr);
  case Intrinsic::cj_gcread_ref:
    return CI->getArgOperand(GCReadRef::FieldPtr);
  case Intrinsic::cj_gcread_static_ref:
    return CI->getArgOperand(GCReadStaticRef::Ptr);
  }
}

Value *getDest(const CallBase *CI) {
  switch (CI->getIntrinsicID()) {
  default:
    report_fatal_error("Unsupported intrinsic type");
    return nullptr;
  case Intrinsic::cj_gcwrite_struct:
    return CI->getArgOperand(GCWriteStruct::Dst);
  case Intrinsic::cj_gcwrite_static_struct:
    return CI->getArgOperand(GCWriteStaticStruct::Dst);
  case Intrinsic::cj_gcread_struct:
    return CI->getArgOperand(GCReadStruct::Dst);
  case Intrinsic::cj_gcread_static_struct:
    return CI->getArgOperand(GCReadStaticStruct::Dst);
  case Intrinsic::cj_gcwrite_generic:
    return CI->getArgOperand(GCWriteGeneric::DstPtr);
  case Intrinsic::cj_gcread_generic:
    return CI->getArgOperand(GCReadGeneric::DstPtr);
  case Intrinsic::cj_assign_generic:
  case Intrinsic::cj_assign_local_generic:
    return CI->getArgOperand(AssignGeneric::DstPtr);
  case Intrinsic::cj_array_copy_ref:
  case Intrinsic::cj_array_copy_struct:
  case Intrinsic::cj_array_copy_generic:
    return CI->getArgOperand(ArrayCopy::DstPtr);
  }
}

Value *getSource(const CallBase *CI) {
  switch (CI->getIntrinsicID()) {
  default:
    report_fatal_error("Unsupported intrinsic type");
    return nullptr;
  case Intrinsic::cj_gcwrite_struct:
    return CI->getArgOperand(GCWriteStruct::Src);
  case Intrinsic::cj_gcwrite_static_struct:
    return CI->getArgOperand(GCWriteStaticStruct::Src);
  case Intrinsic::cj_gcread_struct:
    return CI->getArgOperand(GCReadStruct::Src);
  case Intrinsic::cj_gcread_static_struct:
    return CI->getArgOperand(GCReadStaticStruct::Src);
  case Intrinsic::cj_gcwrite_generic:
    return CI->getArgOperand(GCWriteGeneric::SrcPtr);
  case Intrinsic::cj_gcread_generic:
    return CI->getArgOperand(GCReadGeneric::SrcPtr);
  case Intrinsic::cj_assign_generic:
  case Intrinsic::cj_assign_local_generic:
    return CI->getArgOperand(AssignGeneric::SrcPtr);
  case Intrinsic::cj_array_copy_ref:
  case Intrinsic::cj_array_copy_struct:
  case Intrinsic::cj_array_copy_generic:
    return CI->getArgOperand(ArrayCopy::SrcPtr);
  }
}

Value *getSize(const CallBase *CI) {
  switch (CI->getIntrinsicID()) {
  default:
    report_fatal_error("Unsupported intrinsic type");
    return nullptr;
  case Intrinsic::cj_gcwrite_struct:
    return CI->getArgOperand(GCWriteStruct::Size);
  case Intrinsic::cj_gcwrite_static_struct:
    return CI->getArgOperand(GCWriteStaticStruct::Size);
  case Intrinsic::cj_gcread_struct:
    return CI->getArgOperand(GCReadStruct::Size);
  case Intrinsic::cj_gcread_static_struct:
    return CI->getArgOperand(GCReadStaticStruct::Size);
  case Intrinsic::cj_gcwrite_generic:
    return CI->getArgOperand(GCWriteGeneric::Size);
  case Intrinsic::cj_gcread_generic:
    return CI->getArgOperand(GCReadGeneric::Size);
  case Intrinsic::cj_array_copy_ref:
  case Intrinsic::cj_array_copy_struct:
  case Intrinsic::cj_array_copy_generic:
    return CI->getArgOperand(ArrayCopy::Size);
  }
}
Value *getAtomicOrder(const CallBase *CI) {
  switch (CI->getIntrinsicID()) {
  default:
    report_fatal_error("Unsupported intrinsic type");
    return nullptr;
  case Intrinsic::cj_atomic_load:
    return CI->getArgOperand(AtomicLoad::Order);
  case Intrinsic::cj_atomic_store:
    return CI->getArgOperand(AtomicStore::Order);
  case Intrinsic::cj_atomic_swap:
    return CI->getArgOperand(AtomicSwap::Order);
  }
}
} // namespace cangjie
