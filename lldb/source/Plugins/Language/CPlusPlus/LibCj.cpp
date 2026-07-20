//===-- LibCj.cpp ---------------------------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#include "LibCj.h"
#include "lldb/DataFormatters/FormattersHelpers.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters;

namespace lldb_private {
namespace formatters {
class CjArraySyntheticFrontEnd : public SyntheticChildrenFrontEnd {
public:
  explicit CjArraySyntheticFrontEnd(ValueObjectSP valobj_sp);
  ~CjArraySyntheticFrontEnd() = default;
  size_t CalculateNumChildren() override { return m_len ? m_len->GetValueAsUnsigned(0) : 0; }
  ValueObjectSP GetChildAtIndex(size_t idx) override;
  bool Update() override;
  bool MightHaveChildren() override { return true; }
  size_t GetIndexOfChildWithName(ConstString name) override { return ExtractIndexFromString(name.GetCString()); }

private:
  ValueObjectSP GetSliceValue(size_t idx);

private:
  ValueObjectSP m_elements;
  ValueObjectSP m_start;
  ValueObjectSP m_len;
};

class CjArrayListSyntheticFrontEnd : public SyntheticChildrenFrontEnd {
public:
  explicit CjArrayListSyntheticFrontEnd(ValueObjectSP valobj_sp);
  ~CjArrayListSyntheticFrontEnd() = default;
  size_t CalculateNumChildren() override { return m_size; }
  ValueObjectSP GetChildAtIndex(size_t idx) override;
  bool Update() override;
  bool MightHaveChildren() override { return true; }
  size_t GetIndexOfChildWithName(ConstString name) override { return m_data->GetIndexOfChildWithName(name); }

private:
  uint64_t m_size;
  std::unique_ptr<CjArraySyntheticFrontEnd> m_data;
};

class CjHashMapSyntheticFrontEnd : public SyntheticChildrenFrontEnd {
public:
  explicit CjHashMapSyntheticFrontEnd(ValueObjectSP valobj_sp);
  ~CjHashMapSyntheticFrontEnd() = default;
  size_t CalculateNumChildren() override;
  ValueObjectSP GetChildAtIndex(size_t idx) override;
  bool Update() override;
  bool MightHaveChildren() override { return true; }
  size_t GetIndexOfChildWithName(ConstString name) override { return ExtractIndexFromString(name.GetCString()); }
  void SetIsInternalType(bool internal) { m_internal = internal; }
  std::string GetChildName(ValueObjectSP value);
  llvm::StringRef GetBasicTypeName(ValueObjectSP value);
  bool IsBasicType(ValueObjectSP value);
private:
  int64_t m_freeOffset = 0;
  ValueObjectSP m_appendIndex;
  ValueObjectSP m_freeSize;
  std::unique_ptr<CjArraySyntheticFrontEnd> m_data;
  bool m_internal = false;
};

class CjHashSetSyntheticFrontEnd : public SyntheticChildrenFrontEnd {
public:
  explicit CjHashSetSyntheticFrontEnd(ValueObjectSP valobj_sp);
  ~CjHashSetSyntheticFrontEnd() = default;
  size_t CalculateNumChildren() override;
  ValueObjectSP GetChildAtIndex(size_t idx) override;
  bool Update() override;
  bool MightHaveChildren() override { return true; }
  size_t GetIndexOfChildWithName(ConstString name) override { return ExtractIndexFromString(name.GetCString()); }

private:
  std::unique_ptr<CjHashMapSyntheticFrontEnd> m_map;
};

class CjTupleSyntheticFrontEnd : public SyntheticChildrenFrontEnd {
public:
  explicit CjTupleSyntheticFrontEnd(ValueObjectSP valobj_sp) : SyntheticChildrenFrontEnd(*valobj_sp) {}
  ~CjTupleSyntheticFrontEnd() = default;
  size_t CalculateNumChildren() override;
  ValueObjectSP GetChildAtIndex(size_t idx) override;
  bool Update() override { return false; }
  bool MightHaveChildren() override { return true; }
  size_t GetIndexOfChildWithName(ConstString name) override { return ExtractIndexFromString(name.GetCString()); }
};

class CjEnumSyntheticFrontEnd : public SyntheticChildrenFrontEnd {
public:
  explicit CjEnumSyntheticFrontEnd(ValueObjectSP valobj_sp);
  ~CjEnumSyntheticFrontEnd() = default;
  size_t CalculateNumChildren() override { return m_enumClass ? m_enumClass->GetNumChildren() : 0; }
  ValueObjectSP GetChildAtIndex(size_t idx) override;
  bool Update() override;
  bool MightHaveChildren() override { return m_enumClass != nullptr; }
  size_t GetIndexOfChildWithName(ConstString name) override { return m_backend.GetIndexOfChildWithName(name); }
  ConstString GetSyntheticTypeName() override;
  ValueObjectSP GetSyntheticValue() override { return m_enumClass; }

private:
  lldb::ValueObjectSP m_enumClass;
};

class CjEnum2SyntheticFrontEnd : public SyntheticChildrenFrontEnd {
public:
  explicit CjEnum2SyntheticFrontEnd(ValueObjectSP valobj_sp);
  ~CjEnum2SyntheticFrontEnd() = default;
  size_t CalculateNumChildren() override { return m_data == nullptr ? 0 : 2; }
  ValueObjectSP GetChildAtIndex(size_t idx) override;
  bool Update() override;
  bool MightHaveChildren() override { return m_backend.MightHaveChildren(); }
  size_t GetIndexOfChildWithName(ConstString name) override { return m_backend.GetIndexOfChildWithName(name); }
  ConstString GetSyntheticTypeName() override;
private:
  ValueObjectSP m_some_constructor;
  ValueObjectSP m_data;
};

class CjEnum3SyntheticFrontEnd : public SyntheticChildrenFrontEnd {
public:
  explicit CjEnum3SyntheticFrontEnd(ValueObjectSP valobj_sp);
  ~CjEnum3SyntheticFrontEnd() = default;
  size_t CalculateNumChildren() override { return m_data != nullptr ? m_data->GetNumChildren() : 0; }
  ValueObjectSP GetChildAtIndex(size_t idx) override;
  bool Update() override;
  bool MightHaveChildren() override { return m_data != nullptr ? m_data->MightHaveChildren() : false; }
  size_t GetIndexOfChildWithName(ConstString name) override { return m_backend.GetIndexOfChildWithName(name); }
  ConstString GetSyntheticTypeName() override;
private:
  ValueObjectSP m_data;
};

class CjComplexAsSimpleSyntheticFrontEnd : public SyntheticChildrenFrontEnd {
public:
  explicit CjComplexAsSimpleSyntheticFrontEnd(ValueObjectSP valobj_sp) : SyntheticChildrenFrontEnd(*valobj_sp) {}
  ~CjComplexAsSimpleSyntheticFrontEnd() = default;
  size_t CalculateNumChildren() override { return 0; }
  ValueObjectSP GetChildAtIndex(size_t idx) override { return nullptr; }
  bool Update() override { return false; }
  bool MightHaveChildren() override { return m_backend.MightHaveChildren(); }
  size_t GetIndexOfChildWithName(ConstString name) override { return 0; }
  ValueObjectSP GetSyntheticValue() override { return m_backend.GetSP(); }
};

class CjClassSyntheticFrontEnd : public SyntheticChildrenFrontEnd {
public:
  explicit CjClassSyntheticFrontEnd(ValueObjectSP valobj_sp) : SyntheticChildrenFrontEnd(*valobj_sp) {}
  ~CjClassSyntheticFrontEnd() = default;
  size_t CalculateNumChildren() override;
  ValueObjectSP GetChildAtIndex(size_t idx) override;
  bool Update() override { return false; }
  bool MightHaveChildren() override { return m_backend.MightHaveChildren(); }
  size_t GetIndexOfChildWithName(ConstString name) override { return m_backend.GetIndexOfChildWithName(name); }
  ConstString GetSyntheticTypeName() override;
};

class CjOptionSyntheticFrontEnd : public SyntheticChildrenFrontEnd {
public:
  explicit CjOptionSyntheticFrontEnd(ValueObjectSP valobj_sp);
  ~CjOptionSyntheticFrontEnd() = default;
  size_t CalculateNumChildren() override;
  ValueObjectSP GetChildAtIndex(size_t idx) override;
  bool Update() override;
  bool MightHaveChildren() override;
  size_t GetIndexOfChildWithName(ConstString name) override { return 0; }
  ConstString GetSyntheticTypeName() override;
private:
  bool m_hasvalue = false;
  ValueObjectSP m_some =  nullptr;
  bool m_children = false;
};

static std::string DeletePrefixOfTypeName(std::string type_name, std::string prefix) {
  auto pos = type_name.find(prefix);
  if (pos != std::string::npos) {
    return type_name.erase(type_name.find(prefix), prefix.size());
  }
  return type_name;
}

bool CjOptionSyntheticFrontEnd::MightHaveChildren() {
  if (m_children == false || m_some == nullptr || m_hasvalue == false) {
    return false;
  }

  return true;
}

size_t CjOptionSyntheticFrontEnd::CalculateNumChildren() {
  if (!m_hasvalue || m_some == nullptr || !m_hasvalue) {
    return 0;
  }

  return m_some->GetNumChildren();
}

ValueObjectSP CjOptionSyntheticFrontEnd::GetChildAtIndex(size_t idx) {
  if (!m_hasvalue) {
    return nullptr;
  }
  return m_some->GetChildAtIndex(idx, true);
}

bool CjOptionSyntheticFrontEnd::Update() {
  ValueObjectSP non_synth_valobj = m_backend.GetNonSyntheticValue();
  if (!non_synth_valobj) {
    m_some = nullptr;
    return false;
  }

  if (non_synth_valobj->IsPointerType()) {
    Status err;
    non_synth_valobj = non_synth_valobj->Dereference(err);
    if (err.Fail()) {
      return false;
    }
  }

  ValueObjectSP value = non_synth_valobj->GetChildMemberWithName(ConstString("val"), true);
  if (value == nullptr) {
    value = non_synth_valobj->GetChildMemberWithName(ConstString("Recursive-val"), true);
  }

  if (value == nullptr) {
    return false;
  }

  if (value->IsPointerType()) {
    Status err;
    value = value->Dereference(err);
  }
  if (!value) {
    return false;
  }
  auto dynamic = value->GetDynamicValue(eDynamicDontRunTarget);
  if (dynamic != nullptr) {
    value = dynamic;
  }

  if (value->HasSyntheticValue()) {
    m_some = value->GetSyntheticValue();
  } else {
    m_some = value;
  }

  if (m_some != nullptr) {
    m_hasvalue = true;
    m_children = (m_some->GetNumChildren() > 0);
  }

  return false;
}

CjOptionSyntheticFrontEnd::CjOptionSyntheticFrontEnd(ValueObjectSP valobj_sp)
    : SyntheticChildrenFrontEnd(*valobj_sp)  {
  Update();
}

ConstString CjOptionSyntheticFrontEnd::GetSyntheticTypeName() {
  auto type_name = m_backend.GetDisplayTypeName().GetStringRef();
  return ConstString(type_name.rtrim(" *").str());
}

class CjVArraySyntheticFrontEnd : public SyntheticChildrenFrontEnd {
public:
  explicit CjVArraySyntheticFrontEnd(ValueObjectSP valobj_sp);
  ~CjVArraySyntheticFrontEnd() = default;
  size_t CalculateNumChildren() override { return m_backend.GetNumChildren(); }
  ValueObjectSP GetChildAtIndex(size_t idx) override { return m_backend.GetChildAtIndex(idx, true); };
  bool Update() override { return false; };
  bool MightHaveChildren() override { return true; }
  size_t GetIndexOfChildWithName(ConstString name) override { return m_backend.GetIndexOfChildWithName(name); }
};

} // namespace formatters
} // namespace lldb_private

