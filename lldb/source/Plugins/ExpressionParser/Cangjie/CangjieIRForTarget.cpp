//===-- CangjieIRForTarget.cpp --------------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#include "CangjieIRForTarget.h"
#include "CangjieDeclMap.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Instructions.h"
#include "lldb/Expression/IRExecutionUnit.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"

using namespace llvm;
using lldb_private::LLDBLog;

CangjieIRForTarget::FunctionValueCache::FunctionValueCache(Maker const &maker)
    : m_maker(maker), m_values() {}

CangjieIRForTarget::FunctionValueCache::~FunctionValueCache() = default;

static llvm::Value *FindEntryInstruction(llvm::Function *function) {
  if (function->empty()) {
    return nullptr;
  }

  return function->getEntryBlock().getFirstNonPHIOrDbg();
}

llvm::Value * CangjieIRForTarget::FunctionValueCache::GetValue(
  llvm::Function *function) {
  if (m_values.find(function) == m_values.end()) {
    llvm::Value *ret = m_maker(function);
    m_values[function] = ret;
    return ret;
  } else {
    return m_values[function];
  }
}

CangjieIRForTarget::CangjieIRForTarget(lldb_private::Materializer *materializer,
                                       llvm::Module *module,
                                       lldb_private::ConstString func_name,
                                       lldb_private::CangjieDeclMap *decl_map,
                                       lldb_private::ExecutionContext &exe_ctx)
    : m_materializer(materializer), m_module(module), m_func_name(func_name),
      m_exe_ctx(exe_ctx), m_decl_map(decl_map), m_entry_instruction_finder(FindEntryInstruction) {
  m_this_obj_name.SetCString("_CN7default20__lldb_injected_selfE");
  m_super_obj_name.SetCString("_CN7default22$__lldb_injected_superE");
}

bool CangjieIRForTarget::ReplaceIRGlobals() {
  Function *const llvm_function =
      m_func_name.IsEmpty() ? nullptr
                            : m_module->getFunction(m_func_name.GetStringRef());
  if (!llvm_function) {
    return false;
  }

  llvm::Function::arg_iterator iter(llvm_function->arg_begin());
  if (iter == llvm_function->arg_end()) {
    return false;
  }
  Argument *argument = &*iter;
  while (argument->getName().equals("this") || argument->getName().equals("") || argument->getName().equals("$BP")) {
    ++iter;
    if (iter == llvm_function->arg_end()) {
      return false;
    }
    argument = &*iter;
  }

  llvm::BasicBlock &entry_block(llvm_function->getEntryBlock());
  LLVMContext &context(m_module->getContext());
  llvm::IntegerType *offset_type(llvm::Type::getInt32Ty(context));

  for (auto it = m_args.begin(); it != m_args.end(); it++) {
    llvm::Value *value = it->m_llvm_value;
    lldb::offset_t offset = it->m_offset;
    bool is_reference = it->m_is_reference;
    auto name = it->m_name;
    if (name == m_super_obj_name) {
      offset = m_args.begin()->m_offset;
    }
    if (name == m_this_obj_name && m_this_is_replaced) {
      continue;
    }
    if (value) {
      FunctionValueCache body_result_maker([this, is_reference, offset_type, offset, argument,
          value](llvm::Function *function) -> llvm::Value * {
          llvm::Instruction *entry_instruction = llvm::cast<llvm::Instruction>(
              m_entry_instruction_finder.GetValue(function));

          llvm::Type *int8Ty = llvm::Type::getInt8Ty(function->getContext());
          ConstantInt *offset_int(
              ConstantInt::get(offset_type, offset, true));
          GetElementPtrInst *get_element_ptr = GetElementPtrInst::Create(
              int8Ty, argument, offset_int, "", entry_instruction);
          if (is_reference) {
            BitCastInst *bit_cast = new BitCastInst(
              get_element_ptr, value->getType(), "", entry_instruction);
            return bit_cast;
          }
          BitCastInst *bit_cast = new BitCastInst(
              get_element_ptr, value->getType()->getPointerTo(), "",
              entry_instruction);

          LoadInst *load = new LoadInst(value->getType(), bit_cast, "",
                                        entry_instruction);
          return load;
        });

      if (Constant *constant = dyn_cast<Constant>(value)) {
        if (!UnfoldConstant(constant, llvm_function, body_result_maker,
                            m_entry_instruction_finder)) {
          return false;
        }
      } else if (llvm::Instruction *instruction = dyn_cast<llvm::Instruction>(value)) {
        if (instruction->getParent()->getParent() != llvm_function) {
          return false;
        }
        value->replaceAllUsesWith(
            body_result_maker.GetValue(instruction->getParent()->getParent()));
      } else {
        return false;
      }
      if (GlobalVariable *var = dyn_cast<GlobalVariable>(value))
        var->eraseFromParent();
    }

    if (m_replace_this_object && name == m_this_obj_name) {
      m_this_is_replaced = true;
      m_replace_this_object = false;
      break;
    }
  }

  return true;
}

