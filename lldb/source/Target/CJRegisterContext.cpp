//===-- CJRegisterContext.cpp ---------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/CJRegisterContext.h"

#include "lldb/Target/CJThread.h"
#include "lldb/Utility/DataBufferHeap.h"
#include "lldb/Utility/RegisterValue.h"

#include "Plugins/Process/Utility/lldb-arm64-register-enums.h"
#include "Plugins/Process/Utility/lldb-x86-register-enums.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace lldb;
using namespace lldb_private;

namespace {

struct RegisterKeyMapping {
  uint32_t regnum;
  llvm::StringRef key;
};

struct RegisterSliceMapping {
  uint32_t regnum;
  llvm::StringRef base_reg;
  uint32_t base_reg_size;
  uint32_t slice_offset;
  uint32_t slice_size;
};

struct RegisterVectorLow64Mapping {
  uint32_t regnum;
  llvm::StringRef low64_reg;
};

llvm::StringRef GetGenericRegisterAlias(uint32_t generic_reg) {
  switch (generic_reg) {
  case LLDB_REGNUM_GENERIC_PC:
    return "pc";
  case LLDB_REGNUM_GENERIC_SP:
    return "sp";
  case LLDB_REGNUM_GENERIC_FP:
    return "fp";
  case LLDB_REGNUM_GENERIC_RA:
    return "ra";
  case LLDB_REGNUM_GENERIC_FLAGS:
    return "flags";
  default:
    return {};
  }
}

uint64_t DecodeUnsignedBytes(llvm::ArrayRef<uint8_t> bytes,
                             lldb::ByteOrder byte_order) {
  uint64_t value = 0;
  const size_t n = std::min<size_t>(bytes.size(), sizeof(uint64_t));

  if (byte_order == eByteOrderBig) {
    for (size_t i = 0; i < n; ++i)
      value = (value << 8) | bytes[i];
  } else {
    for (size_t i = 0; i < n; ++i)
      value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
  }
  return value;
}

const RegisterKeyMapping *FindDirectMapping(
    llvm::ArrayRef<RegisterKeyMapping> mappings, uint32_t regnum) {
  for (const RegisterKeyMapping &mapping : mappings) {
    if (mapping.regnum == regnum)
      return &mapping;
  }
  return nullptr;
}

const RegisterSliceMapping *FindSliceMapping(
    llvm::ArrayRef<RegisterSliceMapping> mappings, uint32_t regnum) {
  for (const RegisterSliceMapping &mapping : mappings) {
    if (mapping.regnum == regnum)
      return &mapping;
  }
  return nullptr;
}

const RegisterVectorLow64Mapping *FindVectorLow64Mapping(
    llvm::ArrayRef<RegisterVectorLow64Mapping> mappings, uint32_t regnum) {
  for (const RegisterVectorLow64Mapping &mapping : mappings) {
    if (mapping.regnum == regnum)
      return &mapping;
  }
  return nullptr;
}

constexpr RegisterKeyMapping kWindowsX64DirectMappings[] = {
    {lldb_rbx_x86_64, "rbx"},       {lldb_rdi_x86_64, "rdi"},
    {lldb_rsi_x86_64, "rsi"},       {lldb_rbp_x86_64, "rbp"},
    {lldb_rsp_x86_64, "rsp"},       {lldb_rip_x86_64, "rip"},
    {lldb_r12_x86_64, "r12"},       {lldb_r13_x86_64, "r13"},
    {lldb_r14_x86_64, "r14"},       {lldb_r15_x86_64, "r15"},
    {lldb_mxcsr_x86_64, "mxcsr"},   {lldb_fctrl_x86_64, "fpucw"},
    {lldb_xmm6_x86_64, "xmm6"},     {lldb_xmm7_x86_64, "xmm7"},
    {lldb_xmm8_x86_64, "xmm8"},     {lldb_xmm9_x86_64, "xmm9"},
    {lldb_xmm10_x86_64, "xmm10"},   {lldb_xmm11_x86_64, "xmm11"},
    {lldb_xmm12_x86_64, "xmm12"},   {lldb_xmm13_x86_64, "xmm13"},
    {lldb_xmm14_x86_64, "xmm14"},   {lldb_xmm15_x86_64, "xmm15"},
};

constexpr RegisterSliceMapping kWindowsX64SliceMappings[] = {
    {lldb_ebx_x86_64, "rbx", 8, 0, 4},   {lldb_edi_x86_64, "rdi", 8, 0, 4},
    {lldb_esi_x86_64, "rsi", 8, 0, 4},   {lldb_ebp_x86_64, "rbp", 8, 0, 4},
    {lldb_esp_x86_64, "rsp", 8, 0, 4},   {lldb_r12d_x86_64, "r12", 8, 0, 4},
    {lldb_r13d_x86_64, "r13", 8, 0, 4},  {lldb_r14d_x86_64, "r14", 8, 0, 4},
    {lldb_r15d_x86_64, "r15", 8, 0, 4},  {lldb_bx_x86_64, "rbx", 8, 0, 2},
    {lldb_di_x86_64, "rdi", 8, 0, 2},    {lldb_si_x86_64, "rsi", 8, 0, 2},
    {lldb_bp_x86_64, "rbp", 8, 0, 2},    {lldb_sp_x86_64, "rsp", 8, 0, 2},
    {lldb_r12w_x86_64, "r12", 8, 0, 2},  {lldb_r13w_x86_64, "r13", 8, 0, 2},
    {lldb_r14w_x86_64, "r14", 8, 0, 2},  {lldb_r15w_x86_64, "r15", 8, 0, 2},
    {lldb_bh_x86_64, "rbx", 8, 1, 1},    {lldb_bl_x86_64, "rbx", 8, 0, 1},
    {lldb_dil_x86_64, "rdi", 8, 0, 1},   {lldb_sil_x86_64, "rsi", 8, 0, 1},
    {lldb_bpl_x86_64, "rbp", 8, 0, 1},   {lldb_spl_x86_64, "rsp", 8, 0, 1},
    {lldb_r12l_x86_64, "r12", 8, 0, 1},  {lldb_r13l_x86_64, "r13", 8, 0, 1},
    {lldb_r14l_x86_64, "r14", 8, 0, 1},  {lldb_r15l_x86_64, "r15", 8, 0, 1},
};

constexpr RegisterKeyMapping kLinuxX64DirectMappings[] = {
    {lldb_rbx_x86_64, "rbx"},     {lldb_rbp_x86_64, "rbp"},
    {lldb_rsp_x86_64, "rsp"},     {lldb_rip_x86_64, "rip"},
    {lldb_r12_x86_64, "r12"},     {lldb_r13_x86_64, "r13"},
    {lldb_r14_x86_64, "r14"},     {lldb_r15_x86_64, "r15"},
    {lldb_mxcsr_x86_64, "mxcsr"}, {lldb_fctrl_x86_64, "fpucw"},
};

constexpr RegisterSliceMapping kLinuxX64SliceMappings[] = {
    {lldb_ebx_x86_64, "rbx", 8, 0, 4},   {lldb_ebp_x86_64, "rbp", 8, 0, 4},
    {lldb_esp_x86_64, "rsp", 8, 0, 4},   {lldb_r12d_x86_64, "r12", 8, 0, 4},
    {lldb_r13d_x86_64, "r13", 8, 0, 4},  {lldb_r14d_x86_64, "r14", 8, 0, 4},
    {lldb_r15d_x86_64, "r15", 8, 0, 4},  {lldb_bx_x86_64, "rbx", 8, 0, 2},
    {lldb_bp_x86_64, "rbp", 8, 0, 2},    {lldb_sp_x86_64, "rsp", 8, 0, 2},
    {lldb_r12w_x86_64, "r12", 8, 0, 2},  {lldb_r13w_x86_64, "r13", 8, 0, 2},
    {lldb_r14w_x86_64, "r14", 8, 0, 2},  {lldb_r15w_x86_64, "r15", 8, 0, 2},
    {lldb_bh_x86_64, "rbx", 8, 1, 1},    {lldb_bl_x86_64, "rbx", 8, 0, 1},
    {lldb_bpl_x86_64, "rbp", 8, 0, 1},   {lldb_spl_x86_64, "rsp", 8, 0, 1},
    {lldb_r12l_x86_64, "r12", 8, 0, 1},  {lldb_r13l_x86_64, "r13", 8, 0, 1},
    {lldb_r14l_x86_64, "r14", 8, 0, 1},  {lldb_r15l_x86_64, "r15", 8, 0, 1},
};

constexpr RegisterKeyMapping kArm64DirectMappings[] = {
    {gpr_x18_arm64, "x18"},    {gpr_x19_arm64, "x19"},
    {gpr_x20_arm64, "x20"},    {gpr_x21_arm64, "x21"},
    {gpr_x22_arm64, "x22"},    {gpr_x23_arm64, "x23"},
    {gpr_x24_arm64, "x24"},    {gpr_x25_arm64, "x25"},
    {gpr_x26_arm64, "x26"},    {gpr_x27_arm64, "x27"},
    {gpr_x28_arm64, "x28"},    {gpr_fp_arm64, "fp"},
    {gpr_lr_arm64, "lr"},      {gpr_sp_arm64, "sp"},
    {gpr_pc_arm64, "pc"},      {fpu_d8_arm64, "d8"},
    {fpu_d9_arm64, "d9"},      {fpu_d10_arm64, "d10"},
    {fpu_d11_arm64, "d11"},    {fpu_d12_arm64, "d12"},
    {fpu_d13_arm64, "d13"},    {fpu_d14_arm64, "d14"},
    {fpu_d15_arm64, "d15"},    {fpu_fpcr_arm64, "fpcr"},
};

constexpr RegisterSliceMapping kArm64SliceMappings[] = {
    {gpr_w18_arm64, "x18", 8, 0, 4},   {gpr_w19_arm64, "x19", 8, 0, 4},
    {gpr_w20_arm64, "x20", 8, 0, 4},   {gpr_w21_arm64, "x21", 8, 0, 4},
    {gpr_w22_arm64, "x22", 8, 0, 4},   {gpr_w23_arm64, "x23", 8, 0, 4},
    {gpr_w24_arm64, "x24", 8, 0, 4},   {gpr_w25_arm64, "x25", 8, 0, 4},
    {gpr_w26_arm64, "x26", 8, 0, 4},   {gpr_w27_arm64, "x27", 8, 0, 4},
    {gpr_w28_arm64, "x28", 8, 0, 4},   {fpu_s8_arm64, "d8", 8, 0, 4},
    {fpu_s9_arm64, "d9", 8, 0, 4},     {fpu_s10_arm64, "d10", 8, 0, 4},
    {fpu_s11_arm64, "d11", 8, 0, 4},   {fpu_s12_arm64, "d12", 8, 0, 4},
    {fpu_s13_arm64, "d13", 8, 0, 4},   {fpu_s14_arm64, "d14", 8, 0, 4},
    {fpu_s15_arm64, "d15", 8, 0, 4},
};

constexpr RegisterVectorLow64Mapping kArm64VectorMappings[] = {
    {fpu_v8_arm64, "d8"},   {fpu_v9_arm64, "d9"},   {fpu_v10_arm64, "d10"},
    {fpu_v11_arm64, "d11"}, {fpu_v12_arm64, "d12"}, {fpu_v13_arm64, "d13"},
    {fpu_v14_arm64, "d14"}, {fpu_v15_arm64, "d15"},
};

} // namespace

