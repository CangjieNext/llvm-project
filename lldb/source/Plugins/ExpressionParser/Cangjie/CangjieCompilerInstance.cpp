//===-- CangjieCompilerInstance.cpp ---------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#include "CangjieCompilerInstance.h"
#include "cangjie/Mangle/BaseMangler.h"

#include "cangjie/AST/PrintNode.h"
#include "cangjie/AST/Walker.h"
#include "cangjie/AST/Create.h"
#include "cangjie/Utils/SafePointer.h"
#include "cangjie/Modules/ImportManager.h"
#include "cangjie/Sema/TypeManager.h"

#include "cangjie/CodeGen/EmitPackageIR.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/LLDBLog.h"

using namespace Cangjie;
using namespace AST;
using namespace lldb_private;
using namespace lldb_private::CangjieExpr;

bool CangjieCompilerInstance::PerformCodeGen() {
  CJC_NULLPTR_CHECK(chirData);
  if (chirData->GetCurrentCHIRPackage() == nullptr) {
    return false;
  }
  auto llvmModules = Cangjie::CodeGen::GenPackageModules(*this, false);
  CJC_ASSERT(llvmModules.size() == 1);
  m_module_up = std::move(llvmModules[0]);
  return true;
}

std::unique_ptr<llvm::Module> CangjieCompilerInstance::ReleaseModule() {
  return std::move(m_module_up);
}

void CangjieCompilerInstance::AddTyOfRefType(AST::RefType& rt)
{
  Ptr<AST::Decl> decl = nullptr;
  for (auto& pkg : m_package_names) {
    // If the identifier begins with the package name.
    auto curpkgPre = pkg + PACKAGE_SUFFIX;
    if (rt.ref.identifier.Val().find(curpkgPre) == 0) {
      // Delete prefix "std.core::"/"std.collection::"
      rt.ref.identifier = rt.ref.identifier.Val().substr(curpkgPre.size());
      decl = importManager.GetImportedDecl(pkg, rt.ref.identifier);
      break;
    }
  }
  if (!decl) {
    // Check if it is in core package.
    decl = importManager.GetCoreDecl(rt.ref.identifier);
    if (!decl) {
      // Check if it is in collection package.
      decl = importManager.GetImportedDecl(COLLECTION_PACKAGE_NAME, rt.ref.identifier);
    }
  }
  std::vector<Ptr<Ty>> typeArgs;
  for (auto& arg : rt.typeArguments) {
    CheckTypeAndAddTy(arg);
    typeArgs.push_back(arg->ty);
  }
  if (!decl) {
    decl = rt.ref.target;
  }
  if (decl) {
    rt.ty = GetTyFromASTType(*decl, typeArgs);
    return;
  }
  return;
}

void CangjieCompilerInstance::CheckTypeAndAddTy(OwnedPtr<AST::Type>& type)
{
  if (!type) {
    return;
  }
  bool isGenericType = false;
  if (type->astKind == AST::ASTKind::REF_TYPE) {
    auto rt = RawStaticCast<AST::RefType *>(type.get());
    if (rt->ref.target && rt->ref.target->astKind == AST::ASTKind::GENERIC_PARAM_DECL) {
      isGenericType = true;
    }
  }
  if (AST::Ty::IsTyCorrect(type->ty) && !isGenericType) {
    return;
  }
  if (type->astKind == AST::ASTKind::PRIMITIVE_TYPE) {
    auto pt = RawStaticCast<AST::PrimitiveType *>(type.get());
    pt->ty = typeManager->GetPrimitiveTy(pt->kind);
  } else if (type->astKind == AST::ASTKind::REF_TYPE) {
    auto rt = RawStaticCast<AST::RefType *>(type.get());
    AddTyOfRefType(*rt);
  } else if (type->astKind == AST::ASTKind::TUPLE_TYPE) {
    auto tt = RawStaticCast<AST::TupleType *>(type.get());
    std::vector<Ptr<Ty>> subTys;
    for (auto& subType : tt->fieldTypes) {
      CheckTypeAndAddTy(subType);
      subTys.push_back(subType->ty);
    }
    tt->ty = typeManager->GetTupleTy(subTys);
  } else if (type->astKind == AST::ASTKind::VARRAY_TYPE) {
    auto vaType = RawStaticCast<AST::VArrayType *>(type.get());
    auto constType = RawStaticCast<AST::ConstantType *>(vaType->constantType.get());
    auto le = RawStaticCast<AST::LitConstExpr *>(constType->constantExpr.get());
    auto vaSize = stoll(le->stringValue);
    CheckTypeAndAddTy(vaType->typeArgument);
    if (vaType->typeArgument) {
      vaType->ty = typeManager->GetVArrayTy(*vaType->typeArgument->ty, vaSize);
    }
  } else if (type->astKind == AST::ASTKind::FUNC_TYPE) {
    auto func_type = RawStaticCast<AST::FuncType *>(type.get());
    std::vector<Ptr<Cangjie::AST::Ty>> params;
    for (auto &param : func_type->paramTypes) {
      CheckTypeAndAddTy(param);
      params.emplace_back(param->ty);
    }
    CheckTypeAndAddTy(func_type->retType);
    if (func_type->retType) {
      type->ty = typeManager->GetFunctionTy(params, func_type->retType->ty);
    }
  }
}

Ptr<AST::Ty> CangjieCompilerInstance::GetInstantiatedTy(AST::ClassTy& cTy, Ptr<AST::Ty>& paramTy) {
  if (!paramTy->IsGeneric()) {
    return paramTy;
  }
  auto& typeParams = cTy.decl->generic->typeParameters;
  CJC_ASSERT(typeParams.size() == cTy.typeArgs.size());
  for (size_t i = 0; i < typeParams.size(); i++) {
    if (typeParams[i]->identifier.Val() == paramTy->name) {
      return cTy.typeArgs[i];
    }
  }
  return paramTy;
}

OwnedPtr<AST::CallExpr> CangjieCompilerInstance::CreateSuperCall(AST::ClassDecl& cd)
{
  auto superType = cd.inheritedTypes.front().get();
  if (!superType) {
    return nullptr;
  }
  auto cTy = StaticCast<AST::ClassTy*>(superType->ty);
  auto superExpr = CreateRefExpr("super");
  superExpr->isSuper = true;
  std::vector<OwnedPtr<FuncArg>> args;
  for (auto& decl : cTy->decl->body->decls) {
    if (auto fd = DynamicCast<AST::FuncDecl*>(decl.get()); fd && fd->TestAttr(Attribute::CONSTRUCTOR)) {
      superExpr->ref.target = fd;
      superExpr->ty = fd->ty;
      for (auto& param : fd->funcBody->paramLists[0]->params) {
        if (!param || !param->ty || param->ty->IsInvalid()) {
          continue;
        }
        // Parent's init func may have generic param.
        auto argTy = GetInstantiatedTy(*cTy, param->ty);
        auto initializer = CreateInitializer(argTy);
        if (!initializer) {
          return nullptr;
        }
        args.emplace_back(CreateFuncArg(std::move(initializer)));
      }
      break;
    }
  }
  auto superCall = CreateCallExpr(std::move(superExpr), std::move(args));
  superCall->callKind = CallKind::CALL_SUPER_FUNCTION;
  superCall->ty = superType->ty;
  for (auto& decl : cTy->decl->body->decls) {
    if (auto fd = DynamicCast<FuncDecl*>(decl.get()); fd && fd->TestAttr(Attribute::CONSTRUCTOR)) {
      superCall->resolvedFunction = fd;
    }
  }
  return superCall;
}

