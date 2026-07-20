//===-- ItaniumABILanguageRuntime.cpp -------------------------------------===//
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

#include "ItaniumABILanguageRuntime.h"

#include "lldb/Core/PluginManager.h"
#include "lldb/DataFormatters/FormattersHelpers.h"
#include "lldb/Expression/DiagnosticManager.h"
#include "lldb/Expression/FunctionCaller.h"
#include "lldb/Interpreter/CommandObject.h"
#include "lldb/Interpreter/CommandObjectMultiword.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Symbol/TypeList.h"
#include "lldb/Target/SectionLoadList.h"
#include "lldb/Target/StopInfo.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"

#include "cangjie/Basic/UGTypeKind.h"

#include <vector>

using namespace lldb;
using namespace lldb_private;

LLDB_PLUGIN_DEFINE_ADV(ItaniumABILanguageRuntime, CXXItaniumABI)

static const char *vtable_demangled_prefix = "vtable for ";
static const size_t name_size = 128;

char ItaniumABILanguageRuntime::ID = 0;

bool ItaniumABILanguageRuntime::CouldHaveDynamicValue(ValueObject &in_value) {
  const bool check_cxx = true;
  const bool check_objc = false;
  return in_value.GetCompilerType().IsPossibleDynamicType(nullptr, check_cxx,
                                                          check_objc);
}

TypeAndOrName ItaniumABILanguageRuntime::GetTypeInfoFromVTableAddress(
    ValueObject &in_value, lldb::addr_t original_ptr,
    lldb::addr_t vtable_load_addr) {
  if (m_process && vtable_load_addr != LLDB_INVALID_ADDRESS) {
    // Find the symbol that contains the "vtable_load_addr" address
    Address vtable_addr;
    Target &target = m_process->GetTarget();
    if (!target.GetSectionLoadList().IsEmpty()) {
      if (target.GetSectionLoadList().ResolveLoadAddress(vtable_load_addr,
                                                         vtable_addr)) {
        // See if we have cached info for this type already
        TypeAndOrName type_info = GetDynamicTypeInfo(vtable_addr);
        if (type_info)
          return type_info;

        SymbolContext sc;
        target.GetImages().ResolveSymbolContextForAddress(
            vtable_addr, eSymbolContextSymbol, sc);
        Symbol *symbol = sc.symbol;
        if (symbol != nullptr) {
          const char *name =
              symbol->GetMangled().GetDemangledName().AsCString();
          if (name && strstr(name, vtable_demangled_prefix) == name) {
            Log *log = GetLog(LLDBLog::Object);
            LLDB_LOGF(log,
                      "0x%16.16" PRIx64
                      ": static-type = '%s' has vtable symbol '%s'\n",
                      original_ptr, in_value.GetTypeName().GetCString(), name);
            // We are a C++ class, that's good.  Get the class name and look it
            // up:
            const char *class_name = name + strlen(vtable_demangled_prefix);
            // We know the class name is absolute, so tell FindTypes that by
            // prefixing it with the root namespace:
            std::string lookup_name("::");
            lookup_name.append(class_name);
            
            type_info.SetName(class_name);
            const bool exact_match = true;
            TypeList class_types;

            // First look in the module that the vtable symbol came from and
            // look for a single exact match.
            llvm::DenseSet<SymbolFile *> searched_symbol_files;
            if (sc.module_sp)
              sc.module_sp->FindTypes(ConstString(lookup_name), exact_match, 1,
                                      searched_symbol_files, class_types);

            // If we didn't find a symbol, then move on to the entire module
            // list in the target and get as many unique matches as possible
            if (class_types.Empty())
              target.GetImages().FindTypes(nullptr, ConstString(lookup_name),
                                           exact_match, UINT32_MAX,
                                           searched_symbol_files, class_types);

            lldb::TypeSP type_sp;
            if (class_types.Empty()) {
              LLDB_LOGF(log, "0x%16.16" PRIx64 ": is not dynamic\n",
                        original_ptr);
              return TypeAndOrName();
            }
            if (class_types.GetSize() == 1) {
              type_sp = class_types.GetTypeAtIndex(0);
              if (type_sp) {
                if (TypeSystemClang::IsCXXClassType(
                        type_sp->GetForwardCompilerType())) {
                  LLDB_LOGF(
                      log,
                      "0x%16.16" PRIx64
                      ": static-type = '%s' has dynamic type: uid={0x%" PRIx64
                      "}, type-name='%s'\n",
                      original_ptr, in_value.GetTypeName().AsCString(),
                      type_sp->GetID(), type_sp->GetName().GetCString());
                  type_info.SetTypeSP(type_sp);
                }
              }
            } else {
              size_t i;
              if (log) {
                for (i = 0; i < class_types.GetSize(); i++) {
                  type_sp = class_types.GetTypeAtIndex(i);
                  if (type_sp) {
                    LLDB_LOGF(
                        log,
                        "0x%16.16" PRIx64
                        ": static-type = '%s' has multiple matching dynamic "
                        "types: uid={0x%" PRIx64 "}, type-name='%s'\n",
                        original_ptr, in_value.GetTypeName().AsCString(),
                        type_sp->GetID(), type_sp->GetName().GetCString());
                  }
                }
              }

              for (i = 0; i < class_types.GetSize(); i++) {
                type_sp = class_types.GetTypeAtIndex(i);
                if (type_sp) {
                  if (TypeSystemClang::IsCXXClassType(
                          type_sp->GetForwardCompilerType())) {
                    LLDB_LOGF(
                        log,
                        "0x%16.16" PRIx64 ": static-type = '%s' has multiple "
                        "matching dynamic types, picking "
                        "this one: uid={0x%" PRIx64 "}, type-name='%s'\n",
                        original_ptr, in_value.GetTypeName().AsCString(),
                        type_sp->GetID(), type_sp->GetName().GetCString());
                    type_info.SetTypeSP(type_sp);
                  }
                }
              }

              if (log) {
                LLDB_LOGF(log,
                          "0x%16.16" PRIx64
                          ": static-type = '%s' has multiple matching dynamic "
                          "types, didn't find a C++ match\n",
                          original_ptr, in_value.GetTypeName().AsCString());
              }
            }
            if (type_info)
              SetDynamicTypeInfo(vtable_addr, type_info);
            return type_info;
          }
        }
      }
    }
  }
  return TypeAndOrName();
}

static bool ReadMemoryFromAddress(Process& process, addr_t addr, void *buf, size_t size) {
  Status err;
  process.ReadMemory(addr, buf, size, err);
  if (err.Success()) {
    return true;
  }

  return false;
}

bool TypeInfo::IsFunc() const
{
  return type == UGTypeKind::UG_FUNC;
}

bool TypeInfo::IsCFunc() const
{
  return type == UGTypeKind::UG_CFUNC;
}

bool TypeInfo::IsVArray() const
{
  return type == UGTypeKind::UG_VARRAY;
}

std::string TypeInfo::GetName(Process& process) const
{
  Status err;
  if (this->typeArgNum == 0) {
    if (this->name != nullptr) {
      char res[name_size + 1];
      res[name_size] = '\0';
      if (ReadMemoryFromAddress(process, (uint64_t)this->name, res, name_size)) {
        return std::string(res);
      }
    } else {
      return "";
    }
  }

  uint64_t args = (uint64_t)this->typeArgs;
  uint64_t argTiAddr;
  TypeInfo argTi;
  if (this->IsFunc()) {
    if (!ReadMemoryFromAddress(process, (uint64_t)args, &argTiAddr, sizeof(uint64_t)) ||
        !ReadMemoryFromAddress(process, (uint64_t)argTiAddr, &argTi, sizeof(TypeInfo))) {
      return "";
    }
    return argTi.GetName(process);
  }

  std::string typeName;

  uint32_t argSize = this->typeArgNum;
  uint32_t startIter = 0;
  std::string suffix;
  // For CFunc, it should be formatted as (typeArg1,typeArg2,...,typeArgN)->typeArg0
  if (this->IsCFunc()) {
    startIter = 1U;
    typeName.append("(");
    process.ReadMemory(args, &argTiAddr, sizeof(uint64_t), err);
    process.ReadMemory(argTiAddr, &argTi, sizeof(TypeInfo), err);
    std::string returnTypeName = argTi.GetName(process);
    suffix.append(")->").append(returnTypeName);
  } else {
    TypeTemplate sourceGeneric;
    process.ReadMemory((uint64_t)this->sourceGeneric, &sourceGeneric, sizeof(TypeTemplate), err);
    char sourceGenericName[name_size + 1];
    sourceGenericName[name_size] = '\0';
    process.ReadMemory((uint64_t)sourceGeneric.name, sourceGenericName, name_size, err);
    typeName.append(sourceGenericName).append("<");
    suffix.append(">");
  }

  if (this->IsVArray()) {
    process.ReadMemory(args, &argTiAddr, sizeof(uint64_t), err);
    process.ReadMemory(argTiAddr, &argTi, sizeof(TypeInfo), err);
    std::string tiName = argTi.GetName(process);
    typeName.append(tiName).append(",").append(std::to_string(argSize));
  }
  for (uint32_t idx = startIter; idx < argSize; ++idx) {
    if (ReadMemoryFromAddress(process, args + idx * 8U, &argTiAddr, sizeof(uint64_t)) &&
        ReadMemoryFromAddress(process, argTiAddr, &argTi, sizeof(TypeInfo))) {
      std::string tiName = argTi.GetName(process);
      typeName.append(tiName);
      if (idx < (argSize - 1U)) {
        typeName.append(",");
      }
    } else {
      break;
    }
  }
  typeName.append(suffix);
  return typeName;
}