CJRegisterContext::CJRegisterContext(
    CJThread &thread, CJDynamicRegisterInfoSP cjreginfo,
    std::shared_ptr<std::map<ConstString, RegisterValue>> regvals)
    : RegisterContext(thread, 0), m_cjreginfo(cjreginfo),
      m_regvals(std::move(regvals)) {
  InitRegisterValue(*m_regvals);
}

void CJRegisterContext::InvalidateAllRegisters() {}

size_t CJRegisterContext::GetRegisterCount() {
  return m_cjreginfo->GetNumRegisters();
}

const RegisterInfo *CJRegisterContext::GetRegisterInfoAtIndex(size_t reg) {
  return m_cjreginfo->GetRegisterInfoAtIndex(reg);
}

size_t CJRegisterContext::GetRegisterSetCount() {
  return m_cjreginfo->GetNumRegisterSets();
}

const RegisterSet *CJRegisterContext::GetRegisterSet(size_t reg_set) {
  return m_cjreginfo->GetRegisterSet(reg_set);
}

lldb::ByteOrder CJRegisterContext::GetByteOrderForValues() const {
  ProcessSP process_sp = m_thread.GetProcess();
  if (process_sp)
    return process_sp->GetByteOrder();
  return endian::InlHostByteOrder();
}

uint32_t CJRegisterContext::GetLLDBRegisterNumber(
    const RegisterInfo *reg_info) const {
  if (!reg_info)
    return LLDB_INVALID_REGNUM;
  return reg_info->kinds[eRegisterKindLLDB];
}