void CangjieCompilerInstance::CreateInitFunc(AST::Decl& decl)
{
  auto funcTy = typeManager->GetFunctionTy({}, decl.ty);
  auto funcBody = MakeOwned<Cangjie::AST::FuncBody>();
  auto funcParamList = MakeOwned<Cangjie::AST::FuncParamList>();
  funcBody->paramLists.push_back(std::move(funcParamList));
  funcBody->body = MakeOwned<Cangjie::AST::Block>();
  funcBody->ty = funcTy;

  auto initFunc = MakeOwned<Cangjie::AST::FuncDecl>();
  initFunc->ty = funcTy;
  initFunc->funcBody = std::move(funcBody);
  initFunc->funcBody->funcDecl = initFunc.get();
  initFunc->identifier = "init";
  initFunc->outerDecl = &decl;
  initFunc->fullPackageName = decl.fullPackageName;
  initFunc->EnableAttr(Attribute::CONSTRUCTOR, Attribute::PUBLIC, Attribute::COMPILER_ADD);
  if (decl.astKind == ASTKind::STRUCT_DECL) {
    initFunc->EnableAttr(Attribute::IN_STRUCT);
    auto structDecl = static_cast<StructDecl *>(&decl);
    if (!structDecl->body) {
      structDecl->body = MakeOwned<StructBody>();
    }
    initFunc->funcBody->parentStruct = structDecl;
    structDecl->body->decls.emplace_back(std::move(initFunc));
    return;
  }
  if (decl.astKind == ASTKind::CLASS_DECL) {
    initFunc->constructorCall = ConstructorCall::SUPER;
    initFunc->EnableAttr(Attribute::IN_CLASSLIKE);
    auto classDecl = static_cast<ClassDecl *>(&decl);
    // Add Object Super Class
    auto refType = MakeOwned<RefType>();
    refType->curFile = classDecl->curFile;
    refType->ref.identifier = OBJECT_NAME;
    refType->EnableAttr(Attribute::COMPILER_ADD);
    refType->ref.target = importManager.GetCoreDecl(OBJECT_NAME);
    if (refType->ref.target) {
      refType->ty = refType->ref.target->ty;
    }
    if (classDecl->inheritedTypes.size() == 0) {
      classDecl->inheritedTypes.emplace_back(std::move(refType));
    }
    if (!classDecl->body) {
      classDecl->body = MakeOwned<ClassBody>();
    }
    initFunc->funcBody->parentClassLike = classDecl;
    // Add super call `super()`
    auto superCall = CreateSuperCall(*classDecl);
    initFunc->funcBody->body->ty = superCall->ty;
    initFunc->funcBody->body->body.push_back(std::move(superCall));
    classDecl->body->decls.emplace_back(std::move(initFunc));
  }
  return;
}

void CangjieCompilerInstance::UpdateDeclTyByGeneric(Ptr<AST::Decl> decl) {
  std::vector<Ptr<Ty>> typeArgs;
  auto generic = decl->GetGeneric();
  if (!generic) {
    return;
  }
  for (auto& it: generic->typeParameters) {
    auto gpd = RawStaticCast<AST::GenericParamDecl *>(it.get());
    CreateTyAndDefaultCtor(*gpd, {});
    typeArgs.push_back(gpd->ty);
  }
  for (auto& it: generic->genericConstraints) {
    auto gc = RawStaticCast<AST::GenericConstraint *>(it.get());
    if (gc->type->ref.target) {
      CreateTyAndDefaultCtor(*gc->type->ref.target, {});
      gc->type->ty = GetTyFromASTType(*gc->type->ref.target, {});
    }
    for (auto& bound:gc->upperBounds) {
      auto rt = RawStaticCast<AST::RefType *>(bound.get());
      if (rt->ref.target) {
        CreateTyAndDefaultCtor(*rt->ref.target, {});
        rt->ty = GetTyFromASTType(*rt->ref.target, {});
      }
    }
  }
  decl->ty = GetTyFromASTType(*decl, typeArgs);
}

Ptr<Ty> CangjieCompilerInstance::GetTyFromASTType(Decl& decl, const std::vector<Ptr<Ty>>& typeArgs)
{
  switch (decl.astKind) {
    case ASTKind::CLASS_DECL: {
      auto cd = As<ASTKind::CLASS_DECL>(&decl);
      for (auto& it : cd->inheritedTypes) {
        CheckTypeAndAddTy(it);
      }
      return typeManager->GetClassTy(*cd, typeArgs);
    }
    case ASTKind::INTERFACE_DECL: {
      auto id = As<ASTKind::INTERFACE_DECL>(&decl);
      for (auto& it : id->inheritedTypes) {
        CheckTypeAndAddTy(it);
      }
      return typeManager->GetInterfaceTy(*id, typeArgs);
    }
    case ASTKind::STRUCT_DECL: {
      auto sd = As<ASTKind::STRUCT_DECL>(&decl);
      for (auto& it : sd->inheritedTypes) {
        CheckTypeAndAddTy(it);
      }      
      return typeManager->GetStructTy(*sd, typeArgs);
    }
    case ASTKind::ENUM_DECL: {
      auto ed = As<ASTKind::ENUM_DECL>(&decl);
      return typeManager->GetEnumTy(*ed, typeArgs);
    }
    case ASTKind::TYPE_ALIAS_DECL: {
      auto tad = As<ASTKind::TYPE_ALIAS_DECL>(&decl);
      return typeManager->GetTypeAliasTy(*tad, typeArgs);
    }
    case ASTKind::GENERIC_PARAM_DECL: {
      auto gpd = As<ASTKind::GENERIC_PARAM_DECL>(&decl);
      return typeManager->GetGenericsTy(*gpd);
    }
    default:
      return decl.ty;
  }
}

