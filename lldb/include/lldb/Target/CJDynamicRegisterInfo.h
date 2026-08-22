//===-- CJDynamicRegisterInfo.h -----------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_TARGET_CJDYNAMICREGISTERINFO_H
#define LLDB_TARGET_CJDYNAMICREGISTERINFO_H

#include <vector>

#include "lldb/Utility/RegisterValue.h"
#include "lldb/Target/DynamicRegisterInfo.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Utility/ArchSpec.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/lldb-private-types.h"
#include "lldb/lldb-enumerations.h"
#include "lldb/lldb-private.h"
#include "lldb/Utility/Endian.h"

namespace lldb_private {

#define XMM_QUADWORD_SIZE 2
#define CJTHREAD_NAME_SIZE 32

template <typename T>
T ReadWithByteOrder(void *ptr, lldb::ByteOrder order) {
    static_assert(
        sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
        "ReadWithByteOrder only supports 2, 4, or 8 byte integer types"
    );

    DataExtractor extractor;
    extractor.SetData(ptr, sizeof(T), order);

    lldb::offset_t offset = 0;
    if constexpr (sizeof(T) == 2) {
        return static_cast<T>(extractor.GetU16(&offset));
    } else if constexpr (sizeof(T) == 4) {
        return static_cast<T>(extractor.GetU32(&offset));
    } else { // sizeof(T) == 8
        return static_cast<T>(extractor.GetU64(&offset));
    }
}

struct CJThreadContext_Linux_X64 {
    unsigned long long rsp;  /* 0x0 */
    unsigned long long rbp;  /* 0x8 */
    unsigned long long rbx;  /* 0x10 */
    unsigned long long rip;  /* 0x18 */
    unsigned long long r12;  /* 0x20 */
    unsigned long long r13;  /* 0x28 */
    unsigned long long r14;  /* 0x30 */
    unsigned long long r15;  /* 0x38 */
    unsigned int mxcsr;      /* 0x40 */
    unsigned short fpuCw;    /* 0x44 */

    void CopyTo(std::map<ConstString, RegisterValue> &regValMap, lldb::ByteOrder byte_order) {
      regValMap[ConstString("rsp")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->rsp, byte_order));
      regValMap[ConstString("rbp")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->rbp, byte_order));
      regValMap[ConstString("rbx")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->rbx, byte_order));
      regValMap[ConstString("rip")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->rip, byte_order));
      regValMap[ConstString("r12")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->r12, byte_order));
      regValMap[ConstString("r13")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->r13, byte_order));
      regValMap[ConstString("r14")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->r14, byte_order));
      regValMap[ConstString("r15")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->r15, byte_order));
      regValMap[ConstString("mxcsr")] = RegisterValue(ReadWithByteOrder<uint32_t>(&this->mxcsr, byte_order));
      regValMap[ConstString("fpucw")] = RegisterValue(ReadWithByteOrder<uint16_t>(&this->fpuCw, byte_order));
    }
};

struct CJThreadContext_Windows_X64 : CJThreadContext_Linux_X64 {
    unsigned long long rdi;                       /* 0x48 */
    unsigned long long rsi;                       /* 0x50 */
    unsigned long long xmm6[XMM_QUADWORD_SIZE];   /* 0x58 */
    unsigned long long xmm7[XMM_QUADWORD_SIZE];   /* 0x68 */
    unsigned long long xmm8[XMM_QUADWORD_SIZE];   /* 0x78 */
    unsigned long long xmm9[XMM_QUADWORD_SIZE];   /* 0x88 */
    unsigned long long xmm10[XMM_QUADWORD_SIZE];  /* 0x98 */
    unsigned long long xmm11[XMM_QUADWORD_SIZE];  /* 0xa8 */
    unsigned long long xmm12[XMM_QUADWORD_SIZE];  /* 0xb8 */
    unsigned long long xmm13[XMM_QUADWORD_SIZE];  /* 0xc8 */
    unsigned long long xmm14[XMM_QUADWORD_SIZE];  /* 0xd8 */
    unsigned long long xmm15[XMM_QUADWORD_SIZE];  /* 0xe8 */
    unsigned long long gsStackLow;                /* 0xf8 */
    unsigned long long gsStackHigh;               /* 0x100 */
    unsigned long long gsStackDeallocation;       /* 0x108 */

    void CopyTo(std::map<ConstString, RegisterValue> &regValMap, lldb::ByteOrder byte_order) {
        CJThreadContext_Linux_X64::CopyTo(regValMap, byte_order);

        regValMap[ConstString("rdi")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->rdi, byte_order));
        regValMap[ConstString("rsi")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->rsi, byte_order));

        RegisterValue xmm_val;
        xmm_val.SetBytes(this->xmm6, sizeof(this->xmm6), byte_order);
        regValMap[ConstString("xmm6")] = xmm_val;

        xmm_val.SetBytes(this->xmm7, sizeof(this->xmm7), byte_order);
        regValMap[ConstString("xmm7")] = xmm_val;

        xmm_val.SetBytes(this->xmm8, sizeof(this->xmm8), byte_order);
        regValMap[ConstString("xmm8")] = xmm_val;

        xmm_val.SetBytes(this->xmm9, sizeof(this->xmm9), byte_order);
        regValMap[ConstString("xmm9")] = xmm_val;

        xmm_val.SetBytes(this->xmm10, sizeof(this->xmm10), byte_order);
        regValMap[ConstString("xmm10")] = xmm_val;

        xmm_val.SetBytes(this->xmm11, sizeof(this->xmm11), byte_order);
        regValMap[ConstString("xmm11")] = xmm_val;

        xmm_val.SetBytes(this->xmm12, sizeof(this->xmm12), byte_order);
        regValMap[ConstString("xmm12")] = xmm_val;

        xmm_val.SetBytes(this->xmm13, sizeof(this->xmm13), byte_order);
        regValMap[ConstString("xmm13")] = xmm_val;

        xmm_val.SetBytes(this->xmm14, sizeof(this->xmm14), byte_order);
        regValMap[ConstString("xmm14")] = xmm_val;

        xmm_val.SetBytes(this->xmm15, sizeof(this->xmm15), byte_order);
        regValMap[ConstString("xmm15")] = xmm_val;
    }
};