void CJRegisterContext::SetZeroRegisterValue(const RegisterInfo *reg_info,
                                             RegisterValue &value) const {
  if (!reg_info)
    return;

  if ((reg_info->encoding == eEncodingUint || reg_info->encoding == eEncodingSint) &&
      reg_info->byte_size <= sizeof(uint64_t)) {
    value.SetUInt(0, reg_info->byte_size);
    return;
  }

  std::vector<uint8_t> zero(reg_info->byte_size, 0);
  value.SetBytes(zero.data(), zero.size(), GetByteOrderForValues());
}

bool CJRegisterContext::GetStoredRegisterValue(llvm::StringRef reg_name,
                                               RegisterValue &value) const {
  if (reg_name.empty())
    return false;

  auto it = m_resolved_regvals.find(ConstString(reg_name));
  if (it == m_resolved_regvals.end())
    return false;

  value = it->second;
  return true;
}

bool CJRegisterContext::StoreRegisterValue(llvm::StringRef reg_name,
                                           const RegisterValue &value) {
  if (reg_name.empty())
    return false;

  ConstString key(reg_name);
  m_resolved_regvals[key] = value;
  if (m_regvals)
    (*m_regvals)[key] = value;
  return true;
}

bool CJRegisterContext::InitRegisterValue(
    const std::map<ConstString, RegisterValue> &regValMap) {
  m_resolved_regvals = regValMap;
  return true;
}

