//===- StackMapsEncode.h - StackMaps ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Utils for compressing stack maps
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_STACKMAPENCODE_H
#define LLVM_CODEGEN_STACKMAPENCODE_H

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <unordered_map>
#include <vector>
namespace llvm {
namespace {
const int32_t SingleVarIntBits = 4;
const int32_t SingleVarIntMaxValue = 11;
const int32_t BitsPerByte = 8;
const int32_t BufferWidth = 64;
} // namespace

inline uint32_t getMinBytesForInt(int32_t Value) {
  if ((Value >= INT8_MIN) && (Value <= INT8_MAX)) {
    return 1;
  }
  if ((Value >= INT16_MIN) && (Value <= INT16_MAX)) {
    return 2;
  }
  return 4;
}
inline uint32_t getMinBytesForUInt(uint32_t Value) {
  if (Value <= UINT8_MAX) {
    return 1;
  }
  if (Value <= UINT16_MAX) {
    return 2;
  }
  if (Value <= 0xFFFFFF) {
    return 3;
  }
  return 4;
}

inline uint32_t getVarIntBitNumsForInt(int32_t Value) {
  if ((Value >= 0) && (Value <= SingleVarIntMaxValue)) {
    return SingleVarIntBits;
  }
  return SingleVarIntBits + BitsPerByte * getMinBytesForInt(Value);
}

inline uint32_t getVarIntBitNumsForUInt(uint32_t Value) {
  if (Value <= SingleVarIntMaxValue) {
    return SingleVarIntBits;
  }
  return SingleVarIntBits + BitsPerByte * getMinBytesForUInt(Value);
}

inline uint32_t getValidBitNums(uint32_t Value) {
  for (int I = 31; I >= 1; --I) { // 31: uint32_t occupy 32 bits at most
    if (Value & (1 << I)) {
      return I + 1;
    }
  }
  return 1; // value 0 and 1 occupy 1 bits.
}

struct CompressedInfo {
  CompressedInfo(const std::unordered_map<uint32_t, uint32_t> &CallSavedRegMap)
      : CSRegMap(CallSavedRegMap) {
    // Items[0] means useless records and will not be emitted.
    // so that StackMapItem's Index should minus 1 before use.
    RegItem RegInfo{0};
    RegItems.getOrInsertIndex(RegInfo);

    SlotItem SlotInfo{0, {0}, {}};
    SlotItems.getOrInsertIndex(SlotInfo);

    LineNumberItem LineNumberInfo{0};
    LNItems.getOrInsertIndex(LineNumberInfo);

    DerivedInfo.emplace_back(std::make_pair(0, 0));
  }
  ~CompressedInfo() = default;

  struct RegItem {
    unsigned RegBit;
    bool operator<(const RegItem &X) const { return RegBit < X.RegBit; }
  };

  struct SlotItem {
    int64_t BaseOffset;
    std::vector<uint64_t> SlotBit;
    std::vector<uint32_t> CompressedSlotBit;
    bool operator<(const SlotItem &X) const {
      return (BaseOffset < X.BaseOffset) ||
             ((BaseOffset == X.BaseOffset) && (SlotBit < X.SlotBit)) ||
             ((BaseOffset == X.BaseOffset) && (SlotBit == X.SlotBit) &&
              (CompressedSlotBit < X.CompressedSlotBit));
    }
  };

  struct LineNumberItem {
    unsigned LN;
    bool operator<(const LineNumberItem &X) const { return LN < X.LN; }
  };

  struct IdxItem {
    unsigned RegIdxPlusOne;
    unsigned SlotIdxPlusOne;
    unsigned LNIdxPlusOne;
    unsigned DerivedInfoStartIdx;
    unsigned SPRegIdxPlusOne;
    unsigned SPSlotIdxPlusOne;
  };

  template <typename T> struct ItemIdxHelper {
    std::vector<T> Items;
    std::map<T, unsigned> Item2Idx;
    unsigned getOrInsertIndex(T &Item) {
      auto Itr = Item2Idx.find(Item);
      if (Itr != Item2Idx.end()) {
        return Itr->second;
      }
      Item2Idx[Item] = Items.size();
      Items.emplace_back(Item);
      return Items.size() - 1;
    }
  };
  struct MaxBitsInfo {
    // StackMapItem
    uint32_t RegIdx = 0;
    uint32_t SlotIdx = 0;
    uint32_t LNIdx = 0;
    uint32_t DerivedIdx = 0;
    uint32_t SPRegIdx = 0;  // stack ptr RegIdx
    uint32_t SPSlotIdx = 0; // stack ptr SlotIdx
    // RegItem
    uint32_t RegBit = 0;
    // SlotItem
    uint32_t BaseOffset = 0;
    uint32_t SlotBit = 0;
    // LineNumberItem
    uint32_t LN = 0;
  };

public:
  // prologue
  uint64_t StackSize;
  uint64_t FormatType;
  unsigned CalleeSavedReg = 0; // aarch64 and x86 use 32 bit at most
  SmallVector<unsigned, 32> CalleeSavedOffsets; // 32: preset vector size

