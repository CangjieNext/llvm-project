//===-- CJRegisterContext.h ---------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_TARGET_CJREGISTERCONTEXT_H
#define LLDB_TARGET_CJREGISTERCONTEXT_H

#include <map>
#include <vector>

#include "lldb/Target/CJDynamicRegisterInfo.h"
#include "lldb/Target/DynamicRegisterInfo.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/lldb-private-types.h"
#include "lldb/lldb-private.h"

namespace lldb_private {

class CJThread;

class CJRegisterContext : public RegisterContext {
  CJDynamicRegisterInfoSP m_cjreginfo;
  std::shared_ptr<std::map<ConstString, RegisterValue>> m_regvals;
  std::map<ConstString, RegisterValue> m_resolved_regvals;

public:
  CJRegisterContext(CJThread &thread, CJDynamicRegisterInfoSP cjreginfo,
                    std::shared_ptr<std::map<ConstString, RegisterValue>> m_regvals);

  ~CJRegisterContext() override {}

  void InvalidateAllRegisters() override;

  size_t GetRegisterCount() override;

  const RegisterInfo *GetRegisterInfoAtIndex(size_t reg) override;

  size_t GetRegisterSetCount() override;

  const RegisterSet *GetRegisterSet(size_t reg_set) override;

  bool ReadRegister(const RegisterInfo *reg_info,
                    RegisterValue &value) override;

  bool WriteRegister(const RegisterInfo *reg_info,
                     const RegisterValue &value) override;

  bool ReadAllRegisterValues(lldb::WritableDataBufferSP &data_sp) override;

  bool WriteAllRegisterValues(const lldb::DataBufferSP &data_sp) override;

  bool ReadAllRegisterValues(RegisterCheckpoint &reg_checkpoint) override;

  bool WriteAllRegisterValues(const RegisterCheckpoint &reg_checkpoint) override;

protected:
  bool InitRegisterValue(const std::map<ConstString, RegisterValue> &regValMap);
  uint32_t GetLLDBRegisterNumber(const RegisterInfo *reg_info) const;
  bool GetStoredRegisterValue(llvm::StringRef reg_name,
                              RegisterValue &value) const;
  bool StoreRegisterValue(llvm::StringRef reg_name,
                          const RegisterValue &value);
  bool GetRegisterBytes(const RegisterValue &value, uint32_t value_size,
                        std::vector<uint8_t> &bytes) const;
  bool SetRegisterValueBytes(const RegisterInfo *reg_info,
                             llvm::ArrayRef<uint8_t> bytes,
                             RegisterValue &value) const;
  bool ReadSubRegister(llvm::StringRef base_reg_name, uint32_t base_reg_size,
                       uint32_t slice_offset, uint32_t slice_size,
                       const RegisterInfo *reg_info,
                       RegisterValue &value) const;
  bool WriteSubRegister(llvm::StringRef base_reg_name, uint32_t base_reg_size,
                        uint32_t slice_offset, const RegisterValue &value);
  bool ReadRegisterWindowsX64(const RegisterInfo *reg_info,
                              RegisterValue &value) const;
  bool WriteRegisterWindowsX64(const RegisterInfo *reg_info,
                               const RegisterValue &value);
  bool ReadRegisterX64(const RegisterInfo *reg_info,
                       RegisterValue &value) const;
  bool WriteRegisterX64(const RegisterInfo *reg_info,
                        const RegisterValue &value);
  bool ReadRegisterArm64(const RegisterInfo *reg_info,
                         RegisterValue &value) const;
  bool WriteRegisterArm64(const RegisterInfo *reg_info,
                          const RegisterValue &value);
  void SetZeroRegisterValue(const RegisterInfo *reg_info,
                            RegisterValue &value) const;
  lldb::ByteOrder GetByteOrderForValues() const;
};

} // namespace lldb_private

#endif // LLDB_TARGET_CJREGISTERCONTEXT_H
