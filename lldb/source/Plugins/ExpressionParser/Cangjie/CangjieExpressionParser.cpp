//===-- CangjieExpressionParser.cpp ---------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#include "CangjieExpressionParser.h"
#include "CangjieIRForTarget.h"
#include "CangjieCompilerInstance.h"
#include "cangjie/Frontend/CompilerInstance.h"
#include "cangjie/Frontend/CompilerInvocation.h"

#include "lldb/Expression/IRExecutionUnit.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"

using namespace clang;
using namespace llvm;
using namespace lldb_private;
using namespace Cangjie;
using namespace Cangjie::AST;

CangjieExpressionParser::CangjieExpressionParser(
    ExecutionContextScope *exe_scope, Expression &expr,
    bool generate_debug_info)
    : ExpressionParser(exe_scope, expr, generate_debug_info) {}

CangjieExpressionParser::~CangjieExpressionParser() = default;

bool CangjieExpressionParser::Complete(CompletionRequest &request,
                                       unsigned line, unsigned pos,
                                       unsigned typed_pos) {
  return true;
}

void CangjieExpressionParser::ResetDeclMap(
    ExecutionContext &exe_ctx, std::vector<std::string> identifiers, unsigned int fileID, std::string fileName) {
  m_expr_decl_map_up = std::make_unique<CangjieDeclMap>(exe_ctx, identifiers, fileID, fileName);
}

void CangjieExpressionParser::PrepareContextInfo(ExecutionContext &exeCtx, Cangjie::SourceManager& sm)
{
  auto frame = exeCtx.GetFramePtr();
  if (!frame) {
    return;
  }
  auto sym_ctx = frame->GetSymbolContext(lldb::eSymbolContextFunction | lldb::eSymbolContextBlock);

  // Get fileID for diag.
  auto file = sym_ctx.GetFunctionStartLineEntry().file;
  auto dir = file.GetDirectory();
  auto base = file.GetFilename();
  std::string filePath = std::string(!dir.IsEmpty() ? dir.AsCString() : "");
  std::string fileName = std::string(!base.IsEmpty() ? base.AsCString() : "");
  m_current_filename = fileName;
  auto path = FileUtil::JoinPath(filePath, fileName);
  std::string failedReason;
  auto content = FileUtil::ReadFileContent(path, failedReason);
  if (content.has_value()) {
    m_file_id = sm.AddSource(path, content.value());
  }

  // Find this variable if in the context of a class.
  auto thisVar = frame->FindVariable(ConstString("this"));
  if (!thisVar) {
    return;
  }
  m_in_class_context = true;
  if (sym_ctx.function) {
    std::string classname = SafeAsCString(thisVar->GetTypeName());
    std::string funcscope = SafeAsCString(sym_ctx.function->GetNameNoArguments());
    auto pos = funcscope.rfind("::");
    auto funcname = (pos == std::string::npos) ? funcscope: funcscope.substr(pos + 2);
    m_in_constructor = (funcname == "init" || funcname == classname);
  }
  return;
}

static void SetTripleInfoIfNeed(ArchSpec target_arch, Cangjie::Triple::Info &target_triple) {
  std::string triple = target_arch.GetTriple().str();
  if (target_arch.GetTriple().getArch() == llvm::Triple::x86_64) {
    target_triple.arch = Cangjie::Triple::ArchType::X86_64;
  } else {
    target_triple.arch = Cangjie::Triple::ArchType::AARCH64;
  }

  auto os = target_arch.GetTriple().getOS();
  if (os == llvm::Triple::Linux) {
    target_triple.os = Cangjie::Triple::OSType::LINUX;
  } else if (os == llvm::Triple::Win32) {
    target_triple.os = Cangjie::Triple::OSType::WINDOWS;
  } else if (os == llvm::Triple::Darwin || os == llvm::Triple::MacOSX) {
    target_triple.os = Cangjie::Triple::OSType::DARWIN;
    target_triple.vendor = Cangjie::Triple::Vendor::APPLE;
    target_triple.env = Cangjie::Triple::Environment::NOT_AVAILABLE;
    return;
  } else if (os == llvm::Triple::IOS) {
    target_triple.os = Cangjie::Triple::OSType::IOS;
    target_triple.env = Cangjie::Triple::Environment::SIMULATOR;
    target_triple.vendor = Cangjie::Triple::Vendor::APPLE;
    return;
  } else {
    target_triple.os = Cangjie::Triple::OSType::UNKNOWN;
  }
  auto env = target_arch.GetTriple().getEnvironment();
  if (env == llvm::Triple::GNU) {
    target_triple.env = Cangjie::Triple::Environment::GNU;
  } else if (env == llvm::Triple::Android) {
    target_triple.env = Cangjie::Triple::Environment::ANDROID;
  } else {
    target_triple.env = Cangjie::Triple::Environment::OHOS;
  }
  target_triple.vendor = Cangjie::Triple::Vendor::UNKNOWN;
  return;
}

