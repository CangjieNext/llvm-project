//===-- CjTypes.cpp ---------------------------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#include "CjTypes.h"
#include <tuple>
#include "lldb/DataFormatters/StringPrinter.h"
#include "lldb/DataFormatters/FormattersHelpers.h"
#include "lldb/Target/Process.h"
#include "lldb/DataFormatters/DumpValueObjectOptions.h"
#include "lldb/DataFormatters/ValueObjectPrinter.h"
#include "lldb/Core/ValueObject.h"

using namespace lldb;
using namespace lldb_private;

bool lldb_private::formatters::BasicSummaryProvider(ValueObject &valobj, Stream &stream, const TypeSummaryOptions &) {
  ConstString type = valobj.GetTypeName();
  if (type == "Rune") {
    DataExtractor data;
    Status error;
    valobj.GetData(data, error);

    if (error.Fail()) {
      return false;
    }

    StringPrinter::ReadBufferAndDumpToStreamOptions options(valobj);
    options.SetData(std::move(data));
    options.SetStream(&stream);
    options.SetQuote('\'');
    options.SetSourceSize(1);
    options.SetBinaryZeroIsTerminator(false);

    return StringPrinter::ReadBufferAndDumpToStream<StringPrinter::StringElementType::UTF32>(options);
  }

  std::string value;
  if (type == "Int8") {
    valobj.GetValueAsCString(lldb::eFormatDecimal, value);
  } else if (type == "UInt8") {
    valobj.GetValueAsCString(lldb::eFormatUnsigned, value);
  }

  if (!value.empty()) {
    stream.Printf("%s", value.c_str());
  }

  return true;
}

bool lldb_private::formatters::StringSummaryProvider(
    ValueObject &valobj, Stream &stream, const TypeSummaryOptions &) {
  lldb::ValueObjectSP raw = valobj.GetNonSyntheticValue();
  if (!raw) {
    return false;
  }

  lldb::ValueObjectSP data = raw->GetChildMemberWithName(ConstString("myData"), true);
  if (!data) {
    return false;
  }

  Status error;
  if (data->IsPointerType()) {
    data = data->Dereference(error);
    if (error.Fail()) {
      return false;
    }
  }

  lldb::ValueObjectSP start = raw->GetChildMemberWithName(ConstString("start"), true);
  lldb::ValueObjectSP len = raw->GetChildMemberWithName(ConstString("length"), true);
  lldb::ValueObjectSP elements = data->GetChildMemberWithName(ConstString("elements"), true);
  if (!start || !len || !elements) {
    return false;
  }

  StringPrinter::ReadStringAndDumpToStreamOptions options(*elements);
  uint64_t startValue = start->GetValueAsUnsigned(UINT64_MAX);
  if (startValue == UINT64_MAX) {
    return false;
  }
  options.SetLocation(elements->GetAddressOf() + startValue);
  options.SetTargetSP(valobj.GetTargetSP());
  options.SetStream(&stream);
  uint64_t lenValue = len->GetValueAsUnsigned(UINT64_MAX);
  if (lenValue == UINT64_MAX) {
    return false;
  }
  if (lenValue == 0) {
    stream.Printf("\"\"");
    return true;
  }
  options.SetSourceSize(lenValue);
  options.SetHasSourceSize(true);
  options.SetNeedsZeroTermination(false);
  options.SetIgnoreMaxLength(false);
  options.SetEscapeNonPrintables(true);
  options.SetBinaryZeroIsTerminator(false);

  return StringPrinter::ReadStringAndDumpToStream<StringPrinter::StringElementType::UTF8>(options);
}

bool lldb_private::formatters::UnitSummaryProvider(
    ValueObject &, Stream &stream, const TypeSummaryOptions &) {
  stream.Printf("()");
  return true;
}

bool lldb_private::formatters::CStringSummaryProvider(
    ValueObject &valobj, Stream &stream, const TypeSummaryOptions &) {
  lldb::ValueObjectSP raw = valobj.GetNonSyntheticValue();
  if (!raw) {
    return false;
  }

  lldb::ValueObjectSP chars = raw->GetChildMemberWithName(ConstString("chars"), true);
  if (!chars) {
    return false;
  }

  lldb::ValueObjectSP cpoint = chars->GetChildMemberWithName(ConstString("ptr"), true);
  if (!cpoint) {
    // For llvmgc backend. If CString in Jet, char * is CString.chars.ptr.
    // Need delete cpoint after CString refactor to built-in on Jet backend.
    cpoint = chars;
  }
  time_t currentTime;
  struct tm* localTime = localtime(&currentTime);
  stream.Printf("%s", cpoint->GetSummaryAsCString());

  return true;
}