ConstString GetTypeName(Process& process, TypeInfo& typeInfo) {
    std::string temp_name = typeInfo.GetName(process);
    UGTypeKind typekind = static_cast<UGTypeKind>(typeInfo.type);
    auto pos = temp_name.find(":");
    if (typekind == UGTypeKind::UG_ENUM || typekind == UGTypeKind::UG_COMMON_ENUM) {
      // type_name: "default:RGBColor"  ==>  "default::E1$RGBColor"
      if (pos != std::string::npos && temp_name.find("std.core:Option") != 0) {
        std::string enum_prefix = "::E1$";
        temp_name = temp_name.substr(0, pos) + enum_prefix + temp_name.substr(pos + 1);
        pos = temp_name.find(":", pos + enum_prefix.size());
      }
    }
    // type_name: "default:A"  ==> "default::A"
    while (pos != std::string::npos) {
      // type_name: "default:$Captured_Int64"  ==> "default::$Captured_Int64"
      if (pos != temp_name.find("::")) {
        // type_name: "org::default:C1", if type_name has org, no need to replace ":" with "::"
        temp_name.replace(pos, 1, "::");
      }
      pos = temp_name.find(":", pos + 2); // size of "::" is 2
    }
    // "org/pkg::CC"  ==>  "org::pkg::CC"
    pos = temp_name.find("/");
    while (pos != std::string::npos) {
      temp_name.replace(pos, 1, "::");
      pos = temp_name.find("/", pos + 2); // size of "::" is 2
    }
    return ConstString(temp_name.c_str());
}

bool IsRefType(int8_t type) {
    if (type < 0) {
        return true;
    }
    return false;
}

std::string ItaniumABILanguageRuntime::GetReflectionFieldName(int16_t id, TypeInfo& typeInfo) {
  uint64_t field_name_addr;
  Status error;
  char field_name[name_size + 1];
  field_name[name_size] = '\0';
  // The `relection` field of `TypeInfo` has two structures: one for enums and another for non-enums.
  // for non-enums named Reflection.
  // EnumReflection
  if (typeInfo.flag == UGTypeKind::UG_ENUM || typeInfo.flag == UGTypeKind::UG_COMMON_ENUM) {
    return "";
  }
  // Reflection
  uint64_t name_addr_offset;
  uint64_t names_addr;
  m_process->ReadMemory((uint64_t)typeInfo.reflection, &name_addr_offset, sizeof(uint64_t), error);
  field_name_addr = (uint64_t)typeInfo.reflection + name_addr_offset;
  m_process->ReadMemory(field_name_addr + id * BitsPerByte, &names_addr, sizeof(uint64_t), error);
  field_name_addr = field_name_addr + names_addr + id * BitsPerByte;

  m_process->ReadMemory(field_name_addr, field_name, name_size, error);
  std::string fname(field_name);
  if (fname.empty()) {
    fname = "x" + std::to_string(id);
  }
  return fname;
}

void ItaniumABILanguageRuntime::AddFieldToRecordType(
    TypeSystemClang& ast, TypeInfo& typeInfo, CompilerType& dynamic_type, bool is_class_member) {
    Status error;
    int16_t super_fields_num = 0;
    if (typeInfo.super) {
      TypeInfo superti;
      m_process->ReadMemory((uint64_t)(typeInfo.super), &superti, sizeof(TypeInfo), error);
      if (error.Success()) {
        auto super_type = GetDynamicTypeFromGenericTypeInfo(ast, superti);
        if (!super_type.IsValid()) {
          return;
        }
        if (is_class_member && super_type.GetTypeName() == "std.core::Object") {
          // add typeinfo* to class member.
          CompilerType ti_type = ast.GetBasicType(eBasicTypeVoid).GetPointerType();
          ast.AddFieldToRecordType(dynamic_type, "$ti*", ti_type, lldb::eAccessPublic, 0);
        } else {
          // DW_TAG_inheritance
          std::vector<std::unique_ptr<clang::CXXBaseSpecifier>> bases;
          bases.push_back(
            ast.CreateBaseClassSpecifier(super_type.GetOpaqueQualType(), lldb::eAccessPublic, false, true));
          ast.TransferBaseClasses(dynamic_type.GetOpaqueQualType(), std::move(bases));
          super_fields_num = superti.fieldsNum;
        }
      }
    }
    uint64_t field_type_addr = 0;
    uint64_t field_name_addr = 0;
    char field_name[name_size + 1];
    field_name[name_size] = '\0';
    for (int16_t i = 0; i < typeInfo.fieldsNum - super_fields_num; i++) {
      m_process->ReadMemory((uint64_t)(typeInfo.fields) + i * BitsPerByte, &field_type_addr, sizeof(uint64_t), error);
      TypeInfo fieldti;
      m_process->ReadMemory(field_type_addr, &fieldti, sizeof(TypeInfo), error);
      CompilerType field_type;
      if (IsRefType(fieldti.type)) {
        field_type = GetDynamicTypeFromGenericTypeInfo(ast, fieldti).GetPointerType();
      } else {
        field_type = GetDynamicTypeFromGenericTypeInfo(ast, fieldti);
      }
      std::string fname = GetReflectionFieldName(i, typeInfo);
      auto field = ast.AddFieldToRecordType(dynamic_type, fname.c_str(), field_type, lldb::eAccessPublic, 0);
      if (field_type.GetTypeName() == "Unit") {
        clang::ASTContext &clang_ast =  ast.getASTContext();
        llvm::APInt bitfield_bit_size_apint(clang_ast.getTypeSize(clang_ast.IntTy),
                                            0);
        auto unit_bit_width = new (clang_ast)
            clang::IntegerLiteral(clang_ast, bitfield_bit_size_apint,
                                  clang_ast.IntTy, clang::SourceLocation());
        field->setBitWidth(unit_bit_width);
      }
    }
}

CompilerType ItaniumABILanguageRuntime::GetDynamicClassType(
    TypeSystemClang& ast, TypeInfo& typeInfo, ConstString& type_name) {
    if (type_name.GetStringRef().contains("$C")) {
      return GetDynamicFuncType(ast, typeInfo, type_name);
    }
    CompilerType dynamic_type = ast.GetTypeForIdentifier<clang::CXXRecordDecl>(type_name);
    if (dynamic_type.IsValid()) {
      return dynamic_type;
    }
    dynamic_type = ast.CreateRecordType(nullptr, OptionalClangModuleID(), lldb::eAccessPublic,
                    type_name.GetCString(), clang::TTK_Class, lldb::eLanguageTypeC);
    ast.StartTagDeclarationDefinition(dynamic_type);
    AddFieldToRecordType(ast, typeInfo, dynamic_type, true);
    ast.CompleteTagDeclarationDefinition(dynamic_type);
    return dynamic_type;
}

