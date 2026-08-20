//===-- Version.cpp -------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#include "lldb/Version/Version.h"
#include "VCSVersion.inc"
#include "lldb/Version/Version.inc"
#include "clang/Basic/Version.h"

static const char *GetLLDBVersion() {
#ifdef LLDB_FULL_VERSION_STRING
  return LLDB_FULL_VERSION_STRING;
#else
  return "lldb version " LLDB_VERSION_STRING;
#endif
}

static const char *GetLLDBRevision() {
#ifdef LLDB_REVISION
  return LLDB_REVISION;
#else
  return nullptr;
#endif
}

const char *lldb_private::GetVersion() {
  static std::string g_version_str;

  if (g_version_str.empty()) {
    const char *lldb_version = GetLLDBVersion();
    const char *lldb_rev = GetLLDBRevision();
    g_version_str += lldb_version;
    if (lldb_rev) {
      g_version_str += " (";
      g_version_str += "revision ";
      g_version_str += lldb_rev;
      g_version_str += ")";
    }

    std::string clang_rev(clang::getClangRevision());
    if (clang_rev.length() > 0) {
      g_version_str += "\n  clang revision ";
      g_version_str += clang_rev;
    }

    std::string llvm_rev(clang::getLLVMRevision());
    if (llvm_rev.length() > 0) {
      g_version_str += "\n  llvm revision ";
      g_version_str += llvm_rev;
    }
    g_version_str += "\n  cangjie 1.3.0-alpha.02";
  }

  return g_version_str.c_str();
}