bool CJRegisterContext::GetRegisterBytes(const RegisterValue &value,
                                         uint32_t value_size,
                                         std::vector<uint8_t> &bytes) const {
  if (const void *raw = value.GetBytes()) {
    const uint8_t *src = static_cast<const uint8_t *>(raw);
    bytes.assign(src, src + value.GetByteSize());
    return true;
  }

  if (value_size == 0 || value_size > sizeof(uint64_t))
    return false;

  bool success = false;
  const uint64_t scalar = value.GetAsUInt64(0, &success);
  if (!success)
    return false;

  bytes.resize(value_size);
  if (GetByteOrderForValues() == eByteOrderBig) {
    for (uint32_t i = 0; i < value_size; ++i)
      bytes[value_size - 1 - i] = static_cast<uint8_t>(scalar >> (i * 8));
  } else {
    for (uint32_t i = 0; i < value_size; ++i)
      bytes[i] = static_cast<uint8_t>(scalar >> (i * 8));
  }
  return true;
}

bool CJRegisterContext::SetRegisterValueBytes(const RegisterInfo *reg_info,
                                              llvm::ArrayRef<uint8_t> bytes,
                                              RegisterValue &value) const {
  if (!reg_info || bytes.size() < reg_info->byte_size)
    return false;

  if ((reg_info->encoding == eEncodingUint || reg_info->encoding == eEncodingSint) &&
      reg_info->byte_size <= sizeof(uint64_t)) {
    value.SetUInt(DecodeUnsignedBytes(bytes.take_front(reg_info->byte_size),
                                      GetByteOrderForValues()),
                  reg_info->byte_size);
    return true;
  }

  value.SetBytes(bytes.data(), reg_info->byte_size, GetByteOrderForValues());
  return true;
}