CjVArraySyntheticFrontEnd::CjVArraySyntheticFrontEnd(ValueObjectSP valobj_sp)
    : SyntheticChildrenFrontEnd(*valobj_sp)  {
  Update();
}

CjArraySyntheticFrontEnd::CjArraySyntheticFrontEnd(ValueObjectSP valobj_sp)
    : SyntheticChildrenFrontEnd(*valobj_sp), m_elements(nullptr), m_start(nullptr), m_len(nullptr) {
  Update();
}

ValueObjectSP CjArraySyntheticFrontEnd::GetChildAtIndex(size_t idx) {
  if (idx >= UINT32_MAX - 1) {
    return GetSliceValue(idx);
  }

  if (!m_elements || !m_start || (idx >= CalculateNumChildren())) {
    return ValueObjectSP();
  }

  CompilerType element_type = m_elements->GetCompilerType();
  llvm::Optional<uint64_t> size = element_type.GetByteSize(nullptr);
  uint64_t element_size = size ? *size : 0;
  addr_t addr = m_elements->GetAddressOf() + (m_start->GetValueAsUnsigned(0) + idx) * element_size;
  ValueObjectSP valobj_sp = CreateValueObjectFromAddress(llvm::StringRef(llvm::formatv("[{0}]", idx)),
                                                         addr, m_elements->GetExecutionContextRef(),
                                                         element_type);
  if (valobj_sp->IsPointerType()) {
    Status error;
    valobj_sp = valobj_sp->Dereference(error);
    valobj_sp->SetName(ConstString(valobj_sp->GetName().GetStringRef().ltrim('*').str()));
  }

  return valobj_sp;
}