  MapVector<const MCExpr *, IdxItem> StackMapItem;
  ItemIdxHelper<RegItem> RegItems;
  ItemIdxHelper<SlotItem> SlotItems;
  ItemIdxHelper<LineNumberItem> LNItems;
  // <regIdxPlusOne, slotIdxPlusOne>
  std::vector<std::pair<unsigned, unsigned>> DerivedInfo;
  std::map<std::vector<std::pair<unsigned, unsigned>>, unsigned>
 	       DerivedInfo2StartIdx;

  MaxBitsInfo MaxBits;
  // unordered_map<regNo, bitIdx>, regNo can be used to find callee saved reg
  // while bitIdx is used in prologue.
  const std::unordered_map<uint32_t, uint32_t> &CSRegMap;

  unsigned getOrInsertDerivedInfoStartIdx(
      const std::vector<std::pair<unsigned, unsigned>> &Items) {
    auto Itr = DerivedInfo2StartIdx.find(Items);
    if (Itr != DerivedInfo2StartIdx.end()) {
      return Itr->second;
    }

    unsigned StartIdx = DerivedInfo.size();
    DerivedInfo.insert(DerivedInfo.end(), Items.begin(), Items.end());
    DerivedInfo2StartIdx.emplace(Items, StartIdx);
    return StartIdx;
  }
};

class DataEncoder {
public:
  DataEncoder(MCStreamer &Streamer, const CompressedInfo &InitData,
              const std::vector<std::string> &InitPrologueBit2Reg,
              const std::vector<std::string> &Bit2Reg)
      : OS(Streamer), Data(InitData), PrologueBit2Reg(InitPrologueBit2Reg),
        Bit2RegStr(Bit2Reg) {
    reset();
  }
  ~DataEncoder() = default;
  void emitPrologueAndStackMapItemHeader();
  void emitStackMapItem();
  void writeRegRefAndSlotRef();
  void writeLineNumber();
  void writeDerivedInfo();
  void emitBufferContent();
  void emitCommentForDataAfterStackMapItem();

private:
  void writeBits(uint32_t BitNums, int64_t Value) {
    assert(BitNums <= BufferWidth && "can't write bits lager than 64!");
    if (BitNums == 0) {
      return;
    }
    auto Temp = *reinterpret_cast<uint64_t *>(&Value);
    if (BitNums != BufferWidth) {
      // clear high bits: Temp &= 0b0001..BitNums..1
      Temp &= (((uint64_t)1 << BitNums) - 1);
    }
    if (RemainedBits == 0) {
      Buffer.emplace_back(Temp);
      RemainedBits = BufferWidth - BitNums;
      return;
    }
    if (BitNums > RemainedBits) {
      // put some bits to remained bits and others to new uint64_t
      Buffer.back() |= Temp << (BufferWidth - RemainedBits);
      Buffer.emplace_back(Temp >> RemainedBits);
      RemainedBits = BufferWidth - (BitNums - RemainedBits);
      return;
    }
    // put values to remained bits
    Buffer.back() |= Temp << (BufferWidth - RemainedBits);
    RemainedBits -= BitNums;
    return;
  }

  void writeBitVec(uint32_t BitNums, const std::vector<uint64_t> &Value) {
    unsigned Idx = 0;
    assert(!Value.empty() && "Value should not be empty!");
    assert(BitNums >= ((Value.size() - 1) * BufferWidth) &&
           "BitNums should not less than (Value.size() - 1) * 64");
    while (Idx < Value.size() - 1) {
      writeBits(64, Value[Idx++]); // 64: bitnum
      BitNums -= BufferWidth;
    }
    if (BitNums > BufferWidth) {
      // write extra 0s when BitNums larger then vector size
      writeBits(BufferWidth, Value[Idx]);
      BitNums -= BufferWidth;
      while (BitNums >= BufferWidth) {
        writeBits(BufferWidth, 0);
        BitNums -= BufferWidth;
      }
      writeBits(BitNums, 0);
    } else {
      writeBits(BitNums, Value[Idx]);
    }
    return;
  }

  void writeCompressedBitVec(uint32_t BitNums,
                           const std::vector<uint32_t> &Value) {
    uint32_t BitCnt = 0;
    assert(!Value.empty() && "Value should not be empty!");
    // bit31 == 1 means the following 31bits/remain bits are origion val
    // bit31 == 0:
    //   bit30's value stand for 0-compressed/1-compressed:
    //   remain bits are nums of 0/1 compressed unit
    for (unsigned Idx = 0, End = Value.size(); Idx < End; ++Idx) {
      if (Value[Idx] & 0x80000000) {
        writeBits(1, 1);
        uint32_t WriteBitNums = 31;
        // we need to get valid nums if the origin value is the last element.
        if (Idx == End - 1) {
          WriteBitNums = getValidBitNums(Value[Idx] & 0x7fffffff);
        }
        writeBits(WriteBitNums, Value[Idx] & 0x7fffffff);
        BitCnt += 1 + WriteBitNums;
      } else {
        writeBits(1, 0);
        if (Value[Idx] & 0x40000000) {
          writeBits(1, 1);
        } else {
          writeBits(1, 0);
        }
        uint32_t VarIntVal = Value[Idx] & 0x3fffffff;
        writeVarUint(VarIntVal);
        BitCnt += 2;
        BitCnt += getVarIntBitNumsForUInt(VarIntVal);
      }
    }

    if (BitNums < BitCnt) {
      report_fatal_error("bitlen error while write compressed bit map!\n");
    }
    // write extra 0s when BitNums larger then BitCnt
    uint32_t LeftBits = BitNums - BitCnt;
    while (LeftBits > 0) {
      if (LeftBits >= BufferWidth) {
        writeBits(BufferWidth, 0);
        LeftBits -= BufferWidth;
      } else {
        writeBits(LeftBits, 0);
        break;
      }
    }
  }