CompilerType ItaniumABILanguageRuntime::GetDynamicInterfaceType(
    TypeSystemClang& ast, ConstString& type_name) {
    std::string interface_name = std::string("Interface$") + std::string(type_name.AsCString());
    auto dynamic_type = ast.CreateRecordType(nullptr, OptionalClangModuleID(), lldb::eAccessPublic,
                                             interface_name.c_str(), clang::TTK_Class, lldb::eLanguageTypeC);
    return dynamic_type.CreateTypedef(type_name.GetCString(), ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
}

CompilerType ItaniumABILanguageRuntime::GetDynamicRawArrayType(
    TypeSystemClang& ast, TypeInfo& typeInfo, ConstString& type_name) {
    CompilerType dynamic_type = ast.GetTypeForIdentifier<clang::CXXRecordDecl>(type_name);
    if (dynamic_type.IsValid()) {
      return dynamic_type;
    }
    dynamic_type = ast.CreateRecordType(nullptr, OptionalClangModuleID(), lldb::eAccessPublic,
                    type_name.GetCString(), clang::TTK_Struct, lldb::eLanguageTypeC);
    ast.StartTagDeclarationDefinition(dynamic_type);
    // add _typeinfo to raw-array member.
    auto ti_type = ast.GetBasicType(eBasicTypeVoid).GetPointerType();
    ast.AddFieldToRecordType(dynamic_type, "$ti*", ti_type, lldb::eAccessPublic, 0);

    // add size to raw-array member.
    CompilerType basic_type = ast.GetBasicType(eBasicTypeLongLong).CreateTypedef(
            "Int64", ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
    ast.AddFieldToRecordType(dynamic_type, "size", basic_type, lldb::eAccessPublic, 0);

    // add elements to raw-array member.
    CompilerType field_type;
    Status error;
    TypeInfo fieldti;
    m_process->ReadMemory((uint64_t)(typeInfo.super), &fieldti, sizeof(TypeInfo), error);
    if (error.Success()) {
      if (IsRefType(fieldti.type)) {
        field_type = GetDynamicTypeFromGenericTypeInfo(ast, fieldti).GetPointerType();
      } else {
        field_type = GetDynamicTypeFromGenericTypeInfo(ast, fieldti);
      }
    } else {
      // If raw-array does not have a field, use UInt8 as the default value.
      field_type = ast.GetBasicType(eBasicTypeUnsignedChar).CreateTypedef(
            "UInt8", ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
    }
    auto field = ast.AddFieldToRecordType(dynamic_type, "elements", field_type, lldb::eAccessPublic, 0);
    if (field_type.GetTypeName() == "Unit") {
      clang::ASTContext &clang_ast =  ast.getASTContext();
      llvm::APInt bitfield_bit_size_apint(clang_ast.getTypeSize(clang_ast.IntTy),
                                          0);
      auto unit_bit_width = new (clang_ast)
          clang::IntegerLiteral(clang_ast, bitfield_bit_size_apint,
                                clang_ast.IntTy, clang::SourceLocation());
      field->setBitWidth(unit_bit_width);
    }
    ast.CompleteTagDeclarationDefinition(dynamic_type);
    return dynamic_type;
}

ConstString GetFunctionTypeName(std::string type_name,
    uint64_t para_num, std::vector<CompilerType>& param_types, CompilerType& return_type) {
    std::string name = "";
    if (auto pos = type_name.find(":"); pos != std::string::npos) {
      name = type_name.substr(0, pos) + "::(";
    } else {
      name = "(";
    }
    std::string expression_package_name = "__cjdb_expr::";
    uint64_t para_id = 0;
    if (name.find(expression_package_name) == 0) {
      // If the function type is created by cangjie expression. Delete package name.
      name = name.substr(expression_package_name.size());
    }
    for (auto& para : param_types) {
      para_id++;
      name += std::string(para.GetTypeName().GetCString());
      if (para_id != para_num) {
        name += ",";
      }
    }
    name += ")->" + std::string(return_type.GetTypeName().GetCString());
    std::string enum_prefix = "E1$";
    auto pos = name.find(enum_prefix);
    while (pos != std::string::npos) {
      name.replace(pos, enum_prefix.size(), "");
      pos = name.find(enum_prefix, pos + 1);
    }
    return ConstString(name.c_str());
}

CompilerType ItaniumABILanguageRuntime::GetDynamicFuncType(
    TypeSystemClang& ast, TypeInfo& typeInfo, ConstString& type_name) {
    // type_name: (Int32) -> Int32
    Status error;
    TypeInfo closure_ti;
    TypeInfo func_ti;
    bool need_refresh_name = false;

    if (typeInfo.type == UGTypeKind::UG_CLASS) {
      m_process->ReadMemory((uint64_t)(typeInfo.super), &closure_ti, sizeof(TypeInfo), error);
      uint64_t func_ti_addr = 0;
      m_process->ReadMemory((uint64_t)(closure_ti.typeArgs), &func_ti_addr, sizeof(uint64_t), error);
      m_process->ReadMemory(func_ti_addr, &func_ti, sizeof(TypeInfo), error);
      need_refresh_name = true;
    } else if (typeInfo.type == UGTypeKind::UG_FUNC) {
      uint64_t func_ti_addr = 0;
      m_process->ReadMemory((uint64_t)(typeInfo.typeArgs), &func_ti_addr, sizeof(uint64_t), error);
      m_process->ReadMemory(func_ti_addr, &func_ti, sizeof(TypeInfo), error);
      need_refresh_name = true;
    } else if (typeInfo.type == UGTypeKind::UG_CFUNC) {
      func_ti = typeInfo;
    } else {
      return CompilerType();
    }

    uint8_t type_arg_num = func_ti.typeArgNum;
    uint64_t type_args = (uint64_t)func_ti.typeArgs;

    if (!error.Success()) {
      return CompilerType();
    }
    if (type_arg_num < 1) {
      return CompilerType();
    }
    // para_typeinfo: type_args[1..type_arg_num-1]
    uint64_t para_ti_addr = 0;
    TypeInfo fieldti;
    std::vector<CompilerType> param_types;
    for (int8_t i = 1; i < type_arg_num; i++) {
      m_process->ReadMemory(type_args + i * BitsPerByte, &para_ti_addr, sizeof(uint64_t), error);
      m_process->ReadMemory(para_ti_addr, &fieldti, sizeof(TypeInfo), error);
      param_types.push_back(GetDynamicTypeFromGenericTypeInfo(ast, fieldti));
    }

    // return_typeinfo: type_args[0]
    uint64_t return_ti_addr = 0;
    m_process->ReadMemory(type_args, &return_ti_addr, sizeof(uint64_t), error);
    m_process->ReadMemory(return_ti_addr, &fieldti, sizeof(TypeInfo), error);
    if (!error.Success()) {
      return CompilerType();
    }
    auto return_type = GetDynamicTypeFromGenericTypeInfo(ast, fieldti);
    if (need_refresh_name) {
      // type_name: default$$Auto_Env__ZN7default3gooEi
      type_name = GetFunctionTypeName(type_name.GetCString(), type_arg_num - 1, param_types, return_type);
    }

    CompilerType dynamic_type = ast.CreateRecordType(nullptr, OptionalClangModuleID(), lldb::eAccessPublic,
                    type_name.GetCString(), clang::TTK_Struct, lldb::eLanguageTypeC);
    ast.StartTagDeclarationDefinition(dynamic_type);
    // add typeinfo*.
    auto ti_type = ast.GetBasicType(eBasicTypeVoid).GetPointerType();
    ast.AddFieldToRecordType(dynamic_type, "$ti*", ti_type, lldb::eAccessPublic, 0);
    // type -> subroutine -> pointer
    unsigned type_quals = 0;
    auto subroutine_type = ast.CreateFunctionType(return_type,
                    param_types.data(), param_types.size(), false, type_quals, clang::CC_C);
    ast.AddFieldToRecordType(dynamic_type, "ptr", subroutine_type.GetPointerType(), lldb::eAccessPublic, 0);
    ast.CompleteTagDeclarationDefinition(dynamic_type);
    return dynamic_type;
}

CompilerType ItaniumABILanguageRuntime::GetDynamicCStringType(
    TypeSystemClang& ast, TypeInfo& typeInfo, ConstString& type_name) {
    CompilerType dynamic_type = ast.CreateRecordType(nullptr, OptionalClangModuleID(), lldb::eAccessPublic,
                    type_name.GetCString(), clang::TTK_Struct, lldb::eLanguageTypeC);
    ast.StartTagDeclarationDefinition(dynamic_type);
    ast.AddFieldToRecordType(dynamic_type, "chars", ast.GetCStringType(false), lldb::eAccessPublic, 0);
    ast.CompleteTagDeclarationDefinition(dynamic_type);
    return dynamic_type;
}

CompilerType ItaniumABILanguageRuntime::GetDynamicCPointerType(
    TypeSystemClang& ast, TypeInfo& typeInfo, ConstString& type_name) {
    // CPointer<UInt8>
    Status error;
    TypeInfo fieldti;
    m_process->ReadMemory((uint64_t)(typeInfo.super), &fieldti, sizeof(TypeInfo), error);
    if (!error.Success()) {
      return CompilerType();
    }
    CompilerType dynamic_type = ast.CreateRecordType(nullptr, OptionalClangModuleID(), lldb::eAccessPublic,
                    type_name.GetCString(), clang::TTK_Struct, lldb::eLanguageTypeC);
    ast.StartTagDeclarationDefinition(dynamic_type);
    auto child_type = GetDynamicTypeFromGenericTypeInfo(ast, fieldti);
    ast.AddFieldToRecordType(dynamic_type, "ptr", child_type.GetPointerType(), lldb::eAccessPublic, 0);
    ast.CompleteTagDeclarationDefinition(dynamic_type);
    return dynamic_type;
}

CompilerType ItaniumABILanguageRuntime::GetDynamicCFuncType(
    TypeSystemClang& ast, TypeInfo& typeInfo, ConstString& type_name) {
    // type_name: (Int32) -> Int32
    Status error;
    int8_t type_arg_num = typeInfo.typeArgNum;
    uint64_t type_args = (uint64_t)typeInfo.typeArgs;
    if (type_arg_num < 1) {
      return CompilerType();
    }
    // para_num: type_arg_num - 1
    int8_t para_num = type_arg_num - 1;

    // para_typeinfo: type_args
    uint64_t para_ti_addr = 0;
    TypeInfo fieldti;
    std::vector<CompilerType> param_types;
    for (int8_t i = 0; i < para_num; i++) {
      m_process->ReadMemory(type_args + i * BitsPerByte, &para_ti_addr, sizeof(uint64_t), error);
      m_process->ReadMemory(para_ti_addr, &fieldti, sizeof(TypeInfo), error);
      param_types.push_back(GetDynamicTypeFromGenericTypeInfo(ast, fieldti));
    }

    // return_typeinfo: super.typeArgs + para_num * BitsPerByte
    uint64_t return_ti_addr = 0;
    m_process->ReadMemory(type_args + para_num * BitsPerByte, &return_ti_addr, sizeof(uint64_t), error);
    m_process->ReadMemory(return_ti_addr, &fieldti, sizeof(TypeInfo), error);
    if (!error.Success()) {
      return CompilerType();
    }
    auto return_type = GetDynamicTypeFromGenericTypeInfo(ast, fieldti);
    CompilerType dynamic_type = ast.CreateRecordType(nullptr, OptionalClangModuleID(), lldb::eAccessPublic,
                    type_name.GetCString(), clang::TTK_Struct, lldb::eLanguageTypeC);
    ast.StartTagDeclarationDefinition(dynamic_type);
    unsigned type_quals = 0;
    auto subroutine_type = ast.CreateFunctionType(return_type,
                    param_types.data(), param_types.size(), false, type_quals, clang::CC_C);
    ast.AddFieldToRecordType(dynamic_type, "ptr", subroutine_type.GetPointerType(), lldb::eAccessPublic, 0);
    ast.CompleteTagDeclarationDefinition(dynamic_type);
    return dynamic_type;
}

CompilerType ItaniumABILanguageRuntime::GetDynamicVArrayType(
    TypeSystemClang& ast, TypeInfo& typeInfo, ConstString& type_name) {
    // VArray<Int64, $3>
    Status error;
    TypeInfo fieldti;
    m_process->ReadMemory((uint64_t)(typeInfo.super), &fieldti, sizeof(TypeInfo), error);
    if (!error.Success()) {
      return CompilerType();
    }
    auto field_type = GetDynamicTypeFromGenericTypeInfo(ast, fieldti);
    if (!type_name.GetStringRef().contains("$")) {
      // VArray<Int64,3>
      std::string field_type_name = field_type.GetTypeName().GetCString();
      std::string full_type_name = "VArray<" + field_type_name + ",$" + std::to_string(typeInfo.fieldsNum) + ">";
      type_name = ConstString(full_type_name.c_str());
    }
    return ast.CreateArrayType(field_type, typeInfo.fieldsNum, false).CreateTypedef(
                    type_name.GetCString(), ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
}

CompilerType ItaniumABILanguageRuntime::GetDynamicTupleType(
    TypeSystemClang& ast, TypeInfo& typeInfo, ConstString& type_name) {
    Status error;
    CompilerType dynamic_type = ast.GetTypeForIdentifier<clang::CXXRecordDecl>(type_name);
    if (dynamic_type.IsValid()) {
      return dynamic_type;
    }
    dynamic_type = ast.CreateRecordType(nullptr, OptionalClangModuleID(), lldb::eAccessPublic,
                    type_name.GetCString(), clang::TTK_Struct, lldb::eLanguageTypeC);
    ast.StartTagDeclarationDefinition(dynamic_type);
    uint64_t field_type_addr = 0;
    TypeInfo fieldti;
    for (int16_t i = 0; i < typeInfo.fieldsNum; i++) {
      m_process->ReadMemory((uint64_t)(typeInfo.fields) + i * BitsPerByte, &field_type_addr, sizeof(uint64_t), error);
      m_process->ReadMemory(field_type_addr, &fieldti, sizeof(TypeInfo), error);
      auto field_type = GetDynamicTypeFromGenericTypeInfo(ast, fieldti);
      std::string field_name = "_" + std::to_string(i);
      ast.AddFieldToRecordType(dynamic_type, field_name.c_str(), field_type, lldb::eAccessPublic, 0);
    }
    ast.CompleteTagDeclarationDefinition(dynamic_type);
    return dynamic_type;
}

CompilerType ItaniumABILanguageRuntime::GetDynamicStructType(
    TypeSystemClang& ast, TypeInfo& typeInfo, ConstString& type_name) {
    CompilerType dynamic_type = ast.GetTypeForIdentifier<clang::CXXRecordDecl>(type_name);
    if (dynamic_type.IsValid()) {
      return dynamic_type;
    }
    dynamic_type = ast.CreateRecordType(nullptr, OptionalClangModuleID(), lldb::eAccessPublic,
                    type_name.GetCString(), clang::TTK_Struct, lldb::eLanguageTypeC);
    ast.StartTagDeclarationDefinition(dynamic_type);
    AddFieldToRecordType(ast, typeInfo, dynamic_type);
    ast.CompleteTagDeclarationDefinition(dynamic_type);
    return dynamic_type;
}

static CompilerType CreateEnumType(TypeSystemClang& ast, Process& process,
    std::string& enum_name, uint64_t ctor_num, uint64_t enumCtorInfo_addr) {
    // enum_name: "RGBColor"
    CompilerType basic_type = ast.GetBasicType(eBasicTypeInt).CreateTypedef(
            "Int32", ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
    CompilerType enumType = ast.CreateEnumerationType(enum_name.c_str(),
            ast.GetTranslationUnitDecl(), OptionalClangModuleID(), Declaration(), basic_type, false);
    ast.StartTagDeclarationDefinition(enumType);
    Status error;
    char ctor_name[name_size + 1];
    ctor_name[name_size] = '\0';
    // The `relection` field of `TypeInfo` has two structures: one for enums and another for non-enums.
    // for enums named EnumReflection
    for (uint64_t i = 0; i < ctor_num; i++) {
      uint64_t name_addr = 0;
      uint64_t name_addr_offset = 0;
      process.ReadMemory(enumCtorInfo_addr + i * BitsPerWidth, &name_addr_offset, sizeof(uint64_t), error);
      name_addr = enumCtorInfo_addr + i * BitsPerWidth + name_addr_offset;
      process.ReadMemory(name_addr, ctor_name, name_size, error);
      ast.AddEnumerationValueToEnumerationType(enumType, Declaration(), ctor_name, i, 4);
    }
    ast.CompleteTagDeclarationDefinition(enumType);
    return enumType;
}

void ItaniumABILanguageRuntime::CreateAndAddInheritTypeToRecordType(CompilerType& dynamic_type, CompilerType& enum_type,
    TypeSystemClang& ast, uint64_t ctor_num, uint64_t ctors_addr) {
    auto ti_type = ast.GetBasicType(eBasicTypeVoid).GetPointerType();
    TypeInfo ctor_ti;
    Status error;
    std::string enum_name(enum_type.GetTypeName().AsCString());
    std::vector<std::unique_ptr<clang::CXXBaseSpecifier>> bases;  // DW_TAG_inheritance
    for (uint64_t i = 0; i < ctor_num; i++) {
      auto ctor_name = enum_name + "_ctor_" + std::to_string(i);
      auto ctor_type = ast.CreateRecordType(nullptr, OptionalClangModuleID(), lldb::eAccessPublic,
              ctor_name.c_str(), clang::TTK_Struct, lldb::eLanguageTypeC);
      ast.StartTagDeclarationDefinition(ctor_type);
      ast.AddFieldToRecordType(ctor_type, "$ti*", ti_type, lldb::eAccessPublic, 0);
      ast.AddFieldToRecordType(ctor_type, "constructor", enum_type, lldb::eAccessPublic, 0);
      uint64_t ctor_addr = 0;
      m_process->ReadMemory(ctors_addr + i * BitsPerByte, &ctor_addr, sizeof(uint64_t), error);
      m_process->ReadMemory(ctor_addr, &ctor_ti, sizeof(TypeInfo), error);
      if (!error.Success()) {
        continue;
      }
      if (ctor_ti.fieldsNum > 1) {
        uint64_t ctor_para_addr = 0;
        TypeInfo ctor_para_ti;
        for (int16_t j = 0; j < ctor_ti.fieldsNum - 1; j++) {
          m_process->ReadMemory((uint64_t)(ctor_ti.fields) + (j + 1)  * BitsPerByte, &ctor_para_addr,
                                sizeof(uint64_t), error);
          m_process->ReadMemory(ctor_para_addr, &ctor_para_ti, sizeof(TypeInfo), error);
          if (!error.Success()) {
            break;
          }
          auto ctor_para_type = GetDynamicTypeFromGenericTypeInfo(ast, ctor_para_ti);
          std::string ctor_para_name = "arg_" + std::to_string(j + 1);
          ast.AddFieldToRecordType(ctor_type, ctor_para_name.c_str(),
            IsRefType(ctor_para_ti.type) ? ctor_para_type.GetPointerType() : ctor_para_type, lldb::eAccessPublic, 0);
        }
      }
      ast.CompleteTagDeclarationDefinition(ctor_type);
      auto inherit_type = ast.CreateBaseClassSpecifier(ctor_type.GetOpaqueQualType(),
                                    lldb::eAccessPublic, false, true);
      auto type_source_info = inherit_type->getTypeSourceInfo();
      if (type_source_info) {
        auto type = ast.GetType(type_source_info->getType());
        ast.StartTagDeclarationDefinition(type);
        ast.CompleteTagDeclarationDefinition(type);
      }
      bases.push_back(std::move(inherit_type));
    }
    ast.TransferBaseClasses(dynamic_type.GetOpaqueQualType(), std::move(bases));
}

void ItaniumABILanguageRuntime::CreateAndAddInheritTypeToEnum3Type(CompilerType& dynamic_type, CompilerType& enum_type,
    TypeSystemClang& ast, uint64_t ctor_num, uint64_t ctors_addr) {
    TypeInfo ctor_ti;
    Status error;
    std::string enum_name(enum_type.GetTypeName().AsCString());
    std::vector<std::unique_ptr<clang::CXXBaseSpecifier>> bases;  // DW_TAG_inheritance
    for (uint64_t i = 0; i < ctor_num; i++) {
      auto ctor_name = enum_name + "_ctor_" + std::to_string(i);
      auto ctor_type = ast.CreateRecordType(nullptr, OptionalClangModuleID(), lldb::eAccessPublic,
              ctor_name.c_str(), clang::TTK_Struct, lldb::eLanguageTypeC);
      ast.StartTagDeclarationDefinition(ctor_type);
      uint64_t ctor_addr = 0;
     // the offset of the enumCtors array member is 16, type info offset is 8.
      m_process->ReadMemory(ctors_addr + i * BitsPerWidth + BitsPerByte, &ctor_addr, sizeof(uint64_t), error);
      m_process->ReadMemory(ctor_addr, &ctor_ti, sizeof(TypeInfo), error);
      if (!error.Success()) {
        continue;
      }
      ast.AddFieldToRecordType(ctor_type, "constructor", enum_type, lldb::eAccessPublic, 0);
      if (ctor_ti.fieldsNum > 1) {
        uint64_t ctor_para_addr = 0;
        TypeInfo ctor_para_ti;
        for (int16_t j = 1; j < ctor_ti.fieldsNum; j++) {
          m_process->ReadMemory((uint64_t)(ctor_ti.fields) + j * BitsPerByte, &ctor_para_addr,
                                sizeof(uint64_t), error);
          m_process->ReadMemory(ctor_para_addr, &ctor_para_ti, sizeof(TypeInfo), error);
          if (!error.Success()) {
            break;
          }
          auto ctor_para_type = GetDynamicTypeFromGenericTypeInfo(ast, ctor_para_ti);
          std::string ctor_para_name = "arg_" + std::to_string(j);
          ast.AddFieldToRecordType(ctor_type, ctor_para_name.c_str(), ctor_para_type, lldb::eAccessPublic, 0);
        }
      }
      auto inherit_type = ast.CreateBaseClassSpecifier(ctor_type.GetOpaqueQualType(),
                                    lldb::eAccessPublic, false, true);
      auto type_source_info = inherit_type->getTypeSourceInfo();
      if (type_source_info) {
        auto type = ast.GetType(type_source_info->getType());
        ast.StartTagDeclarationDefinition(type);
        ast.CompleteTagDeclarationDefinition(type);
      }
      bases.push_back(std::move(inherit_type));
    }
    ast.TransferBaseClasses(dynamic_type.GetOpaqueQualType(), std::move(bases));
}

CompilerType ItaniumABILanguageRuntime::CreateEnum2Type(TypeSystemClang& ast,
  CompilerType& enum_type, ConstString type_name, uint64_t ctor_num, uint64_t enumCtorInfo_addr) {
  bool is_reference = false;
  auto dynamic_type = ast.CreateRecordType(nullptr, OptionalClangModuleID(), lldb::eAccessPublic,
                      type_name.GetCString(), clang::TTK_Struct, lldb::eLanguageTypeC);
  ast.StartTagDeclarationDefinition(dynamic_type);
  TypeInfo ctor_ti;
  Status error;
  std::string enum_name(enum_type.GetTypeName().AsCString());
  for (uint64_t i = 0; i < ctor_num; i++) {
    uint64_t ctor_addr = 0;
    // the offset of the enumCtors array member is 16, type info offset is 8.
    m_process->ReadMemory(enumCtorInfo_addr + i * BitsPerWidth + BitsPerByte, &ctor_addr, sizeof(uint64_t), error);
    m_process->ReadMemory(ctor_addr, &ctor_ti, sizeof(TypeInfo), error);
    if (!error.Success()) {
      continue;
    }
    if (ctor_ti.fieldsNum < 1) {
      continue;
    }
    uint64_t ctor_para_addr = 0;
    TypeInfo ctor_para_ti;
    for (int16_t j = 1; j < ctor_ti.fieldsNum; j++) {
      m_process->ReadMemory((uint64_t)(ctor_ti.fields) + (j)  * BitsPerByte, &ctor_para_addr, sizeof(uint64_t), error);
      m_process->ReadMemory(ctor_para_addr, &ctor_para_ti, sizeof(TypeInfo), error);
      if (!error.Success()) {
        break;
      }
      auto ctor_para_type = GetDynamicTypeFromGenericTypeInfo(ast, ctor_para_ti);
      ast.AddFieldToRecordType(dynamic_type, "constructor", enum_type, lldb::eAccessPublic, 0);
      if (IsRefType(ctor_para_ti.type)) {
        is_reference = true;
        ast.AddFieldToRecordType(dynamic_type, "val", ctor_para_type.GetPointerType(), lldb::eAccessPublic, 0);
      } else {
        ast.AddFieldToRecordType(dynamic_type, "val", ctor_para_type, lldb::eAccessPublic, 0);
      }
    }
  }
  ast.CompleteTagDeclarationDefinition(dynamic_type);
  return is_reference ? dynamic_type.GetPointerType() : dynamic_type;
}

CompilerType ItaniumABILanguageRuntime::GetDynamicOptionType(
    TypeSystemClang& ast, TypeInfo& typeInfo, ConstString& type_name) {
    Status error;
    uint64_t val_ti_addr = 0;
    TypeInfo valti;
    m_process->ReadMemory((uint64_t)typeInfo.typeArgs, &val_ti_addr, sizeof(uint64_t), error);
    m_process->ReadMemory(val_ti_addr, &valti, sizeof(TypeInfo), error);
    if (!error.Success()) {
      CompilerType();
    }
    auto option_type = ast.CreateRecordType(nullptr, OptionalClangModuleID(), lldb::eAccessPublic,
        type_name.GetCString(), clang::TTK_Struct, lldb::eLanguageTypeC);
    ast.StartTagDeclarationDefinition(option_type);
    auto val_type = GetDynamicTypeFromGenericTypeInfo(ast, valti);
    if (IsRefType(valti.type)) {
      ast.AddFieldToRecordType(option_type, "val", val_type.GetPointerType(), lldb::eAccessPublic, 0);
    } else {
      CompilerType basic_type = ast.GetBasicType(lldb::eBasicTypeInt);
      CompilerType enumType = ast.CreateEnumerationType(std::string("created"),
              ast.GetTranslationUnitDecl(), OptionalClangModuleID(), Declaration(), basic_type, false);
      ast.StartTagDeclarationDefinition(enumType);
      ast.AddEnumerationValueToEnumerationType(enumType, Declaration(), "Some", 0, 4);
      ast.AddEnumerationValueToEnumerationType(enumType, Declaration(), "None", 1, 4);
      ast.CompleteTagDeclarationDefinition(enumType);
      auto field = ast.AddFieldToRecordType(option_type, "constructor", enumType, lldb::eAccessPublic, 0);
      uint64_t size = val_type.GetByteSize(nullptr).value_or(0);
      if (size < 8) {
        clang::ASTContext &clang_ast =  ast.getASTContext();
        llvm::APInt bitfield_bit_size_apint(clang_ast.getTypeSize(clang_ast.IntTy), 1);
        auto bit_width = new (clang_ast)
            clang::IntegerLiteral(clang_ast, bitfield_bit_size_apint,
                                  clang_ast.IntTy, clang::SourceLocation());
        field->setBitWidth(bit_width);
      }
      ast.AddFieldToRecordType(option_type, "val", val_type, lldb::eAccessPublic, 0);
    }
    ast.CompleteTagDeclarationDefinition(option_type);
    return option_type;
}

ReflectModifyType ItaniumABILanguageRuntime::GetEnumKind(TypeInfo& typeInfo) {
  uint32_t modifier = 0;
  Status error;
  uint64_t modifier_addr = (uint64_t)typeInfo.reflection + BitsPerByte;
  m_process->ReadMemory(modifier_addr, &modifier, sizeof(uint32_t), error);
  bool isEnumKind0 = static_cast<bool>(modifier & RMT_ENUM_KIND0);
  bool isEnumKind1 = static_cast<bool>(modifier & RMT_ENUM_KIND1);
  bool isEnumKind2 = static_cast<bool>(modifier & RMT_ENUM_KIND2);
  bool isEnumKind3 = static_cast<bool>(modifier & RMT_ENUM_KIND3);
  if (isEnumKind0) {
    return RMT_ENUM_KIND0;
  } else if (isEnumKind1) {
    return RMT_ENUM_KIND1;
  } else if (isEnumKind2) {
    return RMT_ENUM_KIND2;
  } else if (isEnumKind3) {
    return RMT_ENUM_KIND3;
  } else {
    return RMT_MAX;
  }
}

CompilerType ItaniumABILanguageRuntime::GetDynamicEnumType(
    TypeSystemClang& ast, TypeInfo& typeInfo, ConstString& type_name) {
    Status error;
    auto typekind = static_cast<UGTypeKind>(typeInfo.type);
    if (typekind == UGTypeKind::UG_COMMON_ENUM) {
      m_process->ReadMemory(reinterpret_cast<uint64_t>(typeInfo.super), &typeInfo, sizeof(typeInfo), error);
      type_name = GetTypeName(*m_process, typeInfo);
    }
    const std::string ENUM_PREFIX_NAME = "E1$";
    const std::string E0_PREFIX_NAME = "E0$";
    // type_name: "default::E1$RGBColor"
    std::string enum_name(type_name.GetCString());
    auto pos = enum_name.find("$");
    if (pos != std::string::npos) {
      auto ltPos = enum_name.find(ENUM_PREFIX_NAME);
      if (ltPos != std::string::npos) {
        enum_name = enum_name.substr(0, ltPos) + enum_name.substr(ltPos+ ENUM_PREFIX_NAME.size(), enum_name.size());
      }
      ltPos = enum_name.find(E0_PREFIX_NAME);
      if (ltPos != std::string::npos) {
        enum_name = enum_name.substr(0, ltPos) + enum_name.substr(ltPos+ E0_PREFIX_NAME.size(), enum_name.size());
      }
    } else {
      // std.core::Option
      pos = enum_name.find("::");
      if (pos != std::string::npos) {
        enum_name = enum_name.substr(pos + 2);  // size of "::" is 2
      }
    }
    // Memory Layout of Reflection
    //   enumCtors -> [name + typeinfo]...
    //   u32 modifier
    //   u32 enumCtorInfoCnt
    uint32_t ctor_num = 0;
    ReflectModifyType enum_kind = GetEnumKind(typeInfo);
    // enumCtor count's offset is 12.
    m_process->ReadMemory((uint64_t)typeInfo.reflection + 12, &ctor_num, sizeof(uint32_t), error);
    if (ctor_num == 0) {
      return CompilerType();
    }
    uint64_t ctors_addr = (uint64_t)typeInfo.reflection;
    uint64_t ctor_para_addr = 0;
    m_process->ReadMemory(ctors_addr, &ctor_para_addr, sizeof(uint64_t), error);
    ctor_para_addr = ctors_addr + ctor_para_addr;

    auto enum_type = CreateEnumType(ast, *m_process, enum_name, ctor_num, ctor_para_addr);
    if (enum_kind == ReflectModifyType::RMT_ENUM_KIND0) {
      return enum_type;
    }
    // std.core::Option
    ConstString match("^std[.]core::(Enum\\$)?Option<.+>( \\*)?$");
    RegularExpression regex(match.GetStringRef());
    if (regex.Execute(type_name.AsCString())) {
      return GetDynamicOptionType(ast, typeInfo, type_name);
    }

    if (enum_kind == ReflectModifyType::RMT_ENUM_KIND2) {
      std::string enum_type_name(type_name.AsCString());
      if (auto pos = enum_type_name.find("E1$"); pos != std::string::npos) {
          enum_type_name.replace(pos, std::string("E1$").size(), "E2$");
      }
      type_name.SetCString(enum_type_name.c_str());
      return CreateEnum2Type(ast, enum_type, type_name, ctor_num, ctor_para_addr);
    }
    if (enum_kind == ReflectModifyType::RMT_ENUM_KIND3) {
      std::string enum_type_name(type_name.AsCString());
      if (auto pos = enum_type_name.find("E1$"); pos != std::string::npos) {
          enum_type_name.replace(pos, std::string("E1$").size(), "E3$");
      }
      type_name.SetCString(enum_type_name.c_str());
    }

    auto dynamic_type = ast.GetTypeForIdentifier<clang::CXXRecordDecl>(type_name);
    if (dynamic_type.IsValid()) {
      return dynamic_type;
    }
    dynamic_type = ast.CreateRecordType(nullptr, OptionalClangModuleID(), lldb::eAccessPublic,
                    type_name.GetCString(), clang::TTK_Struct, lldb::eLanguageTypeC);
    ast.StartTagDeclarationDefinition(dynamic_type);
    auto ti_type = ast.GetBasicType(eBasicTypeVoid).GetPointerType();
    ast.AddFieldToRecordType(dynamic_type, "$ti*", ti_type, lldb::eAccessPublic, 0);
    if (enum_kind == ReflectModifyType::RMT_ENUM_KIND3) {
      CreateAndAddInheritTypeToEnum3Type(dynamic_type, enum_type, ast, ctor_num, ctor_para_addr);
    } else {
      CreateAndAddInheritTypeToRecordType(dynamic_type, enum_type, ast, ctor_num, ctor_para_addr);
    }
    ast.CompleteTagDeclarationDefinition(dynamic_type);

    return dynamic_type;
}

CompilerType ItaniumABILanguageRuntime::GetDynamicTypeFromPrimitiveType(TypeSystemClang& ast, TypeInfo& typeInfo) {
  CompilerType dynamic_type;
  auto typekind = static_cast<UGTypeKind>(typeInfo.type);
  switch (typekind) {
    case UGTypeKind::UG_NOTHING:
      break;
    case UGTypeKind::UG_UNIT: {
      dynamic_type = ast.CreateRecordType(nullptr, OptionalClangModuleID(),
          lldb::eAccessPublic, "Unit", clang::TTK_Struct, lldb::eLanguageTypeC);
      ast.StartTagDeclarationDefinition(dynamic_type);
      ast.CompleteTagDeclarationDefinition(dynamic_type);
      break;
    }
    case UGTypeKind::UG_BOOLEAN:
      dynamic_type = ast.GetBasicType(eBasicTypeBool).CreateTypedef(
          "Bool", ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
      break;
    case UGTypeKind::UG_RUNE:
      dynamic_type = ast.GetBasicType(eBasicTypeChar32).CreateTypedef(
          "Rune", ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
      break;
    case UGTypeKind::UG_UINT8:
      dynamic_type = ast.GetBasicType(eBasicTypeUnsignedChar).CreateTypedef(
          "UInt8", ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
      break;
    case UGTypeKind::UG_UINT16:
      dynamic_type = ast.GetBasicType(eBasicTypeUnsignedShort).CreateTypedef(
          "UInt16", ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
      break;
    case UGTypeKind::UG_UINT32:
      dynamic_type = ast.GetBasicType(eBasicTypeUnsignedInt).CreateTypedef(
          "UInt32", ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
      break;
    case UGTypeKind::UG_UINT64:
      dynamic_type = ast.GetBasicType(eBasicTypeUnsignedLongLong).CreateTypedef(
          "UInt64", ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
      break;
    case UGTypeKind::UG_UINT_NATIVE:
      dynamic_type = ast.GetBasicType(eBasicTypeUnsignedLongLong).CreateTypedef(
          "UIntNative", ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
      break;
    case UGTypeKind::UG_INT8:
      dynamic_type = ast.GetBasicType(eBasicTypeSignedChar).CreateTypedef(
          "Int8", ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
      break;
    case UGTypeKind::UG_INT16:
      dynamic_type = ast.GetBasicType(eBasicTypeShort).CreateTypedef(
          "Int16", ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
      break;
    case UGTypeKind::UG_INT32:
      dynamic_type = ast.GetBasicType(eBasicTypeInt).CreateTypedef(
          "Int32", ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
      break;
    case UGTypeKind::UG_INT64:
      dynamic_type = ast.GetBasicType(eBasicTypeLongLong).CreateTypedef(
          "Int64", ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
      break;
    case UGTypeKind::UG_INT_NATIVE:
      dynamic_type = ast.GetBasicType(eBasicTypeLongLong).CreateTypedef(
          "IntNative", ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
      break;
    case UGTypeKind::UG_FLOAT16:
      dynamic_type = ast.GetBasicType(eBasicTypeHalf).CreateTypedef(
          "Float16", ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
      break;
    case UGTypeKind::UG_FLOAT32:
      dynamic_type = ast.GetBasicType(eBasicTypeFloat).CreateTypedef(
          "Float32", ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
      break;
    case UGTypeKind::UG_FLOAT64:
      dynamic_type = ast.GetBasicType(eBasicTypeDouble).CreateTypedef(
          "Float64", ast.CreateDeclContext(ast.GetTranslationUnitDecl()), 0);
      break;
    default:
      break;
  }
  return dynamic_type;
}

CompilerType ItaniumABILanguageRuntime::GetDynamicTypeFromGenericTypeInfo(
    TypeSystemClang& ast, TypeInfo& typeInfo) {
    auto type_name = GetTypeName(*m_process, typeInfo);
    if (type_name.IsEmpty()) {
      return CompilerType();
    }
    auto typekind = static_cast<UGTypeKind>(typeInfo.type);
    // For Primitive type
    if (typekind >= UGTypeKind::UG_NOTHING && typekind <= UGTypeKind::UG_FLOAT64) {
      return GetDynamicTypeFromPrimitiveType(ast, typeInfo);
    }
    // For other types.
    CompilerType dynamic_type;
    switch (typekind) {
      case UGTypeKind::UG_CLASS:
        dynamic_type = GetDynamicClassType(ast, typeInfo, type_name);
        break;
      case UGTypeKind::UG_INTERFACE:
        dynamic_type = GetDynamicInterfaceType(ast, type_name);
        break;
      case UGTypeKind::UG_RAWARRAY:
        dynamic_type = GetDynamicRawArrayType(ast, typeInfo, type_name);
        break;
      case UGTypeKind::UG_FUNC:
        dynamic_type = GetDynamicFuncType(ast, typeInfo, type_name);
        break;
      case UGTypeKind::UG_CSTRING:
        dynamic_type = GetDynamicCStringType(ast, typeInfo, type_name);
        break;
      case UGTypeKind::UG_CPOINTER:
        dynamic_type = GetDynamicCPointerType(ast, typeInfo, type_name);
        break;
      case UGTypeKind::UG_CFUNC:
        dynamic_type = GetDynamicCFuncType(ast, typeInfo, type_name);
        break;
      case UGTypeKind::UG_VARRAY:
        dynamic_type = GetDynamicVArrayType(ast, typeInfo, type_name);
        break;
      case UGTypeKind::UG_TUPLE:
        dynamic_type = GetDynamicTupleType(ast, typeInfo, type_name);
        break;
      case UGTypeKind::UG_STRUCT:
        dynamic_type = GetDynamicStructType(ast, typeInfo, type_name);
        break;
      case UGTypeKind::UG_ENUM:
      case UGTypeKind::UG_COMMON_ENUM:
        dynamic_type = GetDynamicEnumType(ast, typeInfo, type_name);
        break;
      default:
        break;
    }
    return dynamic_type;
}

bool ItaniumABILanguageRuntime::GetGenericDynamicType(ValueObject &in_value,
    TypeAndOrName &class_type_or_name, Address &dynamic_address, Value::ValueType &value_type) {
    auto generic_type = in_value.GetCompilerType();
    auto *ast = llvm::dyn_cast_or_null<TypeSystemClang>(generic_type.GetTypeSystem());
    if (!ast) {
      return false;
    }
    Status error;
    uint64_t type_info_addr = 0;
    TypeInfo typeInfo;
    m_process->ReadMemory(in_value.GetAddressOf(), &type_info_addr, sizeof(uint64_t), error);
    m_process->ReadMemory(type_info_addr, &typeInfo, sizeof(typeInfo), error);
    if (!error.Success()) {
      return false;
    }
    if (GetTypeName(*m_process, typeInfo) == in_value.GetCompilerType().GetTypeName()) {
      return false;
    }
    auto dynamic_type = GetDynamicTypeFromGenericTypeInfo(*ast, typeInfo);
    if (!dynamic_type.IsValid()) {
      return false;
    }
    in_value.SetGenericValueType(!IsRefType(typeInfo.type));
    class_type_or_name.SetCompilerType(dynamic_type);
    in_value.SetOverrideType(dynamic_type);
    dynamic_address = IsRefType(typeInfo.type) ?
        Address(in_value.GetAddressOf()) : Address(in_value.GetAddressOf() + BitsPerByte);
    value_type = Value::ValueType::LoadAddress;
    return true;
}

bool ItaniumABILanguageRuntime::GetDynamicTypeAndAddress(
    ValueObject &in_value, lldb::DynamicValueType use_dynamic,
    TypeAndOrName &class_type_or_name, Address &dynamic_address,
    Value::ValueType &value_type) {
  // For Itanium, if the type has a vtable pointer in the object, it will be at
  // offset 0 in the object.  That will point to the "address point" within the
  // vtable (not the beginning of the vtable.)  We can then look up the symbol
  // containing this "address point" and that symbol's name demangled will
  // contain the full class name. The second pointer above the "address point"
  // is the "offset_to_top".  We'll use that to get the start of the value
  // object which holds the dynamic type.
  if (in_value.GetCangjieDynamicType(class_type_or_name)) {
    if (in_value.IsCangjieGenericType()) {
      return GetGenericDynamicType(in_value, class_type_or_name, dynamic_address, value_type);
    }
    // Other types except struct already have the typeinfo type in debuginfo, and no offset 8 is required.
    dynamic_address = in_value.GetCompilerType().GetTypeClass() == lldb::eTypeClassClass ?
                        Address(in_value.GetAddressOf()) : Address(in_value.GetAddressOf() + BitsPerByte);
    value_type = Value::ValueType::LoadAddress;
    return true;
  }

  class_type_or_name.Clear();
  value_type = Value::ValueType::Scalar;

  // Only a pointer or reference type can have a different dynamic and static
  // type:
  if (!CouldHaveDynamicValue(in_value))
    return false;

  // First job, pull out the address at 0 offset from the object.
  AddressType address_type;
  lldb::addr_t original_ptr = in_value.GetPointerValue(&address_type);
  if (original_ptr == LLDB_INVALID_ADDRESS)
    return false;

  ExecutionContext exe_ctx(in_value.GetExecutionContextRef());

  Process *process = exe_ctx.GetProcessPtr();

  if (process == nullptr)
    return false;

  Status error;
  const lldb::addr_t vtable_address_point =
      process->ReadPointerFromMemory(original_ptr, error);

  if (!error.Success() || vtable_address_point == LLDB_INVALID_ADDRESS)
    return false;

  class_type_or_name = GetTypeInfoFromVTableAddress(in_value, original_ptr,
                                                    vtable_address_point);

  if (!class_type_or_name)
    return false;

  CompilerType type = class_type_or_name.GetCompilerType();
  // There can only be one type with a given name, so we've just found
  // duplicate definitions, and this one will do as well as any other. We
  // don't consider something to have a dynamic type if it is the same as
  // the static type.  So compare against the value we were handed.
  if (!type)
    return true;

  if (TypeSystemClang::AreTypesSame(in_value.GetCompilerType(), type)) {
    // The dynamic type we found was the same type, so we don't have a
    // dynamic type here...
    return false;
  }

  // The offset_to_top is two pointers above the vtable pointer.
  const uint32_t addr_byte_size = process->GetAddressByteSize();
  const lldb::addr_t offset_to_top_location =
      vtable_address_point - 2 * addr_byte_size;
  // Watch for underflow, offset_to_top_location should be less than
  // vtable_address_point
  if (offset_to_top_location >= vtable_address_point)
    return false;
  const int64_t offset_to_top = process->ReadSignedIntegerFromMemory(
      offset_to_top_location, addr_byte_size, INT64_MIN, error);

  if (offset_to_top == INT64_MIN)
    return false;
  // So the dynamic type is a value that starts at offset_to_top above
  // the original address.
  lldb::addr_t dynamic_addr = original_ptr + offset_to_top;
  if (!process->GetTarget().GetSectionLoadList().ResolveLoadAddress(
          dynamic_addr, dynamic_address)) {
    dynamic_address.SetRawAddress(dynamic_addr);
  }
  return true;
}

TypeAndOrName ItaniumABILanguageRuntime::FixUpDynamicType(
    const TypeAndOrName &type_and_or_name, ValueObject &static_value) {
  CompilerType static_type(static_value.GetCompilerType());
  Flags static_type_flags(static_type.GetTypeInfo());

  TypeAndOrName ret(type_and_or_name);
  if (type_and_or_name.HasType()) {
    // The type will always be the type of the dynamic object.  If our parent's
    // type was a pointer, then our type should be a pointer to the type of the
    // dynamic object.  If a reference, then the original type should be
    // okay...
    CompilerType orig_type = type_and_or_name.GetCompilerType();
    CompilerType corrected_type = orig_type;
    if (static_type_flags.AllSet(eTypeIsPointer))
      corrected_type = orig_type.GetPointerType();
    else if (static_type_flags.AllSet(eTypeIsReference))
      corrected_type = orig_type.GetLValueReferenceType();
    ret.SetCompilerType(corrected_type);
  } else {
    // If we are here we need to adjust our dynamic type name to include the
    // correct & or * symbol
    std::string corrected_name(type_and_or_name.GetName().GetCString());
    if (static_type_flags.AllSet(eTypeIsPointer))
      corrected_name.append(" *");
    else if (static_type_flags.AllSet(eTypeIsReference))
      corrected_name.append(" &");
    // the parent type should be a correctly pointer'ed or referenc'ed type
    ret.SetCompilerType(static_type);
    ret.SetName(corrected_name.c_str());
  }
  return ret;
}

// Static Functions
LanguageRuntime *
ItaniumABILanguageRuntime::CreateInstance(Process *process,
                                          lldb::LanguageType language) {
  // FIXME: We have to check the process and make sure we actually know that
  // this process supports
  // the Itanium ABI.
  if (language == eLanguageTypeC_plus_plus ||
      language == eLanguageTypeC_plus_plus_03 ||
      language == eLanguageTypeC_plus_plus_11 ||
      language == eLanguageTypeC_plus_plus_14)
    return new ItaniumABILanguageRuntime(process);
  else
    return nullptr;
}

class CommandObjectMultiwordItaniumABI_Demangle : public CommandObjectParsed {
public:
  CommandObjectMultiwordItaniumABI_Demangle(CommandInterpreter &interpreter)
      : CommandObjectParsed(interpreter, "demangle",
                            "Demangle a C++ mangled name.",
                            "language cplusplus demangle") {
    CommandArgumentEntry arg;
    CommandArgumentData index_arg;

    // Define the first (and only) variant of this arg.
    index_arg.arg_type = eArgTypeSymbol;
    index_arg.arg_repetition = eArgRepeatPlus;

    // There is only one variant this argument could be; put it into the
    // argument entry.
    arg.push_back(index_arg);

    // Push the data for the first argument into the m_arguments vector.
    m_arguments.push_back(arg);
  }

  ~CommandObjectMultiwordItaniumABI_Demangle() override = default;

protected:
  bool DoExecute(Args &command, CommandReturnObject &result) override {
    bool demangled_any = false;
    bool error_any = false;
    for (auto &entry : command.entries()) {
      if (entry.ref().empty())
        continue;

      // the actual Mangled class should be strict about this, but on the
      // command line if you're copying mangled names out of 'nm' on Darwin,
      // they will come out with an extra underscore - be willing to strip this
      // on behalf of the user.   This is the moral equivalent of the -_/-n
      // options to c++filt
      auto name = entry.ref();
      if (name.startswith("__Z"))
        name = name.drop_front();

      Mangled mangled(name);
      if (mangled.GuessLanguage() == lldb::eLanguageTypeC_plus_plus) {
        ConstString demangled(mangled.GetDisplayDemangledName());
        demangled_any = true;
        result.AppendMessageWithFormat("%s ---> %s\n", entry.c_str(),
                                       demangled.GetCString());
      } else {
        error_any = true;
        result.AppendErrorWithFormat("%s is not a valid C++ mangled name\n",
                                     entry.ref().str().c_str());
      }
    }

    result.SetStatus(
        error_any ? lldb::eReturnStatusFailed
                  : (demangled_any ? lldb::eReturnStatusSuccessFinishResult
                                   : lldb::eReturnStatusSuccessFinishNoResult));
    return result.Succeeded();
  }
};

class CommandObjectMultiwordItaniumABI : public CommandObjectMultiword {
public:
  CommandObjectMultiwordItaniumABI(CommandInterpreter &interpreter)
      : CommandObjectMultiword(
            interpreter, "cplusplus",
            "Commands for operating on the C++ language runtime.",
            "cplusplus <subcommand> [<subcommand-options>]") {
    LoadSubCommand(
        "demangle",
        CommandObjectSP(
            new CommandObjectMultiwordItaniumABI_Demangle(interpreter)));
  }

  ~CommandObjectMultiwordItaniumABI() override = default;
};

void ItaniumABILanguageRuntime::Initialize() {
  PluginManager::RegisterPlugin(
      GetPluginNameStatic(), "Itanium ABI for the C++ language", CreateInstance,
      [](CommandInterpreter &interpreter) -> lldb::CommandObjectSP {
        return CommandObjectSP(
            new CommandObjectMultiwordItaniumABI(interpreter));
      });
}

void ItaniumABILanguageRuntime::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}

BreakpointResolverSP ItaniumABILanguageRuntime::CreateExceptionResolver(
    const BreakpointSP &bkpt, bool catch_bp, bool throw_bp) {
  return CreateExceptionResolver(bkpt, catch_bp, throw_bp, false);
}

BreakpointResolverSP ItaniumABILanguageRuntime::CreateExceptionResolver(
    const BreakpointSP &bkpt, bool catch_bp, bool throw_bp,
    bool for_expressions) {
  // One complication here is that most users DON'T want to stop at
  // __cxa_allocate_expression, but until we can do anything better with
  // predicting unwinding the expression parser does.  So we have two forms of
  // the exception breakpoints, one for expressions that leaves out
  // __cxa_allocate_exception, and one that includes it. The
  // SetExceptionBreakpoints does the latter, the CreateExceptionBreakpoint in
  // the runtime the former.
  static const char *g_catch_name = "__cxa_begin_catch";
  static const char *g_throw_name1 = "__cxa_throw";
  static const char *g_throw_name2 = "__cxa_rethrow";
  static const char *g_exception_throw_name = "__cxa_allocate_exception";
  std::vector<const char *> exception_names;
  exception_names.reserve(4);
  if (catch_bp)
    exception_names.push_back(g_catch_name);

  if (throw_bp) {
    exception_names.push_back(g_throw_name1);
    exception_names.push_back(g_throw_name2);
  }

  if (for_expressions)
    exception_names.push_back(g_exception_throw_name);

  BreakpointResolverSP resolver_sp(new BreakpointResolverName(
      bkpt, exception_names.data(), exception_names.size(),
      eFunctionNameTypeBase, eLanguageTypeUnknown, 0, eLazyBoolNo));

  return resolver_sp;
}

lldb::SearchFilterSP ItaniumABILanguageRuntime::CreateExceptionSearchFilter() {
  Target &target = m_process->GetTarget();

  FileSpecList filter_modules;
  if (target.GetArchitecture().GetTriple().getVendor() == llvm::Triple::Apple) {
    // Limit the number of modules that are searched for these breakpoints for
    // Apple binaries.
    filter_modules.EmplaceBack("libc++abi.dylib");
    filter_modules.EmplaceBack("libSystem.B.dylib");
  }
  return target.GetSearchFilterForModuleList(&filter_modules);
}

lldb::BreakpointSP ItaniumABILanguageRuntime::CreateExceptionBreakpoint(
    bool catch_bp, bool throw_bp, bool for_expressions, bool is_internal) {
  Target &target = m_process->GetTarget();
  FileSpecList filter_modules;
  BreakpointResolverSP exception_resolver_sp =
      CreateExceptionResolver(nullptr, catch_bp, throw_bp, for_expressions);
  SearchFilterSP filter_sp(CreateExceptionSearchFilter());
  const bool hardware = false;
  const bool resolve_indirect_functions = false;
  return target.CreateBreakpoint(filter_sp, exception_resolver_sp, is_internal,
                                 hardware, resolve_indirect_functions);
}

void ItaniumABILanguageRuntime::SetExceptionBreakpoints() {
  if (!m_process)
    return;

  const bool catch_bp = false;
  const bool throw_bp = true;
  const bool is_internal = true;
  const bool for_expressions = true;

  // For the exception breakpoints set by the Expression parser, we'll be a
  // little more aggressive and stop at exception allocation as well.

  if (m_cxx_exception_bp_sp) {
    m_cxx_exception_bp_sp->SetEnabled(true);
  } else {
    m_cxx_exception_bp_sp = CreateExceptionBreakpoint(
        catch_bp, throw_bp, for_expressions, is_internal);
    if (m_cxx_exception_bp_sp)
      m_cxx_exception_bp_sp->SetBreakpointKind("c++ exception");
  }
}

void ItaniumABILanguageRuntime::ClearExceptionBreakpoints() {
  if (!m_process)
    return;

  if (m_cxx_exception_bp_sp) {
    m_cxx_exception_bp_sp->SetEnabled(false);
  }
}

bool ItaniumABILanguageRuntime::ExceptionBreakpointsAreSet() {
  return m_cxx_exception_bp_sp && m_cxx_exception_bp_sp->IsEnabled();
}

bool ItaniumABILanguageRuntime::ExceptionBreakpointsExplainStop(
    lldb::StopInfoSP stop_reason) {
  if (!m_process)
    return false;

  if (!stop_reason || stop_reason->GetStopReason() != eStopReasonBreakpoint)
    return false;

  uint64_t break_site_id = stop_reason->GetValue();
  return m_process->GetBreakpointSiteList().BreakpointSiteContainsBreakpoint(
      break_site_id, m_cxx_exception_bp_sp->GetID());
}

ValueObjectSP ItaniumABILanguageRuntime::GetExceptionObjectForThread(
    ThreadSP thread_sp) {
  if (!thread_sp->SafeToCallFunctions())
    return {};

  TypeSystemClang *clang_ast_context =
      ScratchTypeSystemClang::GetForTarget(m_process->GetTarget());
  if (!clang_ast_context)
    return {};

  CompilerType voidstar =
      clang_ast_context->GetBasicType(eBasicTypeVoid).GetPointerType();

  DiagnosticManager diagnostics;
  ExecutionContext exe_ctx;
  EvaluateExpressionOptions options;

  options.SetUnwindOnError(true);
  options.SetIgnoreBreakpoints(true);
  options.SetStopOthers(true);
  options.SetTimeout(m_process->GetUtilityExpressionTimeout());
  options.SetTryAllThreads(false);
  thread_sp->CalculateExecutionContext(exe_ctx);

  const ModuleList &modules = m_process->GetTarget().GetImages();
  SymbolContextList contexts;
  SymbolContext context;

  modules.FindSymbolsWithNameAndType(
      ConstString("__cxa_current_exception_type"), eSymbolTypeCode, contexts);
  contexts.GetContextAtIndex(0, context);
  if (!context.symbol) {
    return {};
  }
  Address addr = context.symbol->GetAddress();

  Status error;
  FunctionCaller *function_caller =
      m_process->GetTarget().GetFunctionCallerForLanguage(
          eLanguageTypeC, voidstar, addr, ValueList(), "caller", error);

  ExpressionResults func_call_ret;
  Value results;
  func_call_ret = function_caller->ExecuteFunction(exe_ctx, nullptr, options,
                                                   diagnostics, results);
  if (func_call_ret != eExpressionCompleted || !error.Success()) {
    return ValueObjectSP();
  }

  size_t ptr_size = m_process->GetAddressByteSize();
  addr_t result_ptr = results.GetScalar().ULongLong(LLDB_INVALID_ADDRESS);
  addr_t exception_addr =
      m_process->ReadPointerFromMemory(result_ptr - ptr_size, error);

  if (!error.Success()) {
    return ValueObjectSP();
  }

  lldb_private::formatters::InferiorSizedWord exception_isw(exception_addr,
                                                            *m_process);
  ValueObjectSP exception = ValueObject::CreateValueObjectFromData(
      "exception", exception_isw.GetAsData(m_process->GetByteOrder()), exe_ctx,
      voidstar);
  exception = exception->GetDynamicValue(eDynamicDontRunTarget);

  return exception;
}

TypeAndOrName ItaniumABILanguageRuntime::GetDynamicTypeInfo(
    const lldb_private::Address &vtable_addr) {
  std::lock_guard<std::mutex> locker(m_dynamic_type_map_mutex);
  DynamicTypeCache::const_iterator pos = m_dynamic_type_map.find(vtable_addr);
  if (pos == m_dynamic_type_map.end())
    return TypeAndOrName();
  else
    return pos->second;
}

void ItaniumABILanguageRuntime::SetDynamicTypeInfo(
    const lldb_private::Address &vtable_addr, const TypeAndOrName &type_info) {
  std::lock_guard<std::mutex> locker(m_dynamic_type_map_mutex);
  m_dynamic_type_map[vtable_addr] = type_info;
}