bool CjArraySyntheticFrontEnd::Update() {
  ValueObjectSP rawptr = m_backend.GetChildMemberWithName(ConstString("rawptr"), true);
  if (rawptr) {
    m_elements = rawptr->GetChildMemberWithName(ConstString("elements"), true);
  }

  m_start = m_backend.GetChildMemberWithName(ConstString("start"), true);
  m_len = m_backend.GetChildMemberWithName(ConstString("len"), true);

  return false;
}

ValueObjectSP CjArraySyntheticFrontEnd::GetSliceValue(size_t idx) {
  if (!m_start || !m_len) {
    return ValueObjectSP();
  }

  size_t left = idx % (UINT32_MAX + 1ULL);
  size_t right = idx / (UINT32_MAX + 1ULL);

  auto start = m_start->GetValueAsUnsigned(0);
  auto len = m_len->GetValueAsUnsigned(0);

  if (left == UINT32_MAX - 1) {
    left = 0;
    right = 0;
  } else if (right == UINT32_MAX) {
    right = start + len;
  }

  if ((left > right) || (left < start) || (right > start + len)) {
    return ValueObjectSP();
  }

  Status error;
  m_start->SetValueFromCString(std::to_string(left).c_str(), error);
  m_len->SetValueFromCString(std::to_string(right - left).c_str(), error);

  DataExtractor data;
  m_backend.GetData(data, error);

  ValueObjectSP valobj_sp = CreateValueObjectFromData(llvm::StringRef(llvm::formatv("[{0}..{1}]", left, right)), data,
    m_backend.GetExecutionContextRef(), m_backend.GetCompilerType());

  m_start->SetValueFromCString(std::to_string(start).c_str(), error);
  m_len->SetValueFromCString(std::to_string(len).c_str(), error);

  return valobj_sp;
}