  void writeVarInt(int64_t Value, bool IsUnsigned = false) {
    if ((Value >= 0) && (Value <= SingleVarIntMaxValue)) {
      writeBits(SingleVarIntBits, Value);
    } else {
      uint32_t ByteNums =
          IsUnsigned ? getMinBytesForUInt(Value) : getMinBytesForInt(Value);
      writeBits(SingleVarIntBits, ByteNums + SingleVarIntMaxValue);
      writeBits(ByteNums * BitsPerByte, Value);
    }
    return;
  }

  void writeVarUint(int64_t Value) { writeVarInt(Value, true); }

  void writeBitsToPadding() {
    // 4: PaddingBits range from 0 to 7 which occupy 4 bits.
    unsigned PaddingBits =
        (RemainedBits + BufferWidth - SingleVarIntBits) % BitsPerByte;
    writeVarInt(PaddingBits);
    if (PaddingBits != 0) {
      writeBits(PaddingBits, 0);
    }
  }

  uint64_t readBits(uint32_t BitNums) {
    assert(BitNums <= BufferWidth && "Can't read bits larger than 64");
    if (BitNums == 0) {
      return 0;
    }
    unsigned ReadIdx = ReadBitsAll / BufferWidth;
    uint64_t ReadBitsInCurIdx = ReadBitsAll % BufferWidth;
    ReadBitsAll += BitNums;
    uint64_t Value = Buffer.at(ReadIdx) >> (ReadBitsInCurIdx);
    if (BitNums + ReadBitsInCurIdx <= BufferWidth) {
      if (BitNums != BufferWidth) {
        Value &= ((uint64_t)1 << BitNums) - 1;
      }
    } else {
      // ReadBitsInCurIdx  = [1, 63] and BitNums > (64 -ReadBitsInCurIdx)
      BitNums -= (BufferWidth - ReadBitsInCurIdx);
      Value |= ((Buffer.at(ReadIdx + 1) & (((uint64_t)1 << BitNums) - 1))
                << (BufferWidth - ReadBitsInCurIdx));
    }
    return Value;
  }

  int32_t readBitsAsInt(uint32_t BitNums) {
    uint64_t TempValue = readBits(BitNums);
    switch (BitNums) {
    case 8: {
      uint8_t Result = TempValue & 0xFF;
      return *(reinterpret_cast<int8_t *>(&Result));
    }
    case 16: {
      uint16_t Result = TempValue & 0xFFFF;
      return *(reinterpret_cast<int16_t *>(&Result));
    }
    case 32: {
      uint32_t Result = TempValue & 0xFFFFFFFF;
      return *(reinterpret_cast<int32_t *>(&Result));
    }
    default: {
      report_fatal_error("BitNums should be 8/16/32 for Int Value!");
      return 0;
    }
    }
  }
  int32_t readVarInt() {
    uint32_t Value = readBits(SingleVarIntBits);
    if (Value <= SingleVarIntMaxValue) {
      return Value;
    }
    return readBitsAsInt((Value - SingleVarIntMaxValue) * BitsPerByte);
  }
  uint32_t readVarUint() {
    uint32_t Value = readBits(SingleVarIntBits);
    if (Value <= SingleVarIntMaxValue) {
      return Value;
    }
    return readBits((Value - SingleVarIntMaxValue) * BitsPerByte);
  }

  void emitCommentForPrologueAndStackMapItemHeader();
  void emitCommentForStackMapItem();
  void emitCommentForRegsAndSlots();
  void emitCommentForLineNumber();
  void emitCommentForDerivedInfo();
  void reset() {
    Buffer.clear();
    RemainedBits = 0;
    ReadBitsAll = 0;
  }

private:
  MCStreamer &OS;
  std::vector<uint64_t> Buffer;
  uint32_t RemainedBits = 0;
  uint32_t ReadBitsAll = 0;
  uint64_t FormatType = 0;
  CompressedInfo::MaxBitsInfo MaxBits;
  const CompressedInfo &Data;
  const std::vector<std::string> &PrologueBit2Reg;
  const std::vector<std::string> &Bit2RegStr;
  void emitCommentForSlots(raw_svector_ostream &Comment, unsigned int I);
};
} // end namespace llvm
#endif // LLVM_CODEGEN_STACKMAPENCODE_H