void CangjieCompilerInstance::CreateDefaultCtor(AST::Decl& decl)
{
  if (decl.astKind == ASTKind::CLASS_DECL) {
    auto cd = StaticCast<ClassDecl *>(&decl);
    for (auto& it : cd->inheritedTypes) {
      CheckTypeAndAddTy(it);
    }
    bool hasConstructor = false;
    for (auto& d : cd->body->decls) {
      if (auto fd = DynamicCast<FuncDecl*>(d.get()); fd && fd->TestAttr(Attribute::CONSTRUCTOR)) {
        hasConstructor = true;
        fd->EnableAttr(Attribute::IN_CLASSLIKE, Attribute::COMPILER_ADD);
        fd->funcBody->parentClassLike = cd;
        fd->funcBody->funcDecl = fd;
        if (fd->funcBody->retType) {
          fd->funcBody->retType->ty = decl.ty;
        }
        // Add super call `super()`
        auto superCall = CreateSuperCall(*cd);
        if (superCall) {
          fd->funcBody->body = MakeOwned<Cangjie::AST::Block>();
          fd->funcBody->body->ty = superCall->ty;
          fd->funcBody->body->body.push_back(std::move(superCall));
          fd->constructorCall = ConstructorCall::SUPER;
        }
      }
    }
    if (!hasConstructor) {
      CreateInitFunc(*cd);
    }
  } else if (decl.astKind == ASTKind::STRUCT_DECL) {
    auto sd = StaticCast<StructDecl *>(&decl);
    bool hasConstructor = false;
    for (auto& d : sd->body->decls) {
      if (auto fd = DynamicCast<FuncDecl*>(d.get()); fd && fd->TestAttr(Attribute::CONSTRUCTOR)) {
        hasConstructor = true;
        fd->EnableAttr(Attribute::IN_STRUCT, Attribute::COMPILER_ADD);
        fd->funcBody->parentStruct = sd;
        fd->funcBody->funcDecl = fd;
        if (fd->funcBody->retType) {
          fd->funcBody->retType->ty = decl.ty;
        }
      }
    }
    if (!hasConstructor) {
      CreateInitFunc(*sd);
    }
  }
  return;
}

void CangjieCompilerInstance::CreateFuncdeclTy(AST::FuncDecl& fd)
{
  CheckTypeAndAddTy(fd.funcBody->retType);
  if (!fd.funcBody->retType) {
    return;
  }
  auto retTy = fd.funcBody->retType->ty;
  std::vector<Ptr<Cangjie::AST::Ty>> params;
  for (auto& param : fd.funcBody->paramLists[0]->params) {
    if (!param->type) {
      continue;
    }
    CheckTypeAndAddTy(param->type);
    param->ty = param->type->ty;
    params.emplace_back(param->ty);
  }
  UpdateDeclTyByGeneric(&fd);
  fd.ty = typeManager->GetFunctionTy(params, retTy);
  fd.funcBody->ty = fd.ty;
}

void CangjieCompilerInstance::CreateTyAndDefaultCtor(
    AST::Decl& decl, const std::vector<Ptr<AST::Ty>>& typeArgs)
{
  switch (decl.astKind) {
    case ASTKind::CLASS_DECL: {
      auto cd = As<ASTKind::CLASS_DECL>(&decl);
      for (auto& it : cd->inheritedTypes) {
        if (it->astKind == AST::ASTKind::REF_TYPE && it->GetTypeArgs().empty()) {
          continue;
        }
        auto rt = RawStaticCast<AST::RefType *>(it.get());
        if (rt->ref.target) {
          CreateTyAndDefaultCtor(*rt->ref.target, {});
        }
      }
      decl.ty = typeManager->GetClassTy(*cd, typeArgs);
      UpdateDeclTyByGeneric(&decl);
      CreateDefaultCtor(decl);
      break;
    }
    case ASTKind::INTERFACE_DECL: {
      auto id = As<ASTKind::INTERFACE_DECL>(&decl);
      decl.ty = typeManager->GetInterfaceTy(*id, typeArgs);
      UpdateDeclTyByGeneric(&decl);
      break;
    }
    case ASTKind::STRUCT_DECL: {
      auto sd = As<ASTKind::STRUCT_DECL>(&decl);
      for (auto& it : sd->inheritedTypes) {
        if (it->astKind == AST::ASTKind::REF_TYPE && it->GetTypeArgs().empty()) {
          continue;
        }
        auto rt = RawStaticCast<AST::RefType *>(it.get());
        if (rt->ref.target) {
          CreateTyAndDefaultCtor(*rt->ref.target, {});
        }
      }      
      decl.ty = typeManager->GetStructTy(*sd, typeArgs);
      UpdateDeclTyByGeneric(&decl);
      CreateDefaultCtor(decl);
      break;
    }
    case ASTKind::ENUM_DECL: {
      auto ed = As<ASTKind::ENUM_DECL>(&decl);
      decl.ty = typeManager->GetEnumTy(*ed, typeArgs);
      UpdateDeclTyByGeneric(&decl);
      for (size_t i = 0; i < ed->constructors.size(); i++) {
        auto tempDecl = RawStaticCast<AST::Decl *>(ed->constructors[i].get());
        tempDecl->ty = decl.ty;
      }
      break;
    }
    case ASTKind::TYPE_ALIAS_DECL: {
      auto tad = As<ASTKind::TYPE_ALIAS_DECL>(&decl);
      decl.ty = typeManager->GetTypeAliasTy(*tad, typeArgs);
      break;
    }
    case ASTKind::GENERIC_PARAM_DECL: {
      auto gpd = As<ASTKind::GENERIC_PARAM_DECL>(&decl);
      decl.ty = typeManager->GetGenericsTy(*gpd);
      break;
    }
    case ASTKind::FUNC_DECL: {
      auto fd = As<ASTKind::FUNC_DECL>(&decl);
      CreateFuncdeclTy(*fd);
      break;
    }
    case ASTKind::VAR_DECL: {
      auto vd = As<ASTKind::VAR_DECL>(&decl);
      UpdateDeclTyByGeneric(&decl);
      if (vd->type) {
        CheckTypeAndAddTy(vd->type);
        decl.ty = vd->type->ty;
      }
      break;
    }
    default:
      break;
  }
}

std::string CangjieCompilerInstance::GetStringOfASTNode(const Ptr<Node>& node) {
  std::stringstream buffer;
  std::streambuf* oldbuf = std::cout.rdbuf(buffer.rdbuf());
  PrintNode(node);
  std::cout.rdbuf(oldbuf);
  std::string astNodeString = buffer.str();
  return astNodeString;
}

std::vector<OwnedPtr<FuncArg>> CangjieCompilerInstance::CreateFuncArgsFromCapturedVars(std::string ident)
{
  std::vector<OwnedPtr<FuncArg>> funcargs;
  for (auto& decl: m_tryExpr_result->curFile->decls) {
    if (decl->astKind == AST::ASTKind::CLASS_DECL && decl->identifier == ident) {
      auto cd = As<AST::ASTKind::CLASS_DECL>(decl.get());
      for (auto& d : cd->body->decls) {
        if (d->astKind == AST::ASTKind::VAR_DECL &&
            d->identifier.Val().find("$Captured") == std::string::npos) {
          funcargs.emplace_back(CreateFuncArg(CreateRefExpr(d->identifier)));
        }
      }
    }
  }
  return funcargs;
}

bool HasSameCapturedVar(std::vector<Ptr<AST::VarDecl>>& capturedVars, Ptr<AST::VarDecl> vd) {
  for (auto v : capturedVars) {
    if (v->identifier == vd->identifier) {
      return true;
    }
  }
  return false;
}