bool CJRegisterContext::ReadSubRegister(llvm::StringRef base_reg_name,
                                        uint32_t base_reg_size,
                                        uint32_t slice_offset,
                                        uint32_t slice_size,
                                        const RegisterInfo *reg_info,
                                        RegisterValue &value) const {
  RegisterValue base_value;
  if (!GetStoredRegisterValue(base_reg_name, base_value))
    return false;

  std::vector<uint8_t> base_bytes;
  if (!GetRegisterBytes(base_value, base_reg_size, base_bytes))
    return false;
  if (slice_offset + slice_size > base_bytes.size())
    return false;

  return SetRegisterValueBytes(
      reg_info,
      llvm::ArrayRef<uint8_t>(base_bytes).slice(slice_offset, slice_size),
      value);
}

bool CJRegisterContext::WriteSubRegister(llvm::StringRef base_reg_name,
                                         uint32_t base_reg_size,
                                         uint32_t slice_offset,
                                         const RegisterValue &value) {
  RegisterValue base_value;
  if (!GetStoredRegisterValue(base_reg_name, base_value))
    base_value.SetUInt(0, base_reg_size);

  std::vector<uint8_t> base_bytes;
  std::vector<uint8_t> src_bytes;
  if (!GetRegisterBytes(base_value, base_reg_size, base_bytes) ||
      !GetRegisterBytes(value, value.GetByteSize(), src_bytes))
    return false;

  if (base_bytes.size() < base_reg_size)
    base_bytes.resize(base_reg_size, 0);
  if (slice_offset + src_bytes.size() > base_bytes.size())
    return false;

  std::copy(src_bytes.begin(), src_bytes.end(), base_bytes.begin() + slice_offset);

  RegisterValue merged;
  if (base_reg_size <= sizeof(uint64_t))
    merged.SetUInt(DecodeUnsignedBytes(base_bytes, GetByteOrderForValues()),
                   base_reg_size);
  else
    merged.SetBytes(base_bytes.data(), base_bytes.size(), GetByteOrderForValues());

  return StoreRegisterValue(base_reg_name, merged);
}

bool CJRegisterContext::ReadRegisterWindowsX64(const RegisterInfo *reg_info,
                                               RegisterValue &value) const {
  const uint32_t reg = GetLLDBRegisterNumber(reg_info);

  if (const RegisterKeyMapping *mapping =
          FindDirectMapping(kWindowsX64DirectMappings, reg))
    return GetStoredRegisterValue(mapping->key, value);

  if (const RegisterSliceMapping *mapping =
          FindSliceMapping(kWindowsX64SliceMappings, reg))
    return ReadSubRegister(mapping->base_reg, mapping->base_reg_size,
                           mapping->slice_offset, mapping->slice_size, reg_info,
                           value);

  return false;
}

bool CJRegisterContext::WriteRegisterWindowsX64(const RegisterInfo *reg_info,
                                                const RegisterValue &value) {
  const uint32_t reg = GetLLDBRegisterNumber(reg_info);

  if (const RegisterKeyMapping *mapping =
          FindDirectMapping(kWindowsX64DirectMappings, reg))
    return StoreRegisterValue(mapping->key, value);

  if (const RegisterSliceMapping *mapping =
          FindSliceMapping(kWindowsX64SliceMappings, reg))
    return WriteSubRegister(mapping->base_reg, mapping->base_reg_size,
                            mapping->slice_offset, value);

  return false;
}

bool CJRegisterContext::ReadRegisterX64(const RegisterInfo *reg_info,
                                        RegisterValue &value) const {
  const uint32_t reg = GetLLDBRegisterNumber(reg_info);

  if (const RegisterKeyMapping *mapping =
          FindDirectMapping(kLinuxX64DirectMappings, reg))
    return GetStoredRegisterValue(mapping->key, value);

  if (const RegisterSliceMapping *mapping =
          FindSliceMapping(kLinuxX64SliceMappings, reg))
    return ReadSubRegister(mapping->base_reg, mapping->base_reg_size,
                           mapping->slice_offset, mapping->slice_size, reg_info,
                           value);

  return false;
}