bool lldb_private::formatters::CPointerSummaryProvider(
    ValueObject &valobj, Stream &stream, const TypeSummaryOptions &) {
  lldb::ValueObjectSP raw = valobj.GetNonSyntheticValue();
  if (!raw) {
    return false;
  }

  lldb::ValueObjectSP ptr = raw->GetChildMemberWithName(ConstString("ptr"), true);
  if (!ptr) {
    return false;
  }

  stream.Printf("%s", ptr->GetValueAsCString());

  return true;
}

static void DumpWithoutPrefix(lldb::ValueObjectSP valueSP, Stream &stream)
{
  if (valueSP == nullptr) {
    return;
  }
  auto target = valueSP->GetTargetSP();
  if (target == nullptr) {
    return;
  }

  StreamString result;
  DumpValueObjectOptions options;
  auto max_depth = target->GetMaximumDepthOfChildrenToDisplay();
  options.SetMaximumPointerDepth({DumpValueObjectOptions::PointerDepth::Mode::Always, max_depth.first});
  options.SetMaximumDepth(max_depth.first, max_depth.second);
  options.SetHideRootType(true);
  options.SetVariableFormatDisplayLanguage(valueSP->GetPreferredDisplayLanguage());
  valueSP->Dump(result, options);
  std::string value = result.GetString().data();
  std::string prefix = " = ";
  size_t start = value.find_first_of(prefix);
  size_t end = value.find_last_of("\n");
  if (start != std::string::npos && end != std::string::npos) {
    stream.Printf("%s", value.substr(start + prefix.size(), end - start - prefix.size()).c_str());
  }
  return;
}

bool lldb_private::formatters::RangeSummaryProvider(
    ValueObject &valobj, Stream &stream, const TypeSummaryOptions &) {
  lldb::ValueObjectSP raw = valobj.GetNonSyntheticValue();

  lldb::ValueObjectSP startSP = raw->GetChildMemberWithName(ConstString("start"), true);
  lldb::ValueObjectSP endSP = raw->GetChildMemberWithName(ConstString("end"), true);
  lldb::ValueObjectSP stepSP = raw->GetChildMemberWithName(ConstString("step"), true);
  lldb::ValueObjectSP closedSP = raw->GetChildMemberWithName(ConstString("isClosed"), true);
  lldb::ValueObjectSP hasStartSP = raw->GetChildMemberWithName(ConstString("hasStart"), true);
  lldb::ValueObjectSP hasEndSP = raw->GetChildMemberWithName(ConstString("hasEnd"), true);
  if (!startSP || !endSP || !stepSP || !closedSP || !hasStartSP || !hasEndSP) {
    return false;
  }

  std::string hasStart = hasStartSP->GetValueAsCString();
  std::string hasEnd = hasEndSP->GetValueAsCString();
  std::string closed = closedSP->GetValueAsCString();
  if (hasStart == "true") {
    DumpWithoutPrefix(startSP, stream);
  }
  stream.Printf("..");
  if (closed == "true") {
    stream.Printf("=");
  }
  if (hasEnd == "true") {
    DumpWithoutPrefix(endSP, stream);
  }
  stream.Printf(" : ");
  DumpWithoutPrefix(stepSP, stream);
  return true;
}

bool lldb_private::formatters::OptionSummaryProvider(
    ValueObject &valobj, Stream &stream, const TypeSummaryOptions &options) {
  Status error;
  lldb::ValueObjectSP raw = valobj.GetNonSyntheticValue();
  lldb::ValueObjectSP constructorSP = raw->GetChildMemberWithName(ConstString("constructor"), true);
  if (constructorSP) {
    std::string constructor = constructorSP->GetValueAsCString();
    if (constructor == "None") {
      stream.Printf("None");
      return true;
    }
  }

  lldb::ValueObjectSP valueSP = raw->GetChildMemberWithName(ConstString("val"), true);
  if (valueSP == nullptr) {
    valueSP = raw->GetChildMemberWithName(ConstString("Recursive-val"), true);
    if (valueSP) {
      if (valueSP->IsPointerType()) {
        stream.Printf("%s", valueSP->GetValueAsCString());
      } else {
        stream.Printf("0x%" PRIx64, valueSP->GetAddressOf());
      }
      return true;
    }
    return false;
  }

  if (valueSP->IsPointerType()) {
    valueSP = valueSP->Dereference(error);
    if (error.Fail()) {
      return false;
    }
    auto addr = valueSP->GetAddressOf();
    if (addr == LLDB_INVALID_ADDRESS || addr == 0) {
      stream.Printf("None");
      return true;
    }
  }

  TypeSummaryImpl *entry = valueSP->GetSummaryFormat().get();
  if (entry) {
    std::string summary;
    valueSP->GetSummaryAsCString(entry, summary, lldb::LanguageType::eLanguageTypeC_plus_plus_14);
    if (!summary.empty()) {
      stream.Printf("%s", summary.c_str());
      return true;
    }
  }

  const char *value_cstr = valueSP->GetValueAsCString();
  if (value_cstr) {
    stream.Printf("%s", value_cstr);
    return true;
  }

  return true;
}