bool CangjieExpressionParser::Parse(ExecutionContext &exeCtx, const std::string text) {
  Log *log = GetLog(LLDBLog::Expressions);
  std::string cangjieExprTempFile = "./expr.temp";
  std::unique_ptr<CompilerInvocation> invocation = std::make_unique<CompilerInvocation>();
  auto cangjie_home = getenv("CANGJIE_HOME");
  if (cangjie_home == nullptr) {
    LLDB_LOGF(log, "CANGJIE_HOME is not set!");
    return false;
  }
  auto cangjie_path = getenv("CANGJIE_PATH");
  if (cangjie_path != nullptr) {
    std::stringstream sstream(cangjie_path);
    std::string tmp_path;
    while (std::getline(sstream, tmp_path, ':')) {
      if (!tmp_path.empty()) {
        LLDB_LOGF(log, "add [%s] to env CANGJIE_PATH\n", tmp_path.c_str());
        invocation->globalOptions.environment.cangjiePaths.emplace_back(tmp_path);
      }
    }
  }
  Cangjie::Triple::Info target_triple;
  ArchSpec target_arch = exeCtx.GetTargetSP()->GetArchitecture();
  auto triple = target_arch.GetTriple();
  SetTripleInfoIfNeed(target_arch, target_triple);
  LLDB_LOGF(log, "using target triple %s %s %s %s %s\n", triple.str().c_str(),
            triple.getArchName().str().c_str(), triple.getVendorName().str().c_str(),
            triple.getOSName().str().c_str(), triple.getEnvironmentName().str().c_str());
  if (target_triple.env == Cangjie::Triple::Environment::ANDROID) {
    target_triple.apiLevel = "31";
  }
  invocation->globalOptions.target = target_triple;
  invocation->globalOptions.environment.cangjieHome = cangjie_home;
  invocation->globalOptions.srcFiles.push_back(cangjieExprTempFile);
  invocation->globalOptions.outputMode = GlobalOptions::OutputMode::STATIC_LIB;
  invocation->globalOptions.overflowStrategy = OverflowStrategy::WRAPPING;
  invocation->globalOptions.implicitPrelude = true;
  invocation->globalOptions.chirLLVM = true;
  invocation->globalOptions.aggressiveParallelCompile = 1;
  invocation->globalOptions.cjdbMode = true;

  DiagnosticEngine diag;
  diag.SetDisableWarning(true);
  std::unique_ptr<CangjieExpr::CangjieCompilerInstance> instance =
      std::make_unique<CangjieExpr::CangjieCompilerInstance>(*invocation, diag);
  instance->bufferCache.emplace(cangjieExprTempFile, text);
  instance->loadSrcFilesFromCache = true;
  instance->PerformParse();
  if (diag.GetErrorCount() > 0) {
    // There was a syntax parsing error in the user string.
    return false;
  }

  PrepareContextInfo(exeCtx, instance->GetSourceManager());

  // If user string is `expr a + b`, the refStrs is {a, b}.
  auto refStrs = instance->WalkAndCollectIdentifiers();

  // Create fake packages.
  ResetDeclMap(exeCtx, refStrs, m_file_id, m_current_filename);
  instance->m_fake_packages = m_expr_decl_map_up->CreateExternalAST();

  instance->PerformSuperExpression(m_in_class_context, m_in_constructor, m_in_static_method);
  if (diag.GetErrorCount() > 0) {
    return false;
  }
  if (!instance->PerformImportPackageForCjdb()) {
    return false;
  }
  instance->PerformSema();
  if (diag.GetErrorCount() > 0) {
    return false;
  }
  instance->PerformAfterSemaForCjdb();
  m_search_func_name = instance->IsUserLookupFunction();
  if (m_search_func_name) {
    return false;
  }
  instance->PerformDesugarAfterSema();
  instance->PerformGenericInstantiation();
  instance->PerformOverflowStrategy();
  instance->PerformMangling();
  instance->PerformCHIRCompilation();
  instance->PerformCodeGen();
  m_module = instance->ReleaseModule();
  if (m_module == nullptr) {
    LLDB_LOGF(log, "m_module is null");
    return false;
  }
  m_result_type_name = instance->m_result_type_name;
  std::string demangled_name = lldb_private::Mangled::GetDemangledTypeName(m_result_type_name);
  if (!m_expr_result_type.IsValid() && demangled_name.find("std.core::Option<") == 0) {
    m_expr_result_type = m_expr_decl_map_up->CreateOptionReturnType(instance->m_expr_result->GetTy(), demangled_name);
    return true;
  }
  if (!instance->m_expr_result->GetTy()->typeArgs.empty()) {
    std::string typeName = GetSubNameWithoutPkgname(demangled_name, m_expr_decl_map_up->m_current_pkgname);
    if (typeName.empty()) {
      typeName = demangled_name;
    }
    auto retType = m_expr_decl_map_up->LookUpType(typeName);
    if (retType.IsValid() && retType.GetTypeClass() != lldb::eTypeClassEnumeration) {
      m_expr_result_type = retType;
    } else {
      m_expr_result_type = m_expr_decl_map_up->GetDynamicTypeFromGenericTypeInfo(
          instance->m_expr_result->GetTy(), demangled_name);
    }
  }
  return true;
}