bool CJRegisterContext::WriteRegisterX64(const RegisterInfo *reg_info,
                                         const RegisterValue &value) {
  const uint32_t reg = GetLLDBRegisterNumber(reg_info);

  if (const RegisterKeyMapping *mapping =
          FindDirectMapping(kLinuxX64DirectMappings, reg))
    return StoreRegisterValue(mapping->key, value);

  if (const RegisterSliceMapping *mapping =
          FindSliceMapping(kLinuxX64SliceMappings, reg))
    return WriteSubRegister(mapping->base_reg, mapping->base_reg_size,
                            mapping->slice_offset, value);

  return false;
}

bool CJRegisterContext::ReadRegisterArm64(const RegisterInfo *reg_info,
                                          RegisterValue &value) const {
  const uint32_t reg = GetLLDBRegisterNumber(reg_info);

  if (const RegisterKeyMapping *mapping =
          FindDirectMapping(kArm64DirectMappings, reg))
    return GetStoredRegisterValue(mapping->key, value);

  if (const RegisterSliceMapping *mapping =
          FindSliceMapping(kArm64SliceMappings, reg))
    return ReadSubRegister(mapping->base_reg, mapping->base_reg_size,
                           mapping->slice_offset, mapping->slice_size, reg_info,
                           value);

  if (const RegisterVectorLow64Mapping *mapping =
          FindVectorLow64Mapping(kArm64VectorMappings, reg)) {
    RegisterValue low64_value;
    if (!GetStoredRegisterValue(mapping->low64_reg, low64_value))
      return false;

    std::vector<uint8_t> bytes;
    if (!GetRegisterBytes(low64_value, 8, bytes))
      return false;
    bytes.resize(reg_info->byte_size, 0);
    value.SetBytes(bytes.data(), bytes.size(), GetByteOrderForValues());
    return true;
  }

  return false;
}

bool CJRegisterContext::WriteRegisterArm64(const RegisterInfo *reg_info,
                                           const RegisterValue &value) {
  const uint32_t reg = GetLLDBRegisterNumber(reg_info);

  if (const RegisterKeyMapping *mapping =
          FindDirectMapping(kArm64DirectMappings, reg))
    return StoreRegisterValue(mapping->key, value);

  if (const RegisterSliceMapping *mapping =
          FindSliceMapping(kArm64SliceMappings, reg))
    return WriteSubRegister(mapping->base_reg, mapping->base_reg_size,
                            mapping->slice_offset, value);

  if (const RegisterVectorLow64Mapping *mapping =
          FindVectorLow64Mapping(kArm64VectorMappings, reg)) {
    std::vector<uint8_t> bytes;
    if (!GetRegisterBytes(value, reg_info->byte_size, bytes) || bytes.size() < 8)
      return false;

    RegisterValue low64_value;
    low64_value.SetBytes(bytes.data(), 8, GetByteOrderForValues());
    return StoreRegisterValue(mapping->low64_reg, low64_value);
  }

  return false;
}

bool CJRegisterContext::ReadRegister(const RegisterInfo *reg_info,
                                     RegisterValue &value) {
  if (!reg_info) {
    value.SetValueToInvalid();
    return false;
  }

  ProcessSP process_sp = GetThread().GetProcess();
  if (!process_sp) {
    value.SetValueToInvalid();
    return false;
  }

  const llvm::Triple triple = process_sp->GetSystemArchitecture().GetTriple();

  bool ok = false;
  if (triple.getArch() == llvm::Triple::ArchType::x86_64 &&
      triple.getOS() == llvm::Triple::OSType::Win32) {
    ok = ReadRegisterWindowsX64(reg_info, value);
  } else if (triple.getArch() == llvm::Triple::ArchType::x86_64) {
    ok = ReadRegisterX64(reg_info, value);
  } else if (triple.getArch() == llvm::Triple::ArchType::aarch64) {
    ok = ReadRegisterArm64(reg_info, value);
  } else {
    ok = GetStoredRegisterValue(reg_info->name, value) ||
         (reg_info->alt_name && GetStoredRegisterValue(reg_info->alt_name, value));
  }

  if (!ok)
    SetZeroRegisterValue(reg_info, value);
  return true;
}

