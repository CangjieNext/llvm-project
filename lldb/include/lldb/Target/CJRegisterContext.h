//===-- CJRegisterContext.h -----------------------------------------===//
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

#include "lldb/Target/DynamicRegisterInfo.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/CJDynamicRegisterInfo.h"
#include "lldb/Utility/ArchSpec.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/lldb-private-types.h"
#include "lldb/lldb-enumerations.h"
#include "lldb/lldb-private.h"
#include <vector>

namespace lldb_private {

class CJThread;

class CJRegisterContext : public RegisterContext {
  CJDynamicRegisterInfoSP m_cjreginfo;
  std::vector<uint8_t> m_regdata_snapshot;
  std::shared_ptr<std::map<ConstString, RegisterValue>> m_regvals;
  
public:
  CJRegisterContext(CJThread &thread, CJDynamicRegisterInfoSP cjreginfo,
    std::shared_ptr<std::map<ConstString, RegisterValue>> m_regvals);

  ~CJRegisterContext() override { }

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

  bool
  WriteAllRegisterValues(const RegisterCheckpoint &reg_checkpoint) override;

protected:

  bool InitRegisterValue(const std::map<ConstString, RegisterValue> &regValMap);

  CJRegisterContext(const CJRegisterContext&) = delete;
  const CJRegisterContext &operator=(const CJRegisterContext &) = delete;
};

} // namespace lldb_private

#endif // LLDB_TARGET_CJREGISTERCONTEXT_H