void CangjieCompilerInstance::AddCapturedVarsToTryExpr(
    std::vector<OwnedPtr<Decl>>& localDecls, AST::TryExpr& tryExpr) {
    auto hasCapturedVarsClass = [this](std::string fn) -> bool {
      auto classname = fn + "$class" + LOCAL_FUNC_CAPTUREDVAR;
      for (auto& decl: m_tryExpr_result->curFile->decls) {
        if (decl->astKind == AST::ASTKind::CLASS_DECL &&
          decl->identifier == classname) {
          return true;
        }
      }
      return false;
    };
    for (auto& decl: localDecls) {
      if (decl->astKind != AST::ASTKind::FUNC_DECL) {
        continue;
      }
      if (!hasCapturedVarsClass(decl->identifier)) {
        // If the local function does not have $CapturedVars, add `var foo$Object = Object()`.
        auto vd = MakeOwned<AST::VarDecl>();
        vd->identifier = decl->identifier + "$Object";
        auto re = MakeOwned<RefExpr>();
        re->ref.identifier = "Object";
        re->ref.identifier.SetPos(re->begin, re->begin + re->ref.identifier.Val().size());
        vd->initializer = CreateCallExpr(std::move(re), {});
        AddCurFile(*vd, m_tryExpr_result->curFile);
        if (!HasSameCapturedVar(m_captured_vars, vd.get())) {
          m_captured_vars.emplace_back(vd.get());
          tryExpr.tryBlock->body.emplace(tryExpr.tryBlock->body.begin(), std::move(vd));
        }
        continue;
      }
      /*
        If the local function has $CapturedVars, add the following statements.
        var foo$CaptureVars = foo$class$CapturedVars(aa)
        var foo$Object = (foo$CaptureVars as Object).getOrThrow()
      */
      // Add `var foo$CapturedVars = foo$class$CapturedVars()`.
      auto vd1 = MakeOwned<AST::VarDecl>();
      vd1->identifier = decl->identifier + LOCAL_FUNC_CAPTUREDVAR;
      auto re1 = MakeOwned<RefExpr>();
      re1->ref.identifier = decl->identifier + "$class" + LOCAL_FUNC_CAPTUREDVAR;
      re1->ref.identifier.SetPos(re1->begin, re1->begin + re1->ref.identifier.Val().size());
      auto funcArgs = CreateFuncArgsFromCapturedVars(re1->ref.identifier);
      vd1->initializer = CreateCallExpr(std::move(re1), std::move(funcArgs));
      AddCurFile(*vd1, m_tryExpr_result->curFile);

      // Add `var foo$Object = (foo$CaptureVars as Object).getOrThrow()`.
      auto vd2 = MakeOwned<AST::VarDecl>();
      vd2->identifier = decl->identifier + "$Object";
      auto re2 = MakeOwned<RefExpr>();
      re2->ref.identifier = vd1->identifier;
      re2->ref.identifier.SetPos(re2->begin, re2->begin + re2->ref.identifier.Val().size());
      auto asExpr = MakeOwned<AsExpr>();
      asExpr->leftExpr = std::move(re2);
      asExpr->asType = CreateRefTypeInCore("Object");
      auto ma = CreateMemberAccess(std::move(asExpr), "getOrThrow");
      vd2->initializer = CreateCallExpr(std::move(ma), {});
      AddCurFile(*vd2, m_tryExpr_result->curFile);
      if (!HasSameCapturedVar(m_captured_vars, vd2.get())) {
        m_captured_vars.emplace_back(vd2.get());
        tryExpr.tryBlock->body.emplace(tryExpr.tryBlock->body.begin(), std::move(vd2));
      }

      tryExpr.tryBlock->body.emplace(tryExpr.tryBlock->body.begin(), std::move(vd1));
    }
}

void CangjieCompilerInstance::AddLocalsToExprResult(std::vector<OwnedPtr<Decl>>& localDecls)
{
  auto addLocals = [this, &localDecls](Ptr<AST::Node> curNode) -> AST::VisitAction {
    if (curNode->astKind != AST::ASTKind::TRY_EXPR) {
      return AST::VisitAction::WALK_CHILDREN;
    }
    auto tryExpr = RawStaticCast<AST::TryExpr *>(curNode.get());
    // Add capturedVars of local function first.
    AddCapturedVarsToTryExpr(localDecls, *tryExpr);
    // Add local variable then.
    for (auto& decl: localDecls) {
      if (decl->astKind != AST::ASTKind::VAR_DECL) {
        continue;
      }
      auto var = RawStaticCast<AST::VarDecl *>(decl.get());
      // Add `var a = __lldb_locals_a` at the begin of the tryblock.
      auto varDecl = MakeOwned<AST::VarDecl>();
      varDecl->isVar = var->isVar;
      varDecl->identifier = var->identifier;
      varDecl->initializer = CreateRefExpr(LOCAL_PACKAGE_NAME + "_" + var->identifier);
      AddCurFile(*varDecl, m_tryExpr_result->curFile);
      tryExpr->tryBlock->body.emplace(tryExpr->tryBlock->body.begin(), std::move(varDecl));
      if (var->isVar && !var->ty->IsClass() && !var->ty->IsFunc()) {
        // Add `__lldb_locals_a = a` to the end of the tryBlock.
        auto leftExpr = CreateRefExpr(LOCAL_PACKAGE_NAME + "_" + var->identifier);
        auto assignExpr = CreateAssignExpr(std::move(leftExpr), CreateRefExpr(var->identifier));
        AddCurFile(*assignExpr, m_tryExpr_result->curFile);
        tryExpr->tryBlock->body.emplace_back(std::move(assignExpr));
      }
    }
    return AST::VisitAction::SKIP_CHILDREN;
  };

  AST::Walker walker(m_tryExpr_result, addLocals);
  walker.Walk();
}

void CangjieCompilerInstance::PerformSuperExpression(bool inClassContext, bool isConstructor, bool isStaticMethod)
{
  if (!inClassContext && !isStaticMethod) {
    return;
  }
  // If in the context of a class, modify super to __lldb_injected_super.
  // If in a static method of a class, the diagnostic reports an error.
  auto modifySuper = [this, &isConstructor, &isStaticMethod](Ptr<AST::Node> curNode) -> AST::VisitAction {
    if (curNode->astKind == AST::ASTKind::CALL_EXPR) {
      auto ce = RawStaticCast<CallExpr *>(curNode.get());
      if (ce->baseFunc->astKind == AST::ASTKind::REF_EXPR) {
        auto re = RawStaticCast<RefExpr *>(ce->baseFunc.get());
        if (isStaticMethod && (re->isSuper || re->isThis)) {
          diag.Diagnose(*re, DiagKind::sema_static_members_cannot_call_members, re->ref.identifier.Val());
          return AST::VisitAction::SKIP_CHILDREN;
        }
        if (!isConstructor && re->isSuper) {
          diag.Diagnose(*ce, DiagKind::sema_invalid_this_call_outside_ctor, re->ref.identifier.Val());
          return AST::VisitAction::SKIP_CHILDREN;
        }
        if (re->isThis) {
          re->ref.identifier = INJECTED_THIS_NAME;
          re->isThis = false;
          auto ma = CreateMemberAccess(std::move(ce->baseFunc), "init");
          ce->baseFunc = std::move(ma);
          return AST::VisitAction::SKIP_CHILDREN;
        }
        if (re->isSuper) {
          re->ref.identifier = INJECTED_SUPER_NAME;
          re->isSuper = false;
          auto ma = CreateMemberAccess(std::move(ce->baseFunc), "init");
          ce->baseFunc = std::move(ma);
          return AST::VisitAction::SKIP_CHILDREN;
        }
      }
    }
    if (curNode->astKind != AST::ASTKind::REF_EXPR) {
      return AST::VisitAction::WALK_CHILDREN;
    }
    auto re = RawStaticCast<RefExpr *>(curNode.get());
    if (isStaticMethod && (re->isSuper || re->isThis)) {
      diag.Diagnose(*re, DiagKind::sema_static_members_cannot_call_members, re->ref.identifier.Val());
      return AST::VisitAction::SKIP_CHILDREN;
    }
    if (re->isSuper) {
      re->ref.identifier = INJECTED_SUPER_NAME;
      re->isSuper = false;
    }
    return AST::VisitAction::SKIP_CHILDREN;
  };

  AST::Walker walker(m_expr_result, modifySuper);
  walker.Walk();
  return;
}