bool CangjieIRForTarget::UnfoldConstant(Constant *old_constant,
                                        llvm::Function *llvm_function,
                                        FunctionValueCache &value_maker,
                                        FunctionValueCache &entry_instruction_finder) {
  SmallVector<llvm::User *, 16> users;

  for (llvm::User *u : old_constant->users())
    users.push_back(u);

  for (size_t i = 0; i < users.size(); ++i) {
    User *user = users[i];

    if (llvm::Constant *constant = dyn_cast<llvm::Constant>(user)) {
      if (llvm::ConstantExpr *constant_expr = dyn_cast<llvm::ConstantExpr>(constant)) {
        switch (constant_expr->getOpcode()) {
          default:

            return false;
          case llvm::Instruction::GetElementPtr: {
            // GetElementPtrConstantExpr, OperandList[0] is base, OperandList[1]... are indices

            FunctionValueCache get_element_pointer_maker(
                [&value_maker, &entry_instruction_finder, old_constant,
                constant_expr](llvm::Function *function) -> llvm::Value * {
                  auto *gep = cast<llvm::GEPOperator>(constant_expr);
                  llvm::Value *ptr = gep->getPointerOperand();

                  if (ptr == old_constant)
                    ptr = value_maker.GetValue(function);

                  std::vector<llvm::Value *> index_vector;
                  for (llvm::Value *operand : gep->indices()) {
                    if (operand == old_constant)
                      operand = value_maker.GetValue(function);

                    index_vector.push_back(operand);
                  }

                  ArrayRef<llvm::Value *> indices(index_vector);

                  return GetElementPtrInst::Create(
                      gep->getSourceElementType(), ptr, indices, "",
                      llvm::cast<llvm::Instruction>(
                          entry_instruction_finder.GetValue(function)));
                });

            if (!UnfoldConstant(constant_expr, llvm_function,
                                get_element_pointer_maker,
                                entry_instruction_finder))
              return false;
          } break;
          case llvm::Instruction::BitCast: {
            FunctionValueCache bit_cast_maker(
                [&value_maker, &entry_instruction_finder, old_constant,
                constant_expr](llvm::Function *function) -> llvm::Value * {
                  // UnaryExpr, OperandList[0] is value
                  if (old_constant != constant_expr->getOperand(0)) {
                    return constant_expr;
                  }
                  return new BitCastInst(
                      value_maker.GetValue(function), constant_expr->getType(),
                      "", llvm::cast<llvm::Instruction>(
                              entry_instruction_finder.GetValue(function)));
                });

            if (!UnfoldConstant(constant_expr, llvm_function, bit_cast_maker,
                                entry_instruction_finder))
              return false;
          } break;
        }
      } else {
        return false;
      }
    } else {
      if (llvm::Instruction *inst = llvm::dyn_cast<llvm::Instruction>(user)) {
        if (llvm_function && inst->getParent()->getParent() != llvm_function) {
          return false;
        }
        inst->replaceUsesOfWith(
            old_constant, value_maker.GetValue(inst->getParent()->getParent()));
      } else {
        return false;
      }
    }
  }

  if (!isa<GlobalValue>(old_constant)) {
    old_constant->destroyConstant();
  }

  return true;
}