CjArrayListSyntheticFrontEnd::CjArrayListSyntheticFrontEnd(ValueObjectSP valobj_sp)
    : SyntheticChildrenFrontEnd(*valobj_sp), m_size(0), m_data(nullptr) {
  Update();
}

bool CjArrayListSyntheticFrontEnd::Update() {
  ValueObjectSP data = m_backend.GetChildMemberWithName(ConstString("myData"), true);
  ValueObjectSP size = m_backend.GetChildMemberWithName(ConstString("mySize"), true);
  if (data) {
    m_data = std::make_unique<CjArraySyntheticFrontEnd>(data);
  }
  if (size) {
    m_size = size->GetValueAsUnsigned(0);
  }

  return false;
}

ValueObjectSP CjArrayListSyntheticFrontEnd::GetChildAtIndex(size_t idx) {
  if (idx >= m_size || !m_data) {
    return ValueObjectSP();
  }
  return m_data->GetChildAtIndex(idx);
}

ValueObjectSP CjTupleSyntheticFrontEnd::GetChildAtIndex(size_t idx) {
  ValueObjectSP valobj_sp = m_backend.GetChildAtIndex(idx, true);
  std::string name = llvm::formatv("[{0}]", idx).str();
  valobj_sp->SetName(ConstString(name.c_str()));
  if (valobj_sp->IsPointerType()) {
    Status error;
    valobj_sp = valobj_sp->Dereference(error);
    valobj_sp->SetName(ConstString(valobj_sp->GetName().GetStringRef().ltrim('*').str()));
  }

  return valobj_sp;
}

size_t CjTupleSyntheticFrontEnd::CalculateNumChildren() {
  return m_backend.GetNumChildren();
}

ConstString CjClassSyntheticFrontEnd::GetSyntheticTypeName() {
  std::string raw_display = m_backend.GetDisplayTypeName().GetStringRef().rtrim(" *").str();
  auto display = DeletePrefixOfTypeName(raw_display, "E0$");

  return ConstString(display.c_str());
}