bool lldb_private::formatters::OptionPtrSummaryProvider(
    ValueObject &valobj, Stream &stream, const TypeSummaryOptions &options) {
  if (valobj.IsPointerType()) {
    Status error;
    lldb::ValueObjectSP valueSP = valobj.Dereference(error);
    if (error.Fail()) {
      stream.Printf("None");
      return true;
    }

    auto addr = valueSP->GetAddressOf();
    if (addr == LLDB_INVALID_ADDRESS || addr == 0) {
      stream.Printf("None");
      return true;
    }

    DumpWithoutPrefix(valueSP, stream);
    return true;
  }

  return false;
}

bool lldb_private::formatters::EnumOptionSummaryProvider(
    ValueObject &valobj, Stream &stream, const TypeSummaryOptions &options) {
  lldb::ValueObjectSP valueSP;
  if (valobj.IsPointerType()) {
    Status error;
    valueSP = valobj.Dereference(error);
    if (error.Fail()) {
      return false;
    }
    valueSP = valueSP->GetNonSyntheticValue();
  } else {
    valueSP = valobj.GetNonSyntheticValue();
  }

  for (size_t i = 0; i < valueSP->GetCompilerType().GetNumDirectBaseClasses(); i++) {
    ValueObjectSP ctor = valueSP->GetChildAtIndex(i, true);
    if (!ctor) {
      continue;
    }
    auto enum_class = ctor->GetChildMemberWithName(ConstString("EnumClass$"), true);
    if (!enum_class) {
      continue;
    }

    auto type = enum_class->GetChildMemberWithName(ConstString("constructor"), true);
    if (!type) {
      continue;
    }

    auto id = type->GetValueAsUnsigned(0);
    if (id >= valueSP->GetNumChildren()) {
      continue;
    }
    auto real_ctor = valueSP->GetChildAtIndex(id, true);
    if (!real_ctor) {
      continue;
    }
    auto real_enumclass = real_ctor->GetChildMemberWithName(ConstString("EnumClass$"), true);
    if (!real_enumclass) {
      continue;
    }
    DataExtractor data;
    Status err;
    enum_class->GetData(data, err);
    valueSP = lldb_private::ValueObject::CreateValueObjectFromData("EnumClass", data,
        real_enumclass->GetExecutionContextRef(), real_enumclass->GetCompilerType());
    valueSP = valueSP->GetChildMemberWithName(ConstString("constructor"), true);
    break;
  }

  TypeSummaryImpl *entry = valueSP->GetSummaryFormat().get();
  if (entry) {
    std::string summary;
    valueSP->GetSummaryAsCString(entry, summary, lldb::LanguageType::eLanguageTypeC_plus_plus_14);
    if (!summary.empty()) {
      stream.Printf("%s", summary.c_str());
      return true;
    }
  }

  std::string value_cstr(valueSP->GetValueAsCString());
  if (!value_cstr.empty() && value_cstr != std::string("Some")) {
    stream.Printf("%s", value_cstr.c_str());
    return true;
  }
  return true;
}

bool lldb_private::formatters::EnumOptionPtrSummaryProvider(
    ValueObject &valobj, Stream &stream, const TypeSummaryOptions &options) {
  auto valueSP = valobj.GetNonSyntheticValue();
  if (valueSP->IsPointerType()) {
    Status error;
    valueSP = valueSP->Dereference(error);
    if (error.Fail()) {
      stream.Printf("None");
      return true;
    }

    auto addr = valueSP->GetAddressOf();
    if (addr == LLDB_INVALID_ADDRESS || addr == 0) {
      stream.Printf("None");
      return true;
    }

    DumpWithoutPrefix(valueSP, stream);
    return true;
  }

  return false;
}