bool checkUnsupportedNode(AST::Node& node) {
  Log *log = GetLog(LLDBLog::Expressions);
  if (node.astKind == ASTKind::REF_TYPE) {
    auto decl = Ty::GetDeclOfTy(node.ty);
    if (decl && decl->fullPackageName == "std.core" && decl->identifier == "Future") {
      if (log) {
        LLDB_LOGF(log, "%s is not supported in expression currently.\n", decl->identifier.Val().c_str());
      }
      return true;
    }
  }
  return false;
}

void DeletePackagePrefix(Ptr<AST::Decl> decl)
{
    auto deletepkg = [](Ptr<AST::Node> curNode) -> AST::VisitAction {
    if (curNode->astKind == AST::ASTKind::REF_TYPE) {
        auto rt = RawStaticCast<RefType *>(curNode.get());
        auto pos = rt->ref.identifier.Val().find(PACKAGE_SUFFIX);
        if (pos != std::string::npos) {
          rt->ref.identifier = rt->ref.identifier.Val().substr(pos + PACKAGE_SUFFIX.size());
        }
        return AST::VisitAction::SKIP_CHILDREN;
    }
    return AST::VisitAction::WALK_CHILDREN;
  };

  AST::Walker walker(decl, deletepkg);
  walker.Walk();
}

bool CangjieCompilerInstance::PerformImportPackageForCjdb() {
  std::vector<Ptr<Cangjie::AST::Package>> packages;
  // Collect rawptr of fake package.
  for (auto& pkg : this->m_fake_packages) {
    if (pkg->fullPackageName == LOCAL_FUNC_CAPTUREDVAR) {
      // Move local capturedvar class to Expr package.
      for (auto& decl : pkg->files[0]->decls) {
        DeletePackagePrefix(decl.get());
        m_tryExpr_result->curFile->decls.emplace_back(std::move(decl));
      }
      pkg->files[0]->decls.clear();
      continue;
    }
    packages.emplace_back(pkg.get());
  }

  // Add import to __lldb_expr_pkg.
  AddImportSpecToLLDBExprPackage();
  // Set fake package to importManager.
  importManager.SetImportedPackageFromASTNode(m_fake_packages);

  PerformImportPackage();
  Log *log = GetLog(LLDBLog::Expressions);
  if (diag.GetErrorCount() > 0) {
    std::string helpInfo = "check if the .cjo file of the package exists in CANGJIE_PATH or CANGJIE_HOME";
    if (log) {
      LLDB_LOGF(log, "%s", helpInfo.c_str());
    }
    std::cout<<"help: "<<helpInfo<<std::endl;
    return false;
  }
  bool inBlacklist = false;
  // Walk decls in packages, and set decl's ty especially for those from std.core.
  auto addDeclTy = [&](Ptr<AST::Node> curNode) -> VisitAction {
    if (curNode->IsDecl()) {
      auto decl = RawStaticCast<AST::Decl *>(curNode.get());
      CreateTyAndDefaultCtor(*decl, {});
      decl->mangledName = BaseMangler().Mangle(*decl);
    }
    if (checkUnsupportedNode(*curNode)) {
      inBlacklist = true;
    }
    return VisitAction::WALK_CHILDREN;
  };

  for (auto& fpkg : packages) {
    AST::Walker astWalker(fpkg.get(), addDeclTy);
    astWalker.Walk();
    if (fpkg->fullPackageName == LOCAL_PACKAGE_NAME) {
      // Add local variable: var a = __lldb_locals_a
      AddLocalsToExprResult(fpkg->files[0]->decls);
    }
    // Expression in the blacklist, which is not supported currently.
    if (inBlacklist) {
      return false;
    }
  }
  if (log) {
    std::string fakePkg;
    for (auto& fpkg : packages) {
      fakePkg = fakePkg + GetStringOfASTNode(fpkg);
    }
    LLDB_LOGF(log, "-------- Package Node after Importpackage -------- \n%s", fakePkg.c_str());
    std::string str = GetStringOfASTNode(this->GetSourcePackages()[0]);
    LLDB_LOGF(log, "-------- Package Node before sema -------- \n%s", str.c_str());
  }
  return true;
}

// Collect identifiers in user string after parsing and walking the AST.
std::vector<std::string> CangjieCompilerInstance::WalkAndCollectIdentifiers()
{
  auto exprPkg = this->GetSourcePackages()[0];
  auto findExprResult = [this](Ptr<AST::Node> curNode) -> VisitAction {
    if (curNode->astKind != AST::ASTKind::TRY_EXPR) {
      return AST::VisitAction::WALK_CHILDREN;
    }
    auto tryExpr = RawStaticCast<AST::TryExpr *>(curNode.get());
    for (auto& node : tryExpr->tryBlock->body) {
      if (node->astKind != AST::ASTKind::VAR_DECL) {
        continue;
      }
      auto varDecl = RawStaticCast<AST::VarDecl *>(node.get());
      if (varDecl->identifier == EXPR_RESULT_NAME) {
        m_tryExpr_result = tryExpr;
        m_expr_result = varDecl;
        return VisitAction::SKIP_CHILDREN;
      }
    }
    return VisitAction::WALK_CHILDREN;
  };
  AST::Walker lldbFindWalker(exprPkg, findExprResult);
  lldbFindWalker.Walk();
  std::vector<std::string> names;
  auto collectIdentifier = [&names](Ptr<const Node> curNode) -> VisitAction {
    if (curNode->astKind == AST::ASTKind::REF_TYPE) {
      auto re = RawStaticCast<const RefType *>(curNode.get());
      auto find = std::find_if(names.begin(), names.end(),
          [&re](auto& name) { return name == re->ref.identifier; });
      if (find == names.end()) {
        names.emplace_back(re->ref.identifier);
      }
      return VisitAction::SKIP_CHILDREN;
    }
    if (curNode->astKind == AST::ASTKind::REF_EXPR) {
      auto re = RawStaticCast<const RefExpr *>(curNode.get());
      if (re->ref.identifier == "this") {
        return VisitAction::SKIP_CHILDREN;
      }
      auto find = std::find_if(names.begin(), names.end(),
          [&re](auto& name) { return name == re->ref.identifier; });
      if (find == names.end()) {
        names.emplace_back(re->ref.identifier);
      }
      return VisitAction::WALK_CHILDREN;
    }
    return VisitAction::WALK_CHILDREN;
  };
  AST::Walker lldbWalker(m_expr_result, collectIdentifier);
  lldbWalker.Walk();
  Log *log = GetLog(LLDBLog::Expressions);
  if (log) {
    std::string ids = "";
    for (auto tmpId:names) {
      ids = ids + " ; " + tmpId;
    }
    LLDB_LOGF(log, "All collected identifiers are as follows: %s! \n", ids.c_str());
  }
  return names;
}