size_t CjClassSyntheticFrontEnd::CalculateNumChildren() {
  // Class is internal type means a child of cangjie HashMap.
  // HashMap only need 2 children
  return m_backend.IsInternalType()? 2 : m_backend.GetNumChildren();
}

CjEnumSyntheticFrontEnd::CjEnumSyntheticFrontEnd(ValueObjectSP valobj_sp)
    : SyntheticChildrenFrontEnd(*valobj_sp) {
  Update();
}

bool CjEnumSyntheticFrontEnd::Update() {
  for (size_t i = 0; i < m_backend.GetCompilerType().GetNumDirectBaseClasses(); i++) {
    ValueObjectSP ctor = m_backend.GetChildAtIndex(i, true);
    if (!ctor) {
      continue;
    }
    auto type = ctor->GetChildMemberWithName(ConstString("constructor"), true);
    if (!type) {
      continue;
    }
    auto id = type->GetValueAsUnsigned(0);
    auto real_ctor = m_backend.GetChildAtIndex(id, true);
    if (!real_ctor) {
      continue;
    }
    DataExtractor data;
    Status err;
    real_ctor->GetData(data, err);
    m_enumClass = CreateValueObjectFromData("EnumClass", data, real_ctor->GetExecutionContextRef(),
                                            real_ctor->GetCompilerType());
    break;
  }

  if (m_enumClass != nullptr) {
    auto constructor = m_enumClass->GetChildMemberWithName(ConstString("constructor"), true);
    if (constructor != nullptr) {
      auto value = constructor->GetValueAsCString();
      if (value && std::string(value) == std::string("Some")) {
        m_enumClass = m_enumClass->GetChildMemberWithName(ConstString("arg_1"), true);
      }
    }
  }

  return false;
}

ConstString CjEnumSyntheticFrontEnd::GetSyntheticTypeName() {
  std::string raw_display = m_backend.GetDisplayTypeName().GetStringRef().rtrim(" *").str();
  auto display = DeletePrefixOfTypeName(raw_display, "E1$");
  display = DeletePrefixOfTypeName(display, "Enum$");
  return ConstString(display.c_str());
}

ValueObjectSP CjEnumSyntheticFrontEnd::GetChildAtIndex(size_t idx) {
  if (!m_enumClass) {
    return ValueObjectSP();
  }

  ValueObjectSP child = m_enumClass->GetChildAtIndex(idx, true);
  if (child && child->IsPointerType() && child->GetValueAsUnsigned(0) != 0) {
    Status error;
    child = child->Dereference(error);
    child->SetName(ConstString(child->GetName().GetStringRef().ltrim('*').str()));
  }

  return child;
}

ValueObjectSP CjEnum2SyntheticFrontEnd::GetChildAtIndex(size_t idx) {
  return idx == 0 ? m_some_constructor : m_data;
}

CjEnum2SyntheticFrontEnd::CjEnum2SyntheticFrontEnd(ValueObjectSP valobj_sp)
    : SyntheticChildrenFrontEnd(*valobj_sp) {
  Update();
}

bool CjEnum2SyntheticFrontEnd::Update() {
    std::string field_name;
    auto enum_type = m_backend.GetCompilerType();
    auto constructor = enum_type.GetFieldAtIndex(0, field_name, nullptr, nullptr, nullptr);
    llvm::APSInt enum_value;
    constructor.ForEachEnumerator(
      [&enum_value, this](const CompilerType &integer_type, ConstString name, const llvm::APSInt &value) -> bool {
        if (!name.GetStringRef().contains("N$_")) {
          enum_value = value;
        }
        return true;
      }
    );
    if (m_backend.IsPointerType()) {
      return false;
    }
    uint32_t value = (uint32_t)enum_value.getSExtValue();
    lldb::WritableDataBufferSP buffer_sp = std::make_shared<DataBufferHeap>(sizeof(uint32_t), 0);
    auto data_ptr = reinterpret_cast<uint32_t *>(buffer_sp->GetBytes());
    *data_ptr = value;
    DataExtractor extractor(buffer_sp, m_backend.GetProcessSP()->GetByteOrder(),
                            m_backend.GetProcessSP()->GetAddressByteSize());
    m_some_constructor = CreateValueObjectFromData(llvm::StringRef("constructor"), extractor,
      m_backend.GetExecutionContextRef(), constructor);
    m_data = m_backend.GetChildMemberWithName(ConstString("val"), true);
    if (m_data != nullptr) {
      m_data->SetName(ConstString("arg_1"));
    }
  return false;
}