bool lldb_private::formatters::FunctionSummaryProvider(
    ValueObject &valobj, Stream &stream, const TypeSummaryOptions &) {
  lldb::ValueObjectSP raw = valobj.GetNonSyntheticValue();
  if (!raw) {
    return false;
  }

  lldb::ValueObjectSP ptr = raw->GetChildMemberWithName(ConstString("ptr"), true);
  if (!ptr) {
    return false;
  }
  if (ptr->GetSummaryAsCString() == nullptr) {
    stream.Printf("%s", ptr->GetValueAsCString());
  } else {
    stream.Printf("%s %s", ptr->GetValueAsCString(), ptr->GetSummaryAsCString());
  }
  return true;
}

bool lldb_private::formatters::EnumSummaryProvider(
    ValueObject &valobj, Stream &stream, const TypeSummaryOptions &) {
  lldb::ValueObjectSP raw = valobj.GetNonSyntheticValue();
  if (!raw) {
    return false;
  }

  lldb::ValueObjectSP ptr = raw->GetChildMemberWithName(ConstString("constructor"), true);
  if (ptr == nullptr) {
    return false;
  }

  std::vector<ConstString> enumerators;
  auto type = ptr->GetCompilerType();
  type.ForEachEnumerator(
    [&enumerators](const CompilerType &integer_type, ConstString name, const llvm::APSInt &value) -> bool {
      enumerators.push_back(name);
      return true;
    }
  );

  if (enumerators.size() == 1) {
    stream.Printf("%s", enumerators[0].AsCString());
  } else {
    stream.Printf("%s", ptr->GetValueAsCString());
  }

  return true;
}


// The layout of the type is:
// public struct BigInt {
//    let int: UInt64,
//    let intArr: Array<UInt32>,
//    let negSign: Bool
// }
// public struct Array<T> {
//   let rawptr: RawArray<T>
//   let start: Int64
//   let len: Int64

class BigInt {
public:
    BigInt(uint64_t intv, std::vector<uint32_t> big_intArr, int64_t len, bool negSign)
        : m_intv(intv),
          m_big_intArr(big_intArr),
          m_len(len),
          m_negsign(negSign) { }
    std::string ToString() {
      int64_t digits = 9;
      uint64_t anyBase = 1000000000;
      // Get str Arr size
      int64_t strArrSize = (32 * (m_len + 2) / int64_t(29) + 1);
      std::vector<uint64_t> stringArr;
      for (size_t i = 0; i < strArrSize; i++) {
        stringArr.emplace_back(0);
      }
      int64_t stringArrBound = CalculateStrArr(stringArr, m_big_intArr, m_intv, 1);

      std::vector<uint8_t> stringUtf8Arr(stringArrBound * digits + 1, '0');
      int64_t begin = ToBaseString(stringUtf8Arr, 9, stringArr[stringArrBound - 1]);
      if (m_negsign) {
        stringUtf8Arr[begin] = '-';
        begin--;
      }
      begin++;
      for (int64_t i = stringArrBound - 2; i >= 0; i--) {
        ToBaseString(stringUtf8Arr, digits * (stringArrBound - i), stringArr[i]);
      }
      std::string result;
      for (size_t i = begin; i < stringUtf8Arr.size(); i++) {
        result += static_cast<char>(stringUtf8Arr[i]);
      }
      return result;
    }

    int64_t ToBaseString(std::vector<uint8_t>& stringUtf8Arr, int64_t index, uint64_t int1) {
      const uint8_t DIGIT_DIFF = 0x30;
      const uint8_t LETTER_UPPER_DIFF = 0x41 - 0x0A;
      const uint8_t LETTER_LOWER_DIFF = 0x61 - 0x0A;
      uint64_t base = 10;
      int64_t i = index;
      uint64_t quo = int1;
      while (quo != 0) {
          uint8_t rem = uint8_t(quo % base);
          if (rem < 10) { // Decimal 10
            stringUtf8Arr[i] = rem + DIGIT_DIFF;
          } else {
            stringUtf8Arr[i] = rem + LETTER_UPPER_DIFF;
          }
          quo = quo / uint64_t(base);
          i--;
      }
      return i;
    }

    int64_t CalculateStrArr(std::vector<uint64_t>& stringArr, uint64_t bigInt, int64_t bound) {
      int64_t stringArrBound = bound;
      int64_t anyBase = 1000000000;
      for (int64_t j = stringArrBound - 1; j >= 0; --j) {
        stringArr[j] <<= 32; // 32-bit
      }
      stringArr[0] += bigInt;
      for (size_t j = 0; j < stringArrBound - 1; j++) {
          stringArr[j + 1] += stringArr[j] / anyBase;
          stringArr[j] %= anyBase;
      }
      int64_t rem = stringArr[stringArrBound - 1] / anyBase;
      stringArr[stringArrBound - 1] %= anyBase;
      while (rem > 0) {
          stringArr[stringArrBound] = rem;
          stringArrBound++;
          rem = stringArr[stringArrBound - 1] / anyBase;
          stringArr[stringArrBound - 1] %= anyBase;
          if (stringArrBound >= stringArr.size()) {
            break;
          }
      }
      return stringArrBound;
    }