bool CangjieIRForTarget::IsReferenceType(const lldb_private::CompilerType &ty) {
  std::string type_name = ty.GetTypeName().AsCString();
  if (ty.GetTypeClass() == lldb::eTypeClassFunction) {
    return false;
  }
  if (ty.GetTypeClass() == lldb::eTypeClassEnumeration) {
    return false;
  }
  // For UG, enum with args is reference type.
  if (ty.GetTypeClass() == lldb::eTypeClassStruct &&
      type_name.find(lldb_private::ENUM_PREFIX_NAME) != std::string::npos) {
    return true;
  }
  if (ty.GetTypeClass() == lldb::eTypeClassStruct &&
      type_name.find(lldb_private::E2_PREFIX_NAME_OPTION_LIKE) != std::string::npos) {
    for (uint32_t i = 0; i < ty.GetNumFields(); i++) {
      std::string member_name;
      auto child_type = ty.GetFieldAtIndex(i, member_name, nullptr, nullptr, nullptr);
      if (member_name == "val") {
        return IsReferenceType(child_type);
      }
    }
  }
  std::vector<std::string> value_type = {"Float64", "Float32", "Float16", "Int64", "Int32", "Int16", "Int8",
                                          "UInt64", "UInt32", "UInt16", "UInt8", "Bool", "Unit", "IntNative",
                                          "UIntNative", "std.core.String", "Rune",};
  lldb_private::ConstString match(
    "(^Tuple<.+>$)|(^std.core::Range<.+>$)|(^VArray<.+>$)|(^Enum\\$)|(^std[.]core::Array<.+>$)");
  lldb_private::RegularExpression regex(match.GetStringRef());
  if (regex.Execute(type_name.c_str())) {
    return false;
  }
  lldb_private::ConstString func_match("(.+::|^)\\(.*\\)( ?)->.+$");
  lldb_private::RegularExpression func_regex(func_match.GetStringRef());
  if (func_regex.Execute(type_name.c_str())) {
    return true;
  }
  if (std::find(value_type.begin(), value_type.end(), type_name) != value_type.end()) {
    return false;
  }
  if (ty.GetTypeClass() == lldb::eTypeClassStruct) {
    return false;
  }
  return true;
}

bool CangjieIRForTarget::IsReferenceType(lldb::VariableSP value) {
  lldb_private::Log *log = GetLog(LLDBLog::Expressions);
  auto frame = m_exe_ctx.GetFramePtr();
  if (frame == nullptr) {
    return false;
  }

  auto valobj = frame->GetValueObjectForFrameVariable(value, lldb::DynamicValueType::eNoDynamicValues);
  if (valobj == nullptr) {
    return false;
  }
  bool isRef = IsReferenceType(valobj->GetCompilerType());
  if (log) {
    LLDB_LOGF(log, "variable [%s]'s type is ref[%d].", valobj->GetName().AsCString(), isRef);
  }
  return isRef;
}

void CangjieIRForTarget::AddVariableToArgs() {
  lldb_private::Log *log = GetLog(LLDBLog::Expressions);
  for (llvm::GlobalVariable &global_var : m_module->globals()) {
    lldb_private::ConstString global_name(global_var.getName().data());
    lldb_private::Mangled mangled_name(global_name);
    lldb_private::ConstString demanglede_name = mangled_name.GetName();
    if (demanglede_name.GetStringRef().startswith("__lldb_locals::")) {
      demanglede_name.SetCString(demanglede_name.GetStringRef().substr(strlen("__lldb_locals::")).data());
    }

    lldb::VariableSP value;
    if (global_name == m_this_obj_name) {
      m_replace_this_object = true;
      value = m_decl_map->FindParsedVarByName(lldb_private::ConstString("this"));
    } else if (global_name == m_super_obj_name) {
      ArgsForIR arg(global_name, &global_var, 0, nullptr);
      arg.m_is_reference = true;
      m_args.push_back(arg);
      if (log) {
        LLDB_LOGF(log, "llvm ir: add variable [%s] to Args.", global_name.AsCString());
      }
    } else {
      value = m_decl_map->FindParsedVarByName(demanglede_name);
    }

    if (value != nullptr) {
      lldb_private::Status err;
      auto offset = m_materializer->AddVariable(value, err);
      if (err.Fail()) {
        continue;
      }
      llvm::Value *llvm_value = &global_var;
      ArgsForIR arg(global_name, llvm_value, offset, value);

      if (global_name == m_this_obj_name) {
        arg.m_is_reference = IsReferenceType(value);
        m_args.insert(m_args.begin(), arg);
        continue;
      } else {
        arg.m_is_reference = IsReferenceType(value);
      }
      if (log) {
        LLDB_LOGF(log, "llvm ir: add variable [%s] to Args.", global_name.AsCString());
      }
      m_args.push_back(arg);
    }
  }
}

