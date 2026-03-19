#include "lldb/Target/CJRegisterContext.h"
#include "lldb/Core/ValueObject.h"
#include "lldb/Target/CJThread.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Utility/DataBufferHeap.h"
#include "lldb/Utility/RegisterValue.h"
#include "lldb/lldb-private-types.h"

#include <assert.h>

// 1. compiler save caller saved registers, including rdi
// 2. invoke MCall
// 3. save context
// 4. switch to next

using namespace lldb_private;

#define CJ_ASSERT assert

struct CJContextRegAccessInfo {
  llvm::StringRef target_regname;
  unsigned offset, byte_size;
};

CJRegisterContext::CJRegisterContext(
    CJThread &thread, CJDynamicRegisterInfoSP cjreginfo, 
    std::shared_ptr<std::map<ConstString, RegisterValue>> m_regvals)
    : RegisterContext(thread, 0), m_cjreginfo(cjreginfo), m_regvals(m_regvals) {
  InitRegisterValue(*m_regvals);
}

void CJRegisterContext::InvalidateAllRegisters() {
}

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

bool CJRegisterContext::InitRegisterValue(const std::map<ConstString, RegisterValue> &regValMap) {
  for (auto &regValPair : regValMap) {
    ConstString name = regValPair.first;
    const RegisterInfo *reg_info = m_cjreginfo->GetRegisterInfo(name.GetStringRef());
    if (!reg_info) continue;
    if (m_regdata_snapshot.size() < reg_info->byte_offset + reg_info->byte_size) {
      m_regdata_snapshot.resize(reg_info->byte_offset + reg_info->byte_size);
    }

    const RegisterValue &regVal = regValPair.second;
    if (reg_info->byte_size == regVal.GetByteSize())
      memcpy(&m_regdata_snapshot[reg_info->byte_offset],
        regVal.GetBytes(), regVal.GetByteSize());
  }

  return true;
}

bool CJRegisterContext::ReadRegister(const RegisterInfo *reg_info,
                                     RegisterValue &value) {
  // __builtin_printf("read register %s\n", reg_info->name);
  // DumpValueObject(m_cjt_ctx, 0);
  // llvm::Triple::ArchType arch_type = GetThread().GetProcess()->GetSystemArchitecture().GetTriple().getArch();

  value.SetBytes(&m_regdata_snapshot[reg_info->byte_offset], reg_info->byte_size,
                 lldb_private::endian::InlHostByteOrder());
  return true;
}

bool CJRegisterContext::WriteRegister(const RegisterInfo *reg_info,
                                      const RegisterValue &value) {
  /* This is just a snapshot, can't perform write.
   * So, implement write in the future if needed.
   */
  __builtin_printf("try write register %s %lx\n", reg_info->name, value.GetAsUInt64());

  memcpy(&m_regdata_snapshot[reg_info->byte_offset], value.GetBytes(),
    std::min<unsigned>(m_regdata_snapshot.size(), value.GetByteSize()));
  return true;
}

bool CJRegisterContext::ReadAllRegisterValues(
    lldb::WritableDataBufferSP &data_sp) {
  // std::vector<uint8_t> data_buf;
  // data_buf.resize(m_all_register_data_size);

  auto data_buf =
      std::make_shared<DataBufferHeap>(m_regdata_snapshot.size(), '\0');

  uint8_t *target_buf = data_buf->GetBytes();
  // unsigned bufsz = data_buf->GetByteSize();
  memcpy(&target_buf[0], &m_regdata_snapshot[0],
    std::min<unsigned>(data_buf->GetByteSize(), m_regdata_snapshot.size()));

  data_sp = data_buf;
  return true;
}

bool CJRegisterContext::ReadAllRegisterValues(
    RegisterCheckpoint &reg_checkpoint) {
  lldb::WritableDataBufferSP &data_sp = reg_checkpoint.GetData();
  return ReadAllRegisterValues(data_sp);
}

bool CJRegisterContext::WriteAllRegisterValues(
    const lldb::DataBufferSP &data_sp) {
  // CJ_ASSERT(false && "unimplemented");
  return false;
}

bool CJRegisterContext::WriteAllRegisterValues(
    const RegisterCheckpoint &reg_checkpoint) {
  // CJ_ASSERT(false && "unimplemented");
  return false;
}