    int64_t CalculateStrArr(std::vector<uint64_t>& stringArr, std::vector<uint32_t>& bigArray,
                            uint64_t bigInt, int64_t bound) {
      int64_t stringArrBound = bound;
      int64_t i = bigArray.size() - 1;
      while (i >= 0) {
        stringArrBound = CalculateStrArr(stringArr, uint64_t(bigArray[i]), stringArrBound);
        i--;
      }
      stringArrBound = CalculateStrArr(stringArr, bigInt >> 32, stringArrBound); // 32-bit
      return CalculateStrArr(stringArr, (bigInt & 0xFFFFFFFF), stringArrBound);
    }
private:
   uint64_t m_intv;
   std::vector<uint32_t> m_big_intArr;
   int64_t m_len;
   bool m_negsign;
};

std::string lldb_private::formatters::GetBigIntvalue(ValueObject &value) {
  // The layout of the type is:
  // public struct BigInt {
  //    let int: UInt64,
  //    let intArr: Array<UInt32>,
  //    let negSign: Bool
  // }
  lldb::ValueObjectSP bint = value.GetChildMemberWithName(ConstString("int"), true);
  lldb::ValueObjectSP negSign = value.GetChildMemberWithName(ConstString("negSign"), true);
  lldb::ValueObjectSP valueIntArr = value.GetChildMemberWithName(ConstString("intArr"), true);
  if (!bint || !negSign || !valueIntArr) {
    return "0";
  }
  auto value_int = bint->GetValueAsUnsigned(UINT64_MAX);
  auto value_negSign = negSign->GetValueAsSigned(INT64_MAX);
  auto array_len = valueIntArr->GetChildMemberWithName(ConstString("len"), true);
  if (!array_len) {
    return "0";
  }
  auto len = array_len->GetValueAsSigned(INT64_MAX);
  if (len == 0) {
    std::string result = std::to_string(value_int);
    if (value_negSign == 1) {
      result = "-" + result;
    }
    return result;
  }
  // use intArr
  // public struct Array<T> {
  //   let rawptr: RawArray<T>
  //   let start: Int64
  //   let len: Int64
  auto rawptr = valueIntArr->GetChildMemberWithName(ConstString("rawptr"), true);
  auto m_start = valueIntArr->GetChildMemberWithName(ConstString("start"), true);
  if (!rawptr || !m_start) {
    return "0";
  }
  auto start = m_start->GetValueAsSigned(INT64_MAX);

  auto m_elements = rawptr->GetChildMemberWithName(ConstString("elements"), true);
  CompilerType element_type = m_elements->GetCompilerType();
  llvm::Optional<uint64_t> size = element_type.GetByteSize(nullptr);
  uint64_t element_size = 4; // UInt32's size is 4.

  std::vector<uint32_t> big_intArr;
  for (size_t idx = 0; idx < len; idx++) {
    addr_t addr = m_elements->GetAddressOf() + (m_start->GetValueAsUnsigned(0) + idx) * element_size;
    ValueObjectSP valobj_sp(
      ValueObject::CreateValueObjectFromAddress("tmp", addr, m_elements->GetExecutionContextRef(), element_type));

    if (valobj_sp)
      valobj_sp->SetSyntheticChildrenGenerated(true);

    if (valobj_sp->IsPointerType()) {
      Status error;
      valobj_sp = valobj_sp->Dereference(error);
      valobj_sp->SetName(ConstString(valobj_sp->GetName().GetStringRef().ltrim('*').str()));
    }
    auto tmpU32 = valobj_sp->GetValueAsUnsigned(UINT32_MAX);
    big_intArr.emplace_back(tmpU32);
  }
  auto bigint = BigInt(value_int, big_intArr, len, value_negSign);
  return bigint.ToString();
}