void CangjieIRForTarget::RemoveGlobalInitFunctionCall() {
  for (llvm::Function &function : *m_module) {
    for (llvm::BasicBlock &bb : function) {
      RemoveGlobalInitFunctionCall(bb);
    }
  }
}

void CangjieIRForTarget::RemoveGlobalInitFunctionCall(llvm::BasicBlock &basic_block) {
  std::vector<llvm::CallInst *> calls_to_remove;

  for (llvm::Instruction &inst : basic_block) {
    llvm::CallInst *call = dyn_cast<llvm::CallInst>(&inst);
    if (!call) {
      continue;
    }

    llvm::Function *func = call->getCalledFunction();
    if (!func) {
      continue;
    }

    auto func_name = func->getName();
    if (func_name.startswith("_CGP4expr") || func_name.startswith("_CGP13__lldb_locals")) {
      calls_to_remove.push_back(call);
    }
  }

  for (llvm::CallInst *ci : calls_to_remove) {
    ci->eraseFromParent();
  }
}

CangjieIRForTarget::ArgsForIR::ArgsForIR(lldb_private::ConstString name, llvm::Value *value,
                                         int32_t offset, lldb::VariableSP variable)
    : m_name(name), m_llvm_value(value), m_offset(offset), m_value(variable) {}

CangjieIRForTarget::ArgsForIR::~ArgsForIR() = default;

void CangjieIRForTarget::SetPlatformInfo() {
  auto arch = m_exe_ctx.GetTargetSP()->GetArchitecture();
  if (!arch.IsValid()) {
    return;
  }
  llvm::Triple::OSType ostype = arch.GetTriple().getOS();
  llvm::Triple::ArchType archtype = arch.GetTriple().getArch();
  auto env = arch.GetTriple().getEnvironmentName();
  if (ostype == llvm::Triple::Linux && archtype == llvm::Triple::ArchType::x86_64) {
    m_module->setDataLayout("e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128");
  }
  if (ostype == llvm::Triple::Win32 && archtype == llvm::Triple::ArchType::x86_64) {
    m_module->setTargetTriple("x86_64-w64-windows-gnu");
    m_module->setDataLayout("e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128");
  }
  if (ostype == llvm::Triple::Linux && archtype == llvm::Triple::ArchType::aarch64) {
    if (env.equals("ohos")) {
      m_module->setTargetTriple("aarch64-unknown-linux-ohos");
    }
    if (env.equals("android")) {
      m_module->setTargetTriple("aarch64-unknown-linux-android");
    }
    m_module->setDataLayout("e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128");
  }
  if (ostype == llvm::Triple::Linux && archtype == llvm::Triple::ArchType::x86_64) {
    if (env.equals("ohos")) {
      m_module->setTargetTriple("x86_64-unknown-linux-ohos");
      m_module->setDataLayout("e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128");
    }
  }
  if (ostype == llvm::Triple::Darwin || ostype == llvm::Triple::MacOSX) {
    if (archtype == llvm::Triple::ArchType::aarch64) {
      m_module->setTargetTriple(arch.GetTriple().str().c_str());
      m_module->setDataLayout("e-m:o-i64:64-i128:128-n32:64-S128");
    } else if (archtype == llvm::Triple::ArchType::x86_64) {
      m_module->setDataLayout("e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128");
    }
  }
  if (ostype == llvm::Triple::IOS && archtype == llvm::Triple::ArchType::aarch64) {
    m_module->setTargetTriple(arch.GetTriple().str().c_str());
    m_module->setDataLayout("e-m:o-i64:64-i128:128-n32:64-S128-Fn32");
  }
}

llvm::StringRef CangjieIRForTarget::FindExtendFunction()
{
  for (llvm::Function &func : m_module->functions()) {
    if (func.getName().startswith("_CN11__cjdb_exprUexpr.temp$X") && func.getName().endswith("__lldb_wrapped_exprHPu")) {
      return func.getName();
    }
  }

  return "";
}