ConstString CjEnum2SyntheticFrontEnd::GetSyntheticTypeName() {
  std::string raw_display = m_backend.GetDisplayTypeName().GetStringRef().rtrim(" *").str();
  auto display = DeletePrefixOfTypeName(raw_display, "E2$");
  return ConstString(display.c_str());
}

CjEnum3SyntheticFrontEnd::CjEnum3SyntheticFrontEnd(ValueObjectSP valobj_sp)
    : SyntheticChildrenFrontEnd(*valobj_sp) {
  Update();
}

bool CjEnum3SyntheticFrontEnd::Update() {
  auto enum_id = m_backend.GetChildAtIndex(0, true);
  if (enum_id == nullptr) {
    return false;
  }
  auto constructor = enum_id->GetChildMemberWithName(ConstString("constructor"), true);
  if (constructor == nullptr) {
    return false;
  }
  auto id = constructor->GetValueAsUnsigned(UINT64_MAX);
  constructor = m_backend.GetChildAtIndex(id, true);
  if (constructor != nullptr) {
    DataExtractor data;
    Status error;
    m_backend.GetData(data, error);
    m_data = CreateValueObjectFromData(llvm::StringRef("enum_class"), data,
      m_backend.GetExecutionContextRef(), constructor->GetCompilerType());
  }

  return false;
}

ConstString CjEnum3SyntheticFrontEnd::GetSyntheticTypeName() {
  std::string raw_display = m_backend.GetDisplayTypeName().GetStringRef().rtrim(" *").str();
  auto display = DeletePrefixOfTypeName(raw_display, "E3$");
  return ConstString(display.c_str());
}

ValueObjectSP CjEnum3SyntheticFrontEnd::GetChildAtIndex(size_t idx) {
  return m_data->GetChildAtIndex(idx, true);
}

ValueObjectSP CjClassSyntheticFrontEnd::GetChildAtIndex(size_t idx) {
  if (m_backend.IsInternalType()) {
    // class is internal type means a child of cangjie HashMap.
    //     struct HashMapEntry<K, V> {
    //          let hashCode: Int64
    //          let next: Int64
    //          let key: K
    //          let value: V
    //        }
    // And idx 0 require idx 2(key), idx 1 require idx 3(value)
    auto child_sp = m_backend.GetChildAtIndex(idx + 2, true);
    if (child_sp != nullptr && child_sp->IsPointerType()) {
      Status error;
      child_sp = child_sp->Dereference(error);
      child_sp->SetName(ConstString(child_sp->GetName().GetStringRef().ltrim("*").str()));
    }
    return child_sp;
  }

  ValueObjectSP child_sp = m_backend.GetChildAtIndex(idx, true);
  if (child_sp && (idx == 0)) {
    std::string name = child_sp->GetName().GetCString();
    size_t pos = name.find("::");
    while (pos != std::string::npos) {
      name.replace(pos, strlen("::"), ".");
      pos = name.find("::", pos);
    }
    child_sp->SetName(ConstString(name.c_str()));
  }

  if (child_sp && child_sp->IsPointerType() && child_sp->GetTypeName() != "char *") {
    Status error;
    auto deref = child_sp->Dereference(error);
    if (error.Fail() || !deref) {
      return child_sp;
    }
    auto addr = deref->GetAddressOf();
    if (addr == 0 || addr == LLDB_INVALID_ADDRESS) {
      return child_sp;
    }
    deref->SetName(ConstString(child_sp->GetName().GetStringRef().ltrim('*').str()));
    return deref;
  }

  return child_sp;
}

CjHashMapSyntheticFrontEnd::CjHashMapSyntheticFrontEnd(ValueObjectSP valobj_sp)
    : SyntheticChildrenFrontEnd(*valobj_sp), m_appendIndex(nullptr), m_freeSize(nullptr), m_data(nullptr) {
  Update();
}

bool CjHashMapSyntheticFrontEnd::IsBasicType(ValueObjectSP value) {
  auto type = value->GetCompilerType();
  if (type.GetTypeClass() == lldb::eTypeClassTypedef) {
    type = type.GetTypedefedType();
  }
  if (type.GetTypeClass() == lldb::eTypeClassBuiltin) {
    return true;
  }
  if (type.GetTypeClass() == lldb::eTypeClassStruct) {
    auto typeName = type.GetTypeName().GetStringRef();
    if (typeName.find("std.core::String") == 0) {
      return true;
    }
    if (typeName.find("std.math.numeric::Decimal") == 0) {
      return true;
    }
    if (typeName.find("std.time::DateTime") == 0) {
      return true;
    }
  }
  return false;
}