// The layout of the type is:
// public struct Decimal {
//    var _scale: Int32
//    var _precision: Int64
//    var _value: BigInt
//    var _sign: Int64
// }
class DecimalToString {
public:
    DecimalToString(int32_t scale, int64_t precision, std::string bigInt, int64_t sign)
        : _scale(scale), _precision(precision), _bigInt(bigInt), _sign(sign) {}
  std::string ToString() {
    if (_scale < 0) {
      if (_sign == 0) {
        return "0";
      }
      std::string decimalStrArr(_bigInt.length() - _scale, '0');
      decimalStrArr = _bigInt + decimalStrArr;
      return decimalStrArr;
    }
    if (_scale > INT32_MAX) {
      return "";
    }
    std::string unscaleValStrArr = _bigInt;
    // scale value bigger than 0, need insert decimal point to corresponding position.
    // precision is bigger than 0 and scale range [0, Int32.Max]
    auto decimalPointIndex = _precision - _scale;
    std::string decimalStrArr(unscaleValStrArr);
    if (decimalPointIndex == 0) {
      if (unscaleValStrArr.at(0) == '-') {
        decimalStrArr.insert(0, 1, '-');
        decimalStrArr.insert(2, 1, '.'); // position is 2
      } else {
        decimalStrArr.insert(0, 1, '0');
        decimalStrArr.insert(1, 1, '.');
      }
    } else if (decimalPointIndex > 0) {
      if (unscaleValStrArr[0] == '-') {
        decimalStrArr.insert(decimalPointIndex + 1, 1, '.');
      } else {
        decimalStrArr.insert(decimalPointIndex, 1, '.');
      }
    } else {
      // decimalPointIndex less than 0, decimal point in the left of bigint value.
      // when decimalPointIndex less than 0, it's range [-Int32.Max, 0], abs will nerver overflow.
      std::string temp(unscaleValStrArr.size() + abs(decimalPointIndex) + 3, '0');
      decimalStrArr = temp;
      if (unscaleValStrArr[0] == '-') {
        decimalStrArr[0] = '-';
        decimalStrArr[2] = '.'; // Index 2.
        for (int64_t i = 1; i < unscaleValStrArr.size(); i++) {
          decimalStrArr[abs(decimalPointIndex) + 3 + i] = unscaleValStrArr[i]; // Offset 3.
        }
      } else {
        decimalStrArr[1] = '.';
        for (int64_t i = 0; i < unscaleValStrArr.size(); i++) {
          decimalStrArr[abs(decimalPointIndex) + 2 + i] = unscaleValStrArr[i]; // Offset 2.
        }
      }
    }
    return decimalStrArr;
  }

private:
  int32_t _scale;
  int64_t _precision;
  std::string _bigInt;
  int64_t _sign;
};

bool lldb_private::formatters::DecimalSummaryProvider(ValueObject &valobj, Stream &stream,
                                                      const TypeSummaryOptions &options)
{
  // The layout of the type is:
  // public struct Decimal {
  //    var _scale: Int32
  //    var _precision: Int64
  //    var _value: BigInt
  //    var _sign: Int64
  // }
  lldb::ValueObjectSP raw = valobj.GetNonSyntheticValue();
  if (!raw) {
    return false;
  }
  lldb::ValueObjectSP scale = raw->GetChildMemberWithName(ConstString("_scale"), true);
  lldb::ValueObjectSP precision = raw->GetChildMemberWithName(ConstString("_precision"), true);
  lldb::ValueObjectSP value = raw->GetChildMemberWithName(ConstString("_value"), true);
  lldb::ValueObjectSP sign = raw->GetChildMemberWithName(ConstString("_sign"), true);
  if (!scale || !precision || !value || !sign) {
    return false;
  }
  auto dscale = scale->GetValueAsSigned(INT64_MAX);
  std::string unscaleValStrArr = GetBigIntvalue(*value.get());
  if (dscale == 0) {
    stream.Printf("%s", unscaleValStrArr.c_str());
    return true;
  }
  auto dsign = sign->GetValueAsSigned(INT64_MAX);
  auto dprecision = precision->GetValueAsSigned(INT64_MAX);
  std::string tmp = DecimalToString(dscale, dprecision, unscaleValStrArr, dsign).ToString();
  stream.Printf("%s", tmp.c_str());
  return true;
}

