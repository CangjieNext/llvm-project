//===-- CangjieCompilerInstance.h -----------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_CANGJIECOMPILERINSTANCE_H
#define LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_CANGJIECOMPILERINSTANCE_H

#include "CangjieExpressionParser.h"
#include "cangjie/FrontendTool/DefaultCompilerInstance.h"

namespace lldb_private {
namespace CangjieExpr {
using namespace Cangjie;

class CangjieCompilerInstance : public Cangjie::DefaultCompilerInstance {
public:
  CangjieCompilerInstance(Cangjie::CompilerInvocation &invocation,
                          Cangjie::DiagnosticEngine &diag)
      : DefaultCompilerInstance(invocation, diag) {}
  bool PerformCodeGen() override;
  std::unique_ptr<llvm::Module> ReleaseModule();

  void PerformSuperExpression(bool inClassContext, bool isConstructor, bool isStaticMethod);
  bool PerformImportPackageForCjdb();
  void PerformAfterSemaForCjdb();
  bool IsUserLookupFunction();
  std::vector<std::string> WalkAndCollectIdentifiers();
  std::string GetStringOfASTNode(const Ptr<Node>& node);
  std::vector<OwnedPtr<AST::Package>> m_fake_packages;
  std::string m_result_type_name;
  Ptr<AST::VarDecl> m_expr_result = nullptr;
private:
  std::unique_ptr<llvm::Module> m_module_up;
  Ptr<AST::TryExpr> m_tryExpr_result = nullptr;
  std::vector<std::string> m_package_names;
  std::vector<Ptr<AST::VarDecl>> m_captured_vars;

  void AddImportNodeFromFakePkg(OwnedPtr<Cangjie::AST::File>& file, OwnedPtr<AST::Package>& fPkg);
  void AddImportSpecToLLDBExprPackage();
  Ptr<AST::Ty> AddObjectToFunctionTy(Ptr<AST::Ty> fTy, Ptr<AST::Ty> objectTy);
  void AddCapturedvarsToCallExprOfLocalFunc();
  std::vector<OwnedPtr<FuncArg>> CreateFuncArgsFromCapturedVars(std::string ident);
  void AddCapturedVarsToTryExpr(std::vector<OwnedPtr<Decl>>& localDecls, AST::TryExpr& tryExpr);
  void AddLocalsToExprResult(std::vector<OwnedPtr<Decl>>& localDecls);
  void AddTyOfRefType(AST::RefType& rt);
  void CheckTypeAndAddTy(OwnedPtr<AST::Type>& type);
  Ptr<AST::Ty> GetInstantiatedTy(AST::ClassTy& cTy, Ptr<AST::Ty> paramTy);
  OwnedPtr<AST::CallExpr> CreateSuperCall(AST::ClassDecl& cd);
  void CreateInitFunc(AST::Decl& decl);
  void CreateDefaultCtor(AST::Decl& decl);
  void UpdateDeclTyByGeneric(Ptr<AST::Decl> decl);
  Ptr<Ty> GetTyFromASTType(Decl& decl, const std::vector<Ptr<Ty>>& typeArgs);
  void CreateFuncdeclTy(AST::FuncDecl& fd);
  void CreateTyAndDefaultCtor(AST::Decl& decl, const std::vector<Ptr<AST::Ty>>& typeArgs);

  Ptr<AST::FuncDecl> GetInitFuncFromClassOrStruct(
    std::vector<OwnedPtr<Decl>>& decls, std::vector<Ptr<Cangjie::AST::Ty>>& params);
  OwnedPtr<Cangjie::AST::Expr> CreatePrimitiveInitializer(Ptr<Cangjie::AST::Ty> ty);
  OwnedPtr<Cangjie::AST::Expr> CreateFunctionInitializer(Ptr<Cangjie::AST::Ty> ty);
  OwnedPtr<Cangjie::AST::Expr> CreateStructInitializer(Ptr<Cangjie::AST::Ty> ty);
  OwnedPtr<Cangjie::AST::Expr> CreateClassInitializer(Ptr<Cangjie::AST::Ty> ty);
  OwnedPtr<Cangjie::AST::Expr> CreateEnumInitializer(Ptr<Cangjie::AST::Ty> ty);
  OwnedPtr<Cangjie::AST::Expr> CreateRangeInitializer(Ptr<Cangjie::AST::Ty> ty);
  OwnedPtr<Cangjie::AST::Expr> CreateInitializer(Ptr<Cangjie::AST::Ty> ty);
};

} // namespace CangjieExpr
} // namespace lldb_private
#endif // LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_CANGJIECOMPILERINSTANCE_H
