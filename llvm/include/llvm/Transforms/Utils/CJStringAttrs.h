//===- CJStringAttrs.h - cangjie cjstring attribute/name markers -*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Attribute and name markers shared by the cangjie CodeGen that emits the
// cjstring data buffers / literal records and the passes that consume them.
// CJStringPoolMerge keys the merge on these, and GlobalOpt must recognize
// CJSTRING_LITERAL_ATTR to leave the non-constant literal records alone, so
// the spelling lives in one place instead of per-translation-unit string
// literals.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_CJSTRINGATTRS_H
#define LLVM_TRANSFORMS_UTILS_CJSTRINGATTRS_H

namespace llvm {

// Marks a cjstring data buffer: the legacy per-package pool or the pool that
// CJStringPoolMerge emits.
inline constexpr const char *CJSTRING_DATA_ATTR = "cjstring_data";
// Name prefix shared by every cjstring data buffer.
inline constexpr const char *CJSTRING_DATA_PREFIX = "$const_cjstring_data.";
// Marks a per-string buffer emitted by the deferred-pooling CodeGen. Only
// buffers carrying this attribute are mergeable; legacy per-package pools are
// left untouched.
inline constexpr const char *CJSTRING_DEFERRED_ATTR = "cjstring_deferred";
// Marks a cjstring literal record. CodeGen emits such globals as non-constant
// until CJStringPoolMerge relayouts them, so pre-link optimization cannot fold
// their placeholder fields; that pass flips them back to constant once the
// pool layout is final, and GlobalOpt must recognize the attribute to avoid
// folding them before that.
inline constexpr const char *CJSTRING_LITERAL_ATTR = "cjstring_literal";

} // namespace llvm

#endif // LLVM_TRANSFORMS_UTILS_CJSTRINGATTRS_H