bool CJRegisterContext::WriteRegister(const RegisterInfo *reg_info,
                                      const RegisterValue &value) {
  if (!reg_info)
    return false;

  ProcessSP process_sp = GetThread().GetProcess();
  if (!process_sp)
    return false;
  const llvm::Triple triple = process_sp->GetSystemArchitecture().GetTriple();

  bool ok = false;
  if (triple.getArch() == llvm::Triple::ArchType::x86_64 &&
      triple.getOS() == llvm::Triple::OSType::Win32) {
    ok = WriteRegisterWindowsX64(reg_info, value);
  } else if (triple.getArch() == llvm::Triple::ArchType::x86_64) {
    ok = WriteRegisterX64(reg_info, value);
  } else if (triple.getArch() == llvm::Triple::ArchType::aarch64) {
    ok = WriteRegisterArm64(reg_info, value);
  }

  if (!ok && triple.getArch() != llvm::Triple::ArchType::x86_64 &&
      triple.getArch() != llvm::Triple::ArchType::aarch64) {
    if (reg_info->name)
      StoreRegisterValue(reg_info->name, value);
    if (reg_info->alt_name)
      StoreRegisterValue(reg_info->alt_name, value);
    ok = true;
  }

  if (!ok)
    return false;
  llvm::StringRef alias =
      GetGenericRegisterAlias(reg_info->kinds[eRegisterKindGeneric]);
  if (!alias.empty())
    StoreRegisterValue(alias, value);
  return true;
}

bool CJRegisterContext::ReadAllRegisterValues(
    lldb::WritableDataBufferSP &data_sp) {
  std::vector<uint8_t> bytes;

  for (size_t i = 0; i < GetRegisterCount(); ++i) {
    const RegisterInfo *reg_info = GetRegisterInfoAtIndex(i);
    if (!reg_info)
      continue;

    RegisterValue value;
    ReadRegister(reg_info, value);

    std::vector<uint8_t> reg_bytes;
    if (!GetRegisterBytes(value, reg_info->byte_size, reg_bytes)) {
      reg_bytes.assign(reg_info->byte_size, 0);
    } else if (reg_bytes.size() < reg_info->byte_size) {
      reg_bytes.resize(reg_info->byte_size, 0);
    }

    bytes.insert(bytes.end(), reg_bytes.begin(),
                 reg_bytes.begin() + reg_info->byte_size);
  }

  data_sp = std::make_shared<DataBufferHeap>(bytes.data(), bytes.size());
  return true;
}

bool CJRegisterContext::ReadAllRegisterValues(
    RegisterCheckpoint &reg_checkpoint) {
  lldb::WritableDataBufferSP &data_sp = reg_checkpoint.GetData();
  return ReadAllRegisterValues(data_sp);
}

bool CJRegisterContext::WriteAllRegisterValues(
    const lldb::DataBufferSP &data_sp) {
  if (!data_sp)
    return false;

  const uint8_t *data = data_sp->GetBytes();
  if (!data)
    return false;

  size_t offset = 0;
  for (size_t i = 0; i < GetRegisterCount(); ++i) {
    const RegisterInfo *reg_info = GetRegisterInfoAtIndex(i);
    if (!reg_info)
      continue;

    if (offset + reg_info->byte_size > data_sp->GetByteSize())
      return false;

    RegisterValue new_value;
    if (!SetRegisterValueBytes(
            reg_info,
            llvm::ArrayRef<uint8_t>(data + offset, reg_info->byte_size),
            new_value))
      return false;

    WriteRegister(reg_info, new_value);
    offset += reg_info->byte_size;
  }

  return offset == data_sp->GetByteSize();
}

bool CJRegisterContext::WriteAllRegisterValues(
    const RegisterCheckpoint &reg_checkpoint) {
  return WriteAllRegisterValues(reg_checkpoint.GetData());
}