class CangjieDateTime {
public:
    CangjieDateTime(int64_t epoch, int64_t offset): epoch(epoch), offset(offset) {}
  enum class MONTH {
    Invaild = 0,
    January = 1,
    February,
    March,
    April,
    May,
    June,
    July,
    August,
    September,
    October,
    November,
    December
  };
  int64_t epoch;
  int64_t offset;
  int64_t START_AD_YEAR = 1;
  int64_t MIN_YEAR = -999999999;
  int64_t MAX_YEAR = 999999999;
  int64_t SECS_PER_DAY = 86400;
  int64_t DAYS_PER_400YEARS = 146097;
  int64_t DAYS_PER_100YEARS = 36524;
  int64_t DAYS_PER_4YEARS = 1461;
  int64_t DAYS_OF_NORMAL_YEAR = 365;
  int64_t SECS_PER_HOUR = 3600;
  int64_t SECS_PER_MINUTE = 60;
  int64_t MAX_DAY_AD1 = (MAX_YEAR - 1) * 365 + (MAX_YEAR - 1) / 4 - (MAX_YEAR - 1) / 100 + (MAX_YEAR - 1) / 400 + 365;
  int64_t MIN_DAY_AD1 = -(MAX_DAY_AD1 + 366);
  int64_t SECS_OF_MIN_TO_AD1 = MIN_DAY_AD1 * SECS_PER_DAY;
  std::vector<int64_t>  DAYS_BEFORE = {
      0,
      31, // January, 31 days
      59, // until February, 31 + 28, 59 days
      90, // 90 days
      120, // 120 days
      151, // 151 days
      181, // 181 days
      212, // 212 days
      243, // 243 days
      273, // 273 days
      304, // 304 days
      334, // 334 days
      365  // 365 days
  };
  int64_t Year() {
    return std::get<0>(GetYearAndSecond());
  }
  MONTH Month() {
    return std::get<0>(GetMonthAndDay());
  }
  int64_t DayOfMonth() {
    return std::get<1>(GetMonthAndDay());
  }
  int64_t Hour() {
    int64_t second = std::get<1>(this->GetYearAndSecond());
    return (second % this->SECS_PER_DAY) / this->SECS_PER_HOUR;
  }
  int64_t Minute() {
    int64_t second = std::get<1>(this->GetYearAndSecond());
    return (second % this->SECS_PER_HOUR) / this->SECS_PER_MINUTE;
  }
  int64_t Second() {
    int64_t second = std::get<1>(this->GetYearAndSecond());
    return second % this->SECS_PER_MINUTE;
  }
private:
  static MONTH MonthOf(int64_t mon) {
    std::vector<MONTH> months = {
        MONTH::Invaild, MONTH::January, MONTH::February, MONTH::March, MONTH::April, MONTH::May, MONTH::June,
        MONTH::July, MONTH::August, MONTH::September, MONTH::October, MONTH::November, MONTH::December
    };
    if (mon >= 1 && mon <= 12) { // month 12
      return months[mon];
    }
    return MONTH::Invaild;
  }
  std::tuple<MONTH, int64_t> GetMonthAndDay() {
    auto year_second = this->GetYearAndSecond();
    return this->GetDate(std::get<0>(year_second), std::get<1>(year_second));
  }
  bool IsLeapYear(int64_t year) {
    return year % 4 == 0 and (year % 100 != 0 or year % 400 == 0); // 4 year, 100 year, 400 year
  }
  std::tuple<MONTH, int64_t> GetDate(int64_t year, int64_t sec) {
    int64_t days = int64_t(sec) / SECS_PER_DAY;
    if (IsLeapYear(year)) {
        if (days == DAYS_BEFORE[2]) { // Month 2
            return {MONTH::February, 29}; // Month 2 has 29 days.
        }
        if (days > DAYS_BEFORE[2]) { // Month 2
            days -= 1;
        }
    }
    int64_t idx = 0;
    int64_t day = 0;
    while (idx < DAYS_BEFORE.size()) {
        /* 1. In this case, the corresponding date is the 1st of the month idx + 1. */
        if (days == DAYS_BEFORE[idx]) {
            idx++;
            day = 1;
            break;
        }
        /* 2. In this case, the corresponding date is month idx. */
        if (days < DAYS_BEFORE[idx]) {
            day = days - DAYS_BEFORE[idx - 1] + 1;
            break;
        }
        idx++;
    }
    MONTH month = MonthOf(idx);
    return {month, day};
  }
  std::tuple<int64_t, int64_t> GetYearAndSecond() {
    auto [year, sec] = this->ToYearAndSecond(this->epoch + this->GetOffset());
    return {year, int64_t(sec)};
  }