bool HasImportSameIdentifier(OwnedPtr<Cangjie::AST::File>& file,
  const std::vector<std::string>& pkgPath, const std::string& identifier) {
  for (auto& im : file->imports) {
    if (im->content.prefixPaths == pkgPath && im->content.identifier == identifier) {
      return true;
    }
  }
  return false;
}

bool HasImportSamePkg(OwnedPtr<Cangjie::AST::File>& file, const std::vector<std::string>& pkgPath) {
  for (auto& im : file->imports) {
    if (im->content.prefixPaths == pkgPath && im->content.kind == ImportKind::IMPORT_ALL) {
      return true;
    }
  }
  return false;
}

void CangjieCompilerInstance::AddImportNodeFromFakePkg(
    OwnedPtr<Cangjie::AST::File>& file, OwnedPtr<AST::Package>& fPkg)
{
  auto& fakeFile = fPkg->files[0];
  m_package_names.emplace_back(fPkg->fullPackageName);
  for (auto& im : fakeFile->imports) {
    if (HasImportSamePkg(file, im->content.prefixPaths)) {
      continue;
    }
    auto impkg = Utils::JoinStrings(im->content.prefixPaths, ".");
    if (im->content.hasDoubleColon) {
      impkg.replace(impkg.find("."), 1, PACKAGE_SUFFIX);
    }
    auto find = std::find_if(m_package_names.begin(), m_package_names.end(),
        [&impkg](auto& pkg) { return pkg == impkg; });
    if (find == m_package_names.end()) {
      m_package_names.emplace_back(impkg);
    }
    auto import = MakeOwned<ImportSpec>();
    import->content.prefixPaths = im->content.prefixPaths;
    const size_t prefixLen = import->content.prefixPaths.size();
    import->content.prefixPoses.resize(prefixLen);
    import->content.prefixDotPoses.resize(prefixLen);
    import->content.identifier = "*";
    import->content.kind = ImportKind::IMPORT_ALL;
    import->content.hasDoubleColon = im->content.hasDoubleColon;
    import->EnableAttr(Attribute::IMPLICIT_ADD, Attribute::COMPILER_ADD);
    import->curFile = file.get();
    file->imports.emplace_back(std::move(import));
  }
}

void CangjieCompilerInstance::AddImportSpecToLLDBExprPackage() {
  auto tempPkg = this->GetSourcePackages()[0];
  for (auto& fPkg : this->m_fake_packages) {
    auto hasDoubleColon = false;
    auto pkgName = fPkg->fullPackageName;
    auto pos = pkgName.find(PACKAGE_SUFFIX);
    if (pos != std::string::npos) {
      hasDoubleColon = true;
      pkgName.replace(pos, PACKAGE_SUFFIX.size(), ".");
    }
    auto pkgPath = Utils::SplitQualifiedName(pkgName);
    for (auto &file : tempPkg->files) {
      auto collectImport = [&file, &pkgPath, &hasDoubleColon](Ptr<const Node> curNode) -> VisitAction {
        // locals: vardecl or funcdecl
        if (pkgPath.back() == LOCAL_PACKAGE_NAME &&
          (curNode->astKind == ASTKind::VAR_DECL || curNode->astKind == ASTKind::FUNC_DECL)) {
          auto localdecl = RawStaticCast<const Decl *>(curNode.get());
          if (HasImportSameIdentifier(file, pkgPath, localdecl->identifier)) {
            return VisitAction::SKIP_CHILDREN;
          }
          // import __lldb_locals.a as __lldb_locals_a
          auto importPackage = MakeOwned<ImportSpec>();
          importPackage->content.prefixPaths = pkgPath;
          importPackage->content.prefixPoses.resize(pkgPath.size());
          importPackage->content.prefixDotPoses.resize(pkgPath.size());
          importPackage->content.identifier = localdecl->identifier.Val();
          importPackage->content.kind = ImportKind::IMPORT_ALIAS;
          if (curNode->astKind == ASTKind::VAR_DECL) {
            importPackage->content.aliasName = LOCAL_PACKAGE_NAME + "_" + localdecl->identifier;
          } else {
            importPackage->content.aliasName = localdecl->identifier.Val();
          }
          importPackage->curFile = file.get();
          importPackage->EnableAttr(Attribute::IMPLICIT_ADD, Attribute::COMPILER_ADD);
          file->imports.emplace_back(std::move(importPackage));
          return VisitAction::SKIP_CHILDREN;
        }
        // globals: vardecl or funcdecl or classdecl
        if (curNode->IsDecl()) {
          if (HasImportSamePkg(file, pkgPath)) {
            return VisitAction::SKIP_CHILDREN;
          }
          // import pkg.*
          auto importPackage = MakeOwned<ImportSpec>();
          importPackage->content.prefixPaths = pkgPath;
          importPackage->content.prefixPoses.resize(pkgPath.size());
          importPackage->content.prefixDotPoses.resize(pkgPath.size());
          importPackage->content.identifier = "*";
          importPackage->content.kind = ImportKind::IMPORT_ALL;
          importPackage->content.hasDoubleColon = hasDoubleColon;
          importPackage->curFile = file.get();
          importPackage->EnableAttr(Attribute::IMPLICIT_ADD, Attribute::COMPILER_ADD);
          file->imports.emplace_back(std::move(importPackage));
          return VisitAction::SKIP_CHILDREN;
        }
        return VisitAction::WALK_CHILDREN;
      };
      AST::Walker walkFakePkg(fPkg.get(), collectImport);
      walkFakePkg.Walk();
      // Add import nodes such as std/collection.
      AddImportNodeFromFakePkg(file, fPkg);
    }
  }
}