struct CJThreadContext_Arm64 {
    unsigned long long x18; /* 0x0 */
    unsigned long long x19; /* 0x8 */
    unsigned long long x20; /* 0x10 */
    unsigned long long x21; /* 0x18 */
    unsigned long long x22; /* 0x20 */
    unsigned long long x23; /* 0x28 */
    unsigned long long x24; /* 0x30 */
    unsigned long long x25; /* 0x38 */
    unsigned long long x26; /* 0x40 */
    unsigned long long x27; /* 0x48 */
    unsigned long long x28; /* 0x50 */
    unsigned long long x29Fp;  /* 0x58 */
    unsigned long long x30Lr;  /* 0x60 */

    unsigned long long pc;  /* 0x68 */
    unsigned long long sp;  /* 0x70 */
    unsigned long long d8;  /* 0x78 */
    unsigned long long d9;  /* 0x80 */
    unsigned long long d10; /* 0x88 */
    unsigned long long d11; /* 0x90 */
    unsigned long long d12; /* 0x98 */
    unsigned long long d13; /* 0xa0 */
    unsigned long long d14; /* 0xa8 */
    unsigned long long d15; /* 0xb0 */

    unsigned int fpcr;      /* 0xb8 */

    void CopyTo(std::map<ConstString, RegisterValue> &regValMap, lldb::ByteOrder byte_order) {
      regValMap[ConstString("x18")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->x18, byte_order));
      regValMap[ConstString("x19")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->x19, byte_order));
      regValMap[ConstString("x20")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->x20, byte_order));
      regValMap[ConstString("x21")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->x21, byte_order));
      regValMap[ConstString("x22")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->x22, byte_order));
      regValMap[ConstString("x23")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->x23, byte_order));
      regValMap[ConstString("x24")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->x24, byte_order));
      regValMap[ConstString("x25")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->x25, byte_order));
      regValMap[ConstString("x26")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->x26, byte_order));
      regValMap[ConstString("x27")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->x27, byte_order));
      regValMap[ConstString("x28")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->x28, byte_order));
      regValMap[ConstString("fp")]  = RegisterValue(ReadWithByteOrder<uint64_t>(&this->x29Fp, byte_order));
      regValMap[ConstString("lr")]  = RegisterValue(ReadWithByteOrder<uint64_t>(&this->x30Lr, byte_order));
      regValMap[ConstString("pc")]  = RegisterValue(ReadWithByteOrder<uint64_t>(&this->pc, byte_order));
      regValMap[ConstString("sp")]  = RegisterValue(ReadWithByteOrder<uint64_t>(&this->sp, byte_order));

      regValMap[ConstString("d8")]  = RegisterValue(ReadWithByteOrder<uint64_t>(&this->d8, byte_order));
      regValMap[ConstString("d9")]  = RegisterValue(ReadWithByteOrder<uint64_t>(&this->d9, byte_order));
      regValMap[ConstString("d10")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->d10, byte_order));
      regValMap[ConstString("d11")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->d11, byte_order));
      regValMap[ConstString("d12")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->d12, byte_order));
      regValMap[ConstString("d13")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->d13, byte_order));
      regValMap[ConstString("d14")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->d14, byte_order));
      regValMap[ConstString("d15")] = RegisterValue(ReadWithByteOrder<uint64_t>(&this->d15, byte_order));
      regValMap[ConstString("fpcr")] = RegisterValue(ReadWithByteOrder<uint32_t>(&this->fpcr, byte_order));
    }
};

struct CJThreadInfoCommon {
    char name[CJTHREAD_NAME_SIZE];              /* cjthread name */
    int state;                                  /* cjthread state */
    unsigned int argSize;                       /* cjthread arg size */
    unsigned int processorId;                   /* processor id */
    unsigned int tid;                           /* thread id */
    uintptr_t argStart;                         /* arg start position */
    unsigned long sp;                           /* sp of the cjthread */
    unsigned long pc;                           /* pc of the cjthread */
    unsigned long long id;                      /* cjthread id */
    unsigned long long pthreadId;               /* pthread id */
};

template <class CJThreadContextType>
struct CJThreadInfo : CJThreadInfoCommon {
    CJThreadContextType context;             /* Context information recorded during the
                                              * last dispatch of the cjthread */
};

class CJDynamicRegisterInfo final : public DynamicRegisterInfo {
public:
  CJDynamicRegisterInfo() : DynamicRegisterInfo() {}

  ~CJDynamicRegisterInfo() override = default;

  bool RetrieveRegisterInfo(Thread &thread);
};

using CJThreadInfoSP = std::shared_ptr<CJThreadInfoCommon>;
using CJDynamicRegisterInfoSP = std::shared_ptr<CJDynamicRegisterInfo>;

} // namespace lldb_private

#endif // LLDB_TARGET_CJDYNAMICREGISTERINFO_H