bool CangjieExpressionParser::RewriteExpression(
    DiagnosticManager &diagnostic_manager) {
  return true;
}

lldb_private::Status CangjieExpressionParser::PrepareForExecution(
    lldb::addr_t &func_addr, lldb::addr_t &func_end,
    lldb::IRExecutionUnitSP &execution_unit_sp, ExecutionContext &exe_ctx,
    bool &can_interpret, ExecutionPolicy execution_policy) {
  Log *log = GetLog(LLDBLog::Expressions);
  lldb_private::Status err;
  std::unique_ptr<llvm::LLVMContext> context_up(&m_module->getContext());
  lldb::TargetSP target_sp = exe_ctx.GetTargetSP();

  SymbolContext sym_ctx;
  if (lldb::StackFrameSP frame_sp = exe_ctx.GetFrameSP()) {
    sym_ctx = frame_sp->GetSymbolContext(lldb::eSymbolContextEverything);
  } else if (lldb::TargetSP target_sp = exe_ctx.GetTargetSP()) {
    sym_ctx.target_sp = target_sp;
  }

  lldb_private::ConstString function_name("_CN11__cjdb_expr11__lldb_exprHPu");
  std::vector<std::string> cpu_features;
  execution_unit_sp = std::make_shared<IRExecutionUnit>(
      context_up, m_module, function_name, target_sp, sym_ctx, cpu_features);

  if (log) {
    std::string s;
    llvm::raw_string_ostream oss(s);
    execution_unit_sp->GetModule()->print(oss, nullptr);
    oss.flush();
    LLDB_LOGF(log, "module in IRExecutionUnit: \n%s", s.c_str());
  }
  CangjieIRForTarget ir_for_execute(m_materializer, execution_unit_sp->GetModule(),
                                    function_name, m_expr_decl_map_up.get(), exe_ctx);
  std::string error_type_name = "Int64";
  ir_for_execute.m_expr_result_type = m_expr_result_type;
  ir_for_execute.CreateResultVariable(m_result_delegate, m_result_type_name,
                                      m_in_member_method, ConstString("__lldb_expr_result"));
  ir_for_execute.CreateResultVariable(m_error_result_delegate, error_type_name,
                                      m_in_member_method, ConstString("__lldb_error_result"));
  ir_for_execute.HandleIRForExecute();
  execution_unit_sp->GetRunnableInfo(err, func_addr, func_end);
  if (err.Fail()) {
    return err;
  }

  m_materialized_address = execution_unit_sp->Malloc(
      m_materializer->GetStructByteSize(),
      m_materializer->GetStructAlignment(),
      lldb::ePermissionsReadable | lldb::ePermissionsWritable,
      IRMemoryMap::eAllocationPolicyMirror, false, err);
  return err;
}

void CangjieExpressionParser::PrepareParse(
    lldb_private::Materializer *materializer,
    lldb_private::Materializer::PersistentVariableDelegate *result_delegate,
    lldb_private::Materializer::PersistentVariableDelegate *error_result_delegate,
    bool is_static_method) {
  m_materializer = materializer;
  m_result_delegate = result_delegate;
  m_error_result_delegate = error_result_delegate;
  m_in_static_method = is_static_method;
}