OwnedPtr<Cangjie::AST::Expr> CangjieCompilerInstance::CreatePrimitiveInitializer(Ptr<Cangjie::AST::Ty>& ty)
{
  switch (ty->kind) {
    case Cangjie::AST::TypeKind::TYPE_INT8:
    case Cangjie::AST::TypeKind::TYPE_INT16:
    case Cangjie::AST::TypeKind::TYPE_INT32:
    case Cangjie::AST::TypeKind::TYPE_INT64:
    case Cangjie::AST::TypeKind::TYPE_UINT8:
    case Cangjie::AST::TypeKind::TYPE_UINT16:
    case Cangjie::AST::TypeKind::TYPE_UINT32:
    case Cangjie::AST::TypeKind::TYPE_UINT64:
    case Cangjie::AST::TypeKind::TYPE_INT_NATIVE:
    case Cangjie::AST::TypeKind::TYPE_UINT_NATIVE:
      return Cangjie::AST::CreateLitConstExpr(Cangjie::AST::LitConstKind::INTEGER, "0", ty);
    case Cangjie::AST::TypeKind::TYPE_FLOAT16:
    case Cangjie::AST::TypeKind::TYPE_FLOAT32:
    case Cangjie::AST::TypeKind::TYPE_FLOAT64:
      return Cangjie::AST::CreateLitConstExpr(Cangjie::AST::LitConstKind::FLOAT, "0", ty);
    case Cangjie::AST::TypeKind::TYPE_RUNE:
      return Cangjie::AST::CreateLitConstExpr(Cangjie::AST::LitConstKind::RUNE, "a", ty);
    case Cangjie::AST::TypeKind::TYPE_BOOLEAN:
      return Cangjie::AST::CreateLitConstExpr(Cangjie::AST::LitConstKind::BOOL, "true", ty);
    case Cangjie::AST::TypeKind::TYPE_UNIT:
      return Cangjie::AST::CreateLitConstExpr(Cangjie::AST::LitConstKind::UNIT, "()", ty);
    default:
      return nullptr;
  }
}

OwnedPtr<Cangjie::AST::Expr> CangjieCompilerInstance::CreateEnumInitializer(Ptr<AST::Ty>& ty)
{
  auto enumTy = RawStaticCast<AST::EnumTy *>(ty);
  if (!enumTy) {
    return nullptr;
  }
  // Choose the first one to init.
  auto edecl = enumTy->decl;
  if (edecl->constructors.empty()) {
    return nullptr;
  }
  if (enumTy->IsCoreOptionType()) {
    // For Option type, initial is memberAccess, Option<Int64>.None
    auto noneCtor = edecl->constructors[1].get();
    if (noneCtor->astKind == AST::ASTKind::VAR_DECL) {
      auto init = RawStaticCast<AST::VarDecl*>(noneCtor);
      auto memberAccess = AST::CreateMemberAccess(AST::CreateRefExpr(*edecl), init->identifier);
      return memberAccess;
    }
  }
  auto firstCtor = edecl->constructors[0].get();
  if (firstCtor->astKind == AST::ASTKind::VAR_DECL) {
    auto init = RawStaticCast<AST::VarDecl*>(firstCtor);
    auto refExpr = AST::CreateRefExpr(*init);
    return refExpr;
  }
  return nullptr;
}

Ptr<AST::FuncDecl> CangjieCompilerInstance::GetInitFuncFromClassOrStruct(
    std::vector<OwnedPtr<Decl>>& decls, std::vector<Ptr<Cangjie::AST::Ty>>& params)
{
  for (auto& decl : decls) {
    if (decl->TestAttr(Attribute::CONSTRUCTOR)) {
      auto initFunc = RawStaticCast<AST::FuncDecl*>(decl.get());
      for (auto& param : initFunc->funcBody->paramLists[0]->params) {
        if (!param->type) {
          continue;
        }
        CheckTypeAndAddTy(param->type);
        param->ty = param->type->ty;
        params.emplace_back(param->ty);
      }
      return initFunc;
    }
  }
  return nullptr;
}

OwnedPtr<Cangjie::AST::Expr> CangjieCompilerInstance::CreateClassInitializer(Ptr<AST::Ty>& ty)
{
  auto classTy = RawStaticCast<AST::ClassTy *>(ty);
  if (!classTy || !classTy->decl) {
    return nullptr;
  }
  auto cd = classTy->decl;
  std::vector<Ptr<Cangjie::AST::Ty>> params;
  auto initFunc = GetInitFuncFromClassOrStruct(cd->body->decls, params);
  if (!initFunc) {
    return nullptr;
  }
  std::vector<OwnedPtr<FuncArg>> args;
  for (auto& pty : params) {
    args.emplace_back(CreateFuncArg(CreateInitializer(pty)));
  }
  auto refExpr = CreateRefExpr(*initFunc);
  refExpr->ref.identifier = cd->identifier;
  refExpr->ty = typeManager->GetFunctionTy(params, classTy);
  auto call = AST::CreateCallExpr(std::move(refExpr), std::move(args), initFunc, classTy);
  call->callKind = CallKind::CALL_OBJECT_CREATION;
  return call;
}

OwnedPtr<Cangjie::AST::Expr> CangjieCompilerInstance::CreateStructInitializer(Ptr<AST::Ty>& ty)
{
  auto structTy = RawStaticCast<AST::StructTy *>(ty);
  if (!structTy || !structTy->decl) {
    return nullptr;
  }
  auto sd = structTy->decl;
  std::vector<Ptr<Cangjie::AST::Ty>> sparams;
  auto initFunc = GetInitFuncFromClassOrStruct(sd->body->decls, sparams);
  if (!initFunc) {
    return nullptr;
  }
  std::vector<OwnedPtr<FuncArg>> args;
  for (auto& pty : sparams) {
    args.emplace_back(CreateFuncArg(CreateInitializer(pty)));
  }
  auto refExpr = CreateRefExpr(*initFunc);
  refExpr->ref.identifier = sd->identifier;
  refExpr->ty = typeManager->GetFunctionTy(sparams, structTy);
  auto call = AST::CreateCallExpr(std::move(refExpr), std::move(args), initFunc, structTy);
  call->callKind = CallKind::CALL_STRUCT_CREATION;
  return call;
}

OwnedPtr<Cangjie::AST::Expr> CangjieCompilerInstance::CreateFunctionInitializer(Ptr<AST::Ty>& ty)
{
  // var result: (Int8) -> Int8 = {a: Int8 => 0 }
  auto functy = RawStaticCast<AST::FuncTy*>(ty);
  if (!functy) {
    return nullptr;
  }
  auto retTy = functy->retTy;
  auto funcBody = MakeOwned<AST::FuncBody>();
  auto paramList = MakeOwned<AST::FuncParamList>();
  size_t i = 0;
  for (auto& pty : functy->paramTys) {
    auto paramName = "a" + std::to_string(i);
    paramList->params.emplace_back(AST::CreateFuncParam(paramName, nullptr, nullptr, pty));
    i++;
  }
  funcBody->paramLists.emplace_back(std::move(paramList));
  OwnedPtr<AST::Block> block = MakeOwned<AST::Block>();
  block->body.emplace_back(CreateInitializer(retTy));
  funcBody->body = std::move(block);
  funcBody->ty = ty;
  return AST::CreateLambdaExpr(std::move(funcBody));
}

OwnedPtr<Cangjie::AST::Expr> CangjieCompilerInstance::CreateRangeInitializer(Ptr<AST::Ty>& ty)
{
  if (ty->typeArgs.empty()) {
    return nullptr;
  }
  auto rangeExpr = MakeOwned<Cangjie::AST::RangeExpr>();
  rangeExpr->startExpr = CreateInitializer(ty->typeArgs[0]);
  rangeExpr->stopExpr = CreateInitializer(ty->typeArgs[0]);
  auto int64ty = typeManager->GetPrimitiveTy(AST::TypeKind::TYPE_INT64);
  rangeExpr->stepExpr = AST::CreateLitConstExpr(Cangjie::AST::LitConstKind::INTEGER, "1", int64ty);
  rangeExpr->ty = ty;
  rangeExpr->decl = importManager.GetCoreDecl<Cangjie::AST::StructDecl>(RANGE_NAME);
  return rangeExpr;
}