bool CangjieIRForTarget::HandleIRForExecute() {
  // First: Collect all variables that need to be replaced.
  AddVariableToArgs();

  auto extend_func = FindExtendFunction();
  if (!extend_func.empty()) {
    // If we need to replace this variable, this variable will always be the first.
    auto this_var = m_args.begin()->m_name;
    // If the class context does not contain the this variable, the struct is zero size.
    // __lldb_injected_self will be deleted by codegen.
    if (this_var != m_this_obj_name) {
      m_func_name.SetCString(extend_func.data());
      m_replace_this_object = false;
    }
  }

  // Check whether the "this" variable needs to be replaced.
  if (m_replace_this_object) {
    ReplaceIRGlobals();
    m_func_name.SetCString(extend_func.data());
  }

  // Replace all result variables and variables in the program with parameters of the expr function.
  ReplaceIRGlobals();

  RemoveGlobalInitFunctionCall();

  SetPlatformInfo();
  return true;
}

lldb_private::CompilerType CangjieIRForTarget::GetCompilerTypeByName(std::string &name) {
  std::string demangled_name = lldb_private::Mangled::GetDemangledTypeName(name);
  lldb_private::Log *log = GetLog(LLDBLog::Expressions);
  auto pos = demangled_name.find(":");
  if (pos != std::string::npos && pos != demangled_name.find("::")) {
    // org:a::AA
    demangled_name.replace(pos, 1, "::");
  }
  auto type = m_decl_map->FindParsedTypesByName(demangled_name);
  if (type.GetTypeClass() == lldb::eTypeClassStruct &&
    type.GetTypeName().GetStringRef().contains(lldb_private::E0_PREFIX_NAME)) {
    std::string member_name;
    auto child_type = type.GetFieldAtIndex(0, member_name, nullptr, nullptr, nullptr);
    if (child_type.GetTypeClass() == lldb::eTypeClassEnumeration) {
      return child_type;
    }
  }
  if (m_decl_map->IsInterfaceType(type) && m_decl_map->dynamic_type.IsValid()) {
    type = m_decl_map->dynamic_type;
  }
  if (type.IsValid()) {
    if (log) {
      LLDB_LOGF(log, "get type by name[%s] from m_parsed_types. \n", demangled_name.c_str());
    }
    return type;
  }
  // Create compilerType for generic decl. For example, define CT<T> and use CT<Int32>()
  if (!type.IsValid() && m_decl_map->IsInstantiatedOfGenericDecl(demangled_name)) {
    return m_expr_result_type;
  }
  type = m_decl_map->LookUpType(demangled_name);
  return type;
}

bool CangjieIRForTarget::CreateResultVariable(lldb_private::Materializer::PersistentVariableDelegate* result_delegate,
                                              std::string type_name, bool in_member,
                                              lldb_private::ConstString result_name) {
  lldb_private::Status err;
  lldb_private::Log *log = GetLog(LLDBLog::Expressions);
  auto result_type = GetCompilerTypeByName(type_name);

  if (log) {
    lldb_private::StreamString s;
    result_type.DumpTypeDescription(&s);
    LLDB_LOGF(log, "Result variable Decl:\n %s", s.GetData());
  }

  bool isref = IsReferenceType(result_type);
  auto offset = m_materializer->AddResultVariable(result_type, isref, true, result_delegate, err);

  // There are two wrapper kind: Function and MemberFunction.
  lldb_private::ConstString replaceFunc = m_func_name;
  if (in_member) {
    replaceFunc.SetCString(FindExtendFunction().data());
  }
  Function *llvm_function =
      replaceFunc.IsEmpty() ? nullptr
                            : m_module->getFunction(replaceFunc.GetStringRef());
  if (!llvm_function) {
    return false;
  }
  // Get llvm::Type from local variable.
  for (auto &I:llvm_function->getEntryBlock()) {
    if (AllocaInst* allocaInst = dyn_cast<AllocaInst>(&I)) {
      if (allocaInst->getName() == result_name.AsCString()) {
        llvm::Type* localResultType = allocaInst->getAllocatedType();
        auto res = llvm::cast<llvm::GlobalVariable>(
          m_module->getOrInsertGlobal(result_name.GetStringRef(), localResultType));
        allocaInst->replaceAllUsesWith(res);
        allocaInst->removeFromParent();
        break;
      }
    }
  }
  llvm::Value *result_value = m_module->getNamedValue(result_name.GetStringRef());
  ArgsForIR arg(result_name, result_value, offset, nullptr);
  arg.m_is_reference = IsReferenceType(result_type);
  m_args.push_back(arg);
  return true;
}