  /**
  * Calculate year and second in year through seconds since year 1 A.D.
  */
  std::tuple<int64_t, int64_t> ToYearAndSecond(int64_t sec) {
      int64_t year = START_AD_YEAR;
      int64_t seconds = sec;
      if (seconds < 0) {
          seconds -= SECS_OF_MIN_TO_AD1;
          year = MIN_YEAR;
      }
      int64_t days = seconds / SECS_PER_DAY;
      int64_t restSec = seconds % SECS_PER_DAY;

      int64_t times = days / DAYS_PER_400YEARS;
      year += 400 * times; // 400 years
      days %= DAYS_PER_400YEARS;

      times = days / DAYS_PER_100YEARS;
      times -= times >> 2; // 2
      year += 100 * times; // 100 year
      days -= DAYS_PER_100YEARS * times;

      times = days / DAYS_PER_4YEARS;
      year += 4 * times; // 4 year
      days %= DAYS_PER_4YEARS;

      times = days / DAYS_OF_NORMAL_YEAR;
      times -= times >> 2; // 2
      year += times;
      days -= DAYS_OF_NORMAL_YEAR * times;

      int64_t secInYear = restSec + days * SECS_PER_DAY;
      return {year, secInYear};
  }
  int64_t GetOffset() {
    return this->offset;
  }
};

bool lldb_private::formatters::DateTimeSummaryProvider(
    ValueObject &valobj, Stream &stream, const TypeSummaryOptions &options)
{
  // The layout of the type is:
  // public struct DateTime {
  //   let d: Duration
  //   let tz: TimeZone
  //  }
  lldb::ValueObjectSP raw = valobj.GetNonSyntheticValue();
  if (!raw) {
    return false;
  }
  lldb::ValueObjectSP duration = raw->GetChildMemberWithName(ConstString("d"), true);
  if (!duration) {
    return false;
  }
  lldb::ValueObjectSP sec = duration->GetChildMemberWithName(ConstString("sec"), true);
  if (!sec) {
    return false;
  }
  int64_t secVal = sec->GetValueAsSigned(INT64_MAX);
  lldb::ValueObjectSP timeZone = raw->GetChildMemberWithName(ConstString("tz"), true);
  if (!timeZone) {
    return false;
  }
  lldb::ValueObjectSP zoneId = timeZone->GetChildMemberWithName(ConstString("zoneId"), true);
  if (!zoneId) {
    return false;
  }
  auto str_zoneId = zoneId->GetSummaryAsCString();
  CangjieDateTime date = CangjieDateTime(secVal, 0);
  if (date.Year() > 999 || date.Year() < -999) { // 999, -999 year
    stream.Printf("%d-%02d-%02dT%02d:%02d:%02dZ", date.Year(),
                  date.Month(), date.DayOfMonth(), date.Hour(), date.Minute(), date.Second());
  } else if (date.Year() < 0) {
    stream.Printf("%05d-%02d-%02dT%02d:%02d:%02dZ", date.Year(),
                  date.Month(), date.DayOfMonth(), date.Hour(), date.Minute(), date.Second());
  } else {
    stream.Printf("%04d-%02d-%02dT%02d:%02d:%02dZ", date.Year(),
                  date.Month(), date.DayOfMonth(), date.Hour(), date.Minute(), date.Second());
  }
  return true;
}

bool lldb_private::formatters::Enum2SummaryProvider(
    ValueObject &valobj, Stream &stream, const TypeSummaryOptions &) {
  if (valobj.IsPointerType()) {
    auto ptr = valobj.GetValueAsUnsigned(UINT64_MAX);
    if (ptr == 0) {
      std::string field_name;
      auto enum_type = valobj.GetCompilerType().GetPointeeType();
      auto constructor = enum_type.GetFieldAtIndex(0, field_name, nullptr, nullptr, nullptr);
      ConstString none;
      constructor.ForEachEnumerator(
        [&none](const CompilerType &integer_type, ConstString name, const llvm::APSInt &value) -> bool {
          if (name.GetStringRef().contains("N$_")) {
            none = name;
          }
          return true;
        }
      );
      stream.Printf("%s", none.GetStringRef().ltrim("N$_").data());
      return true;
    } else {
      Status err;
      auto val = valobj.Dereference(err);
      if (!err.Fail()) {
        val->SetIsOptionlikeReference(true);
        DumpWithoutPrefix(val, stream);
        return true;
      }
    }
  }

  lldb::ValueObjectSP raw = valobj.GetNonSyntheticValue();
  if (!raw) {
    return false;
  }
  if (raw->IsOptionlikeReference()) {
    return false;
  }
  auto constructor = raw->GetChildMemberWithName(ConstString("constructor"), true);
  if (constructor == nullptr) {
    return false;
  }

  auto value = constructor->GetValueAsCString();
  if (value != nullptr) {
    if (std::string(value).find("N$_") == 0) {
      stream.Printf("%s", std::string(value).substr(std::string("N$_").size()).c_str());
      return true;
    }
  }

  return false;
}