llvm::StringRef CjHashMapSyntheticFrontEnd::GetBasicTypeName(ValueObjectSP value) {
  auto type = value->GetCompilerType();
  auto typeName = type.GetTypeName().GetStringRef();
  if (typeName == "Rune" || typeName == "Int8" || typeName == "UInt8") {
    return value->GetSummaryAsCString();
  }
  if (type.GetTypeClass() == lldb::eTypeClassStruct) {
    // String
    return value->GetSummaryAsCString();
  }
  // integer, float, bool
  return value->GetValueAsCString();
}

std::string CjHashMapSyntheticFrontEnd::GetChildName(ValueObjectSP value) {
  // HashMap is internal type means a child of cangjie HashSet
  // then we only need one child of value(the type of value is class)
  // and this child idx is 2(key), child idx is 3(value).
  ValueObjectSP child_key_sp = value->GetChildAtIndex(2, true);
  ValueObjectSP child_val_sp = value->GetChildAtIndex(3, true);
  if (!child_key_sp || !child_val_sp) {
    return "";
  }
  if (!IsBasicType(child_key_sp)) {
    return "";
  }
  llvm::StringRef key_str = GetBasicTypeName(child_key_sp);
  if (!IsBasicType(child_val_sp)) {
    std::string name = "[" + key_str.str() +"]";
    return name;
  }
  llvm::StringRef val_str = GetBasicTypeName(child_val_sp);
  std::string name = "[" + key_str.str() + " -> "+ val_str.str() + "]";
  return name;
}

size_t CjHashMapSyntheticFrontEnd::CalculateNumChildren() {
  if (!m_appendIndex || !m_freeSize) {
    return 0;
  }

  int64_t appendIndex = m_appendIndex->GetValueAsSigned(INT64_MAX);
  int64_t freeSize = m_freeSize->GetValueAsSigned(INT64_MAX);
  if (appendIndex == INT64_MAX || freeSize == INT64_MAX || appendIndex < freeSize) {
    return 0;
  }

  return appendIndex - freeSize;
}

ValueObjectSP CjHashMapSyntheticFrontEnd::GetChildAtIndex(size_t idx) {
  if (!m_data) {
    return ValueObjectSP();
  }
  ValueObjectSP value = m_data->GetChildAtIndex(idx + m_freeOffset);
  if (value) {
    while (true) {
      int64_t isfree = value->GetChildAtIndex(0, true)->GetValueAsSigned(INT64_MAX);
      if (isfree < 0) {
        m_freeOffset += 1;
        value = m_data->GetChildAtIndex(idx + m_freeOffset);
        continue;
      }
      break;
    }

    if (m_internal) {
      std::string name = llvm::formatv("[{0}]", idx).str();
      // HashMap is internal type means a child of cangjie HashSet
      // then we only need one child of value(the type of value is class)
      // and this child idx is 2(key)
      value = value->GetChildAtIndex(2, true);
      value->SetName(ConstString(name.c_str()));
      return value;
    }

    // Mark the value(class type) is a child of HashMap.
    value->SetIsInternalType(true);
    // idx =>  Bob -> 123 for metaDsl.
    if (value->IsInternalType()) {
      auto name = GetChildName(value);
      if (!name.empty()) {
        value->SetName(ConstString(name));
      }
    }
    return value;
  }

  return ValueObjectSP();
}

bool CjHashMapSyntheticFrontEnd::Update() {
  m_appendIndex = m_backend.GetChildMemberWithName(ConstString("appendIndex"), true);
  m_freeSize = m_backend.GetChildMemberWithName(ConstString("freeSize"), true);
  auto entries = m_backend.GetChildMemberWithName(ConstString("entries"), true);
  if (entries) {
    m_data = std::make_unique<CjArraySyntheticFrontEnd>(entries);
  }
  return false;
}

CjHashSetSyntheticFrontEnd::CjHashSetSyntheticFrontEnd(ValueObjectSP valobj_sp)
    : SyntheticChildrenFrontEnd(*valobj_sp), m_map(nullptr) {
  Update();
}