OwnedPtr<Cangjie::AST::Expr> CangjieCompilerInstance::CreateInitializer(Ptr<Cangjie::AST::Ty>& ty)
{
  if (!ty) {
    return nullptr;
  }
  if (ty->IsPrimitive()) {
    return CreatePrimitiveInitializer(ty);
  }
  if (ty->IsFunc()) {
    return CreateFunctionInitializer(ty);
  }
  if (ty->IsClass()) {
    return CreateClassInitializer(ty);
  }
  if (ty->IsEnum()) {
    return CreateEnumInitializer(ty);
  }
  if (ty->IsString()) {
    return AST::CreateLitConstExpr(Cangjie::AST::LitConstKind::STRING, "", ty);
  }
  if (ty->IsStructArray()) {
    std::vector<OwnedPtr<Cangjie::AST::Expr>> elements;
    elements.emplace_back(CreateInitializer(ty->typeArgs[0]));
    return AST::CreateArrayLit(std::move(elements), ty);
  }
  if (ty->IsTuple()) {
    std::vector<OwnedPtr<Cangjie::AST::Expr>> elements;
    for (auto& subTy : ty->typeArgs) {
      elements.emplace_back(CreateInitializer(subTy));
    }
    return AST::CreateTupleLit(std::move(elements), ty);
  }
  if (ty->kind == Cangjie::AST::TypeKind::TYPE_VARRAY) {
    auto ret = MakeOwned<ArrayLit>();
    auto varrayTy = RawStaticCast<Cangjie::AST::VArrayTy *>(ty);
    for (int64_t i = 0; i < varrayTy->size; i++) {
      ret->children.emplace_back(CreateInitializer(ty->typeArgs[0]));
    }
    ret->ty = ty;
    return ret;
  }
  if (ty->IsRange()) {
    return CreateRangeInitializer(ty);
  }
  if (ty->IsStruct()) {
    return CreateStructInitializer(ty);
  }
  return nullptr;
}

Ptr<AST::Ty> CangjieCompilerInstance::AddObjectToFunctionTy(Ptr<AST::Ty>& fTy, Ptr<AST::Ty>& objectTy)
{
    auto funcTy = RawStaticCast<FuncTy*>(fTy);
    std::vector<Ptr<Cangjie::AST::Ty>> ptys{objectTy};
    for (auto &pty : funcTy->paramTys) {
      ptys.emplace_back(pty);
    }
    return typeManager->GetFunctionTy(ptys, funcTy->retTy);
}

void CangjieCompilerInstance::AddCapturedvarsToCallExprOfLocalFunc()
{
  auto modifyCall = [this](Ptr<AST::Node> curNode) -> AST::VisitAction {
    if (curNode->astKind != AST::ASTKind::CALL_EXPR) {
      return AST::VisitAction::WALK_CHILDREN;
    }
    auto ce = RawStaticCast<CallExpr *>(curNode.get());
    if (ce->baseFunc && ce->baseFunc->astKind == AST::ASTKind::REF_EXPR) {
      auto re = RawStaticCast<RefExpr *>(ce->baseFunc.get());
      if (re->ref.target && re->ref.target->fullPackageName == LOCAL_PACKAGE_NAME &&
          re->ref.target->astKind == AST::ASTKind::FUNC_DECL) {
        auto fd = RawStaticCast<FuncDecl*>(re->ref.target);
        // Add param "$CapturedVars: Object" to local funcdecl.
        auto paramList = fd->funcBody->paramLists[0].get();
        OwnedPtr<FuncParam> param = MakeOwned<FuncParam>();
        param->identifier = LOCAL_FUNC_CAPTUREDVAR;
        param->EnableAttr(Attribute::COMPILER_ADD);

        // modify: foo() -> foo(foo$Object)
        auto refExpr = MakeOwned<RefExpr>();
        AddCurFile(*refExpr, ce->curFile);
        refExpr->ref.identifier = re->ref.identifier + "$Object";
        for (auto& vd : m_captured_vars) {
          if (vd->identifier == refExpr->ref.identifier) {
            refExpr->ref.target = vd;
            refExpr->ty = vd->ty;
            param->ty = vd->ty;
            fd->ty = AddObjectToFunctionTy(fd->ty, vd->ty);
            re->ty = AddObjectToFunctionTy(re->ty, vd->ty);
            break;
          }
        }
        paramList->params.insert(paramList->params.begin(), std::move(param));
        refExpr->ref.identifier.SetPos(refExpr->begin,
            refExpr->begin + refExpr->ref.identifier.Val().size());
        ce->args.insert(ce->args.begin(), CreateFuncArg(std::move(refExpr)));
        ce->desugarArgs.reset();
        return AST::VisitAction::SKIP_CHILDREN;
      }
    }
    return AST::VisitAction::WALK_CHILDREN;
  };

  AST::Walker walker(m_expr_result, modifyCall);
  walker.Walk();
}

void CangjieCompilerInstance::PerformAfterSemaForCjdb()
{
  AddCapturedvarsToCallExprOfLocalFunc();
  Log *log = GetLog(LLDBLog::Expressions);
  if (!m_expr_result || !m_expr_result->ty) {
    if (log) {
      LLDB_LOGF(log, "%s's type not found\n", EXPR_RESULT_NAME.c_str());
    }
    return;
  }
  // Get the mangle value of the result's type of an expression evaluation after the type is semantically inferred.
  m_result_type_name = BaseMangler().MangleType(*m_expr_result->ty);
  if (log) {
    LLDB_LOGF(log, "result type name is:%s -> %s\n", m_expr_result->ty->String().c_str(), m_result_type_name.c_str());
    std::string str = GetStringOfASTNode(this->GetSourcePackages()[0]);
    LLDB_LOGF(log, "-------- Package Node after sema -------- \n%s", str.c_str());
  }
}

bool CangjieCompilerInstance::IsUserLookupFunction() {
  Log *log = GetLog(LLDBLog::Expressions);
  CJC_ASSERT(m_expr_result->initializer.get()->astKind == AST::ASTKind::BLOCK);
  if (!m_expr_result->ty->IsFunc()) {
    return false;
  }
  auto block = RawStaticCast<AST::Block *>(m_expr_result->initializer.get());
  if (block->body.back()->astKind == AST::ASTKind::REF_EXPR) {
    auto refExpr = RawStaticCast<AST::RefExpr *>(block->body.back().get());
    if (refExpr->ref.target && refExpr->ref.target->astKind == AST::ASTKind::FUNC_DECL) {
      // CC is closed only for global function. For member function, CC is needed.
      LLDB_LOGF(log, "user is searching global func name now! \n");
      return true;
    }
  } else if (block->body.back()->astKind == AST::ASTKind::MEMBER_ACCESS) {
    auto member = RawStaticCast<AST::MemberAccess *>(block->body.back().get());
    if (member->target && member->target->astKind == AST::ASTKind::FUNC_DECL) {
      LLDB_LOGF(log, "user is searching member func name now!\n");
      return true;
    }
  }
  return false;
}