bool CjHashSetSyntheticFrontEnd::Update() {
  m_map = std::make_unique<CjHashMapSyntheticFrontEnd>(m_backend.GetChildMemberWithName(ConstString("myMap"), true));
  if (m_map) {
    // Mark the map(HashMap) is a child of HashSet.
    m_map->SetIsInternalType(true);
    return true;
  }
  return false;
}

ValueObjectSP CjHashSetSyntheticFrontEnd::GetChildAtIndex(size_t idx) {
  ValueObjectSP value = m_map->GetChildAtIndex(idx);
  if (value) {
    return value;
  }

  return ValueObjectSP();
}

size_t CjHashSetSyntheticFrontEnd::CalculateNumChildren() {
  size_t size =  m_map->CalculateNumChildren();
  return size;
}

SyntheticChildrenFrontEnd *lldb_private::formatters::CjArraySyntheticFrontEndCreator(
    CXXSyntheticChildren *, ValueObjectSP valobj_sp) {
  return valobj_sp ? new CjArraySyntheticFrontEnd(valobj_sp) : nullptr;
}

SyntheticChildrenFrontEnd *lldb_private::formatters::CjTupleSyntheticFrontEndCreator(
    CXXSyntheticChildren *, ValueObjectSP valobj_sp) {
  return valobj_sp ? new CjTupleSyntheticFrontEnd(valobj_sp) : nullptr;
}

SyntheticChildrenFrontEnd *lldb_private::formatters::CjEnumSyntheticFrontEndCreator(
    CXXSyntheticChildren *, ValueObjectSP valobj_sp) {
  return valobj_sp ? new CjEnumSyntheticFrontEnd(valobj_sp) : nullptr;
}

SyntheticChildrenFrontEnd *lldb_private::formatters::CjE2SyntheticFrontEndCreator(
    CXXSyntheticChildren *, ValueObjectSP valobj_sp) {
  return valobj_sp ? new CjEnum2SyntheticFrontEnd(valobj_sp) : nullptr;
}

SyntheticChildrenFrontEnd *lldb_private::formatters::CjE3SyntheticFrontEndCreator(
    CXXSyntheticChildren *, ValueObjectSP valobj_sp) {
  return valobj_sp ? new CjEnum3SyntheticFrontEnd(valobj_sp) : nullptr;
}

SyntheticChildrenFrontEnd *lldb_private::formatters::CjComplexAsSimpleSyntheticFrontEndCreator(
    CXXSyntheticChildren *, ValueObjectSP valobj_sp) {
  return valobj_sp ? new CjComplexAsSimpleSyntheticFrontEnd(valobj_sp) : nullptr;
}

SyntheticChildrenFrontEnd *lldb_private::formatters::CjClassSyntheticFrontEndCreator(
    CXXSyntheticChildren *, ValueObjectSP valobj_sp) {
  return valobj_sp ? new CjClassSyntheticFrontEnd(valobj_sp) : nullptr;
}

SyntheticChildrenFrontEnd *lldb_private::formatters::CjArrayListSyntheticFrontEndCreator(
    CXXSyntheticChildren *, ValueObjectSP valobj_sp) {
  return valobj_sp ?
    new CjArrayListSyntheticFrontEnd(valobj_sp) : nullptr;
}

SyntheticChildrenFrontEnd *lldb_private::formatters::CjHashMapSyntheticFrontEndCreator(
    CXXSyntheticChildren *, ValueObjectSP valobj_sp) {
  return valobj_sp ? new CjHashMapSyntheticFrontEnd(valobj_sp) : nullptr;
}

SyntheticChildrenFrontEnd *lldb_private::formatters::CjHashSetSyntheticFrontEndCreator(
    CXXSyntheticChildren *, ValueObjectSP valobj_sp) {
  return valobj_sp ? new CjHashSetSyntheticFrontEnd(valobj_sp) : nullptr;
}

SyntheticChildrenFrontEnd *lldb_private::formatters::CjOptionSyntheticFrontEndCreator(
    CXXSyntheticChildren *, ValueObjectSP valobj_sp) {
  return valobj_sp ? new CjOptionSyntheticFrontEnd(valobj_sp) : nullptr;
}

SyntheticChildrenFrontEnd *lldb_private::formatters::CjVArraySyntheticFrontEndCreator(
    CXXSyntheticChildren *, ValueObjectSP valobj_sp) {
  return valobj_sp ? new CjVArraySyntheticFrontEnd(valobj_sp) : nullptr;
}
