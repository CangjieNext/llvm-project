//===-- CangjieDeclMap.h --------------------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_DECL_MAP_H
#define LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_DECL_MAP_H


#include "lldb/lldb-private.h"
#include "lldb/Symbol/CompilerType.h"
#include "lldb/Target/ExecutionContext.h"
#include "CangjieASTUtils.h"
#include "Plugins/TypeSystem/Clang/TypeSystemClang.h"

using namespace Cangjie;
using namespace Cangjie::AST;

namespace lldb_private {

class CangjieDeclMap {
  typedef std::pair<lldb::ModuleSP, CompilerDeclContext> NamespaceMapItem;
  typedef std::vector<NamespaceMapItem> NamespaceMap;
  typedef std::shared_ptr<NamespaceMap> NamespaceMapSP;

public:
    CangjieDeclMap(lldb_private::ExecutionContext exe_ctx,
        std::vector<std::string> identifiers, unsigned int fileID, std::string fileName)
        : m_exe_ctx(exe_ctx), m_identifys(identifiers), m_file_id(fileID), m_current_filename(fileName) {
        InitVariable();
    };
    enum class DeclKind{
        VarDecl,
        FuncDecl,
        UserDefinedDecl,
        BuiltInDecl,
        OtherDecl
    };
    enum class VariableKind{
        LocalVariable,
        StaticVariable,
        GlobalVariable
    };
    enum class EnumLayout {
        E0Int,
        E1Arg,
        E2OptionLike,
        E3ArgNoRef,
    };
    EnumLayout GetEnumLayout(const std::string& name);
    struct CompilerTypeInfo {
        CompilerTypeInfo(CompilerType type, uint32_t line, std::string mangledName)
            : Type(type), Line(line), mangle_name(mangledName){};
        void SetDeclInfo(DeclKind kind, std::string id, bool global, std::string gn = "", bool isLet = false) {
            this->Kind = kind;
            this->name = id;
            this->Global = global;
            this->IsLet = isLet;
            this->genericName = gn;
        }
        CompilerType Type;
        uint32_t Line;
        std::string mangle_name;
        std::string name = "";
        std::string genericName = ""; // for generic memberFunc
        bool Global = true;
        DeclKind Kind;
        bool IsLet = false;
    };
    std::vector<OwnedPtr<AST::Package>> CreateExternalAST();
    TypeSystemClang * GetTypeSystem() { return m_type_system; }
    bool LookUpPackage(ConstString name);
    CompilerType LookUptype(lldb::ModuleSP module,
        ConstString name, const CompilerDeclContext &namespace_decl);
    CompilerType LookUpType(std::string name);
    std::vector<CompilerTypeInfo> LookUpFunction(lldb::ModuleSP module,
        ConstString name, const CompilerDeclContext &namespace_decl);
    CompilerType LookUpGlobalVariable(lldb::ModuleSP module,
        ConstString name, const CompilerDeclContext &namespace_decl);
    std::string GetFuncNameFromComplierType(const CompilerType& funcType);
    lldb::VariableSP FindParsedVarByName(lldb_private::ConstString name);
    lldb_private::CompilerType FindParsedTypesByName(std::string name);
    CompilerType GetPrimitiveTypeByName(std::string &name);
    bool IsInterfaceType(CompilerType& type);
    bool IsInstantiatedOfGenericDecl(std::string name);
    CompilerType GetDynamicTypeFromTy(Ptr<AST::Ty> ty, std::string typeName, CompilerType& genericType);
    lldb_private::CompilerType GetDynamicTypeFromGenericTypeInfo(Ptr<AST::Ty> ty, std::string& demangled_name);
    CompilerType GetDynamicClassType(Ptr<AST::Ty> ty, std::string typeName, CompilerType& genericType);
    CompilerType GetDynamicFuncType(Ptr<AST::Ty> ty, std::string typeName, CompilerType& genericType);
    CompilerType GetDynamicStructType(Ptr<AST::Ty> ty, std::string typeName, CompilerType& genericType);
    CompilerType GetDynamicTupleType(Ptr<AST::Ty> ty, std::string typeName, CompilerType& genericType);
    CompilerType GetDynamicEnumType(Ptr<AST::Ty> ty, std::string typeName, CompilerType& genericType);
    CompilerType GetEnumType(Ptr<AST::Ty> ty, std::string typeName, CompilerType& genericType);
    CompilerType CreateOptionReturnType(Ptr<Cangjie::AST::Ty> ty, std::string& typeName);
    void CreateAndAddInheritTypeToRecordType(Ptr<AST::Ty> ty, CompilerType& enum_type, CompilerType& instancetiateType, CompilerType& generic_type);

    CompilerType GetDynamicArrayType(Ptr<AST::Ty> ty, std::string typeName, CompilerType& genericType);
    CompilerType GetDynamicRawArrayType(Ptr<AST::Ty> ty, std::string typeName, CompilerType& genericType);
    void AddFieldToRecordType(CompilerType& instancedType, Ptr<AST::Ty> ty, std::string& resultTypeName, CompilerType& genericType);
    Ptr<AST::Ty> GetMemberDeclTyByName(Ptr<AST::Ty> ty, std::string& memberName, std::string& parentTypeName);
    size_t GetSubGenericTyIndex(const Ptr<Cangjie::AST::Decl>& decl, std::string memberName);

    lldb_private::CompilerType dynamic_type;
    struct ParsedFunction {
        ParsedFunction(std::string fscope, std::string fName, std::string func_type, lldb_private::CompilerType type)
            : ScopeName(fscope), FuncName(fName), FuncTypeStr(func_type), funcType(type){};
        std::string ScopeName;
        std::string FuncName;
        std::string FuncTypeStr;
        lldb_private::CompilerType funcType;
    };
    std::vector<ParsedFunction> m_parsed_functions;
    std::string m_current_pkgname;

private:
    lldb_private::CangjieASTBuiler builder;
    lldb_private::ExecutionContext m_exe_ctx;
    std::vector<std::string> m_identifys;
    unsigned int m_file_id;
    std::string m_current_filename;
    lldb::ValueObjectSP m_this_variable;
    lldb::VariableListSP m_local_variables;
    lldb::VariableListSP m_global_variables;
    lldb::VariableListSP m_static_variables;

    std::map<ConstString, lldb_private::CompilerType> m_generic_types;
    std::vector<OwnedPtr<Cangjie::AST::Decl>> m_local_decls;
    std::vector<OwnedPtr<Cangjie::AST::Decl>> m_global_decls;
    std::vector<OwnedPtr<Cangjie::AST::Decl>> m_captured_vars;
    std::set<std::string> m_fullpkgs;
    std::map<ConstString, NamespaceMapSP> m_parsed_packages;
    std::map<lldb_private::ConstString, lldb::VariableSP> m_parsed_vars;
    std::map<std::string, lldb_private::CompilerType> m_parsed_types;
    std::map<std::string, uint64_t> m_captured_var_addrs;

    TypeSystemClang *m_type_system = nullptr;

    void InitVariable();
    void CreateInjectedThis();
    void CreateInjectedSuper(Ptr<AST::Decl>& thisdecl);
    void LookUpTypeByName(const std::string& id, std::vector<CompilerTypeInfo>& decls);
    lldb::VariableSP GetVariableByName(const std::string& sname, VariableKind kind);
    CompilerType GetVariableType(lldb::VariableSP var);
    void CollectVarDecl(const std::string& sname, CompilerType& type, VariableKind kind,
                                    std::vector<CompilerTypeInfo>& m_translate_decls);
    CompilerType LookUpVariable(const std::string& name, VariableKind kind);
    void LookUpMemberVariable(CompilerType& type, std::vector<CompilerTypeInfo>& m_translate_decls);
    std::vector<CompilerTypeInfo> LookUpFunction(std::string name, bool find_global);
    OwnedPtr<AST::VarDecl> CreateVariable(const CompilerType& type, std::string& name, Ptr<AST::Decl> decl = nullptr);
    void CreateMemberDecls(CompilerType type, Ptr<AST::Decl> &decl, std::vector<OwnedPtr<Decl>>& members);
    void CreateMemberVarDecls(CompilerType& type, Ptr<AST::Decl> &decl, std::vector<OwnedPtr<Decl>>& members);
    void CreateMemberFuncDecls(CompilerType& type, Ptr<AST::Decl> &decl, std::vector<OwnedPtr<Decl>>& members);
    void CreateStaticVarDecls(CompilerType& type, Ptr<AST::Decl> &decl, std::vector<OwnedPtr<Decl>>& members);
    void CreateClassMemberDecls(CompilerType type, Ptr<AST::Decl> &decl);
    bool IsGenericTypeWithConstraints(CompilerType& type);
    OwnedPtr<GenericConstraint> CreateGenericConstraintsDecl(Ptr<AST::Decl> &decl, CompilerType& type);
    void CreateGenericConstraints(Ptr<AST::Decl>& decl, CompilerType& type);
    std::set<CompilerType> GetGenericTypeWithConstraints(CompilerType& type);
    void CreateStructMemberDecls(CompilerType type, Ptr<AST::Decl> &decl);
    void CreateClassLikeParentDecls(CompilerType type, Ptr<AST::InheritableDecl> decl);
    void SetGenericDeclBySubclass(Ptr<AST::RefType> refType, std::string name,  Ptr<AST::InheritableDecl>& base);
    CompilerType GetSuperClassDefinedType(CompilerType& type);
    void SetMemberDeclParent(Ptr<AST::FuncDecl> funcdecl, Ptr<AST::Decl> decl);
    void CreateInterfaceMemberDecls(Ptr<AST::Decl> &decl, CompilerType type);
    Ptr<AST::Decl> CreateClassDecl(CompilerType type, std::string prefix);
    Ptr<AST::Decl> CreateStructDecl(CompilerType type, std::string prefix);
    Ptr<AST::Decl> CreateEnumDecl(CompilerType type, std::string prefix);
    Ptr<AST::Decl> CreateInterfaceDecl(CompilerType type, std::string prefix);
    Ptr<AST::Decl> CreateTypeDecl(CompilerType type);
    std::vector<ConstString> GetEnumeratorName(CompilerType& type);
    CompilerType GetEnumerationType(const CompilerType& type, bool hasArgs);
    void ReplaceTypeDefWithInterfaceType(CompilerType& type);
    void CacheParsedVars(ConstString name, lldb::VariableSP& var, VariableKind kind);
    void CreateEnumConstructors(OwnedPtr<AST::EnumDecl> &decl, const CompilerType& type);
    void CreateEnumOptionLikeCtors(OwnedPtr<AST::EnumDecl> &decl, const CompilerType& type,
                                   std::vector<ConstString>& list);
    bool AddPackageNameSpace(ConstString nam);
    bool IsLocalConstVariable(const std::string& name);
    void CreateVarDeclByType(const CompilerTypeInfo& type, std::string name);
    void CreateDeclByType(const CompilerTypeInfo& type);
    void CreateFuncDeclByType(const CompilerTypeInfo& type, std::string name, bool find_global);
    void CreateTypeDeclByType(const CompilerType& type);
    Ptr<Decl> GetSameGlobalDecl(OwnedPtr<AST::Decl> decl, bool insert_begin = false);
    void CollectImportPkgName();
    void AddImportSpec(OwnedPtr<Cangjie::AST::File>& file, std::string curPkgName);
    void CreatePackage(const std::string &name);
    void CollectLocalVariableAddress(const std::string& sname, lldb::VariableSP& var);
    bool CheckAndModifyCapturedVardecl(VarDecl& vd, OwnedPtr<AST::FuncParamList>& paramList);
    void CreateCapturedVars();
    std::vector<OwnedPtr<Cangjie::AST::Package>> CreateFakePackage();
    CompilerType GetGenericTypeByPartName(const std::string& part_name);
    CompilerType CreateOrGetGenericType(const CompilerType& type);
    void CollectGenericTypeOfVariable(const CompilerType& type);
    void CollectOptionValType(CompilerType& type);
    std::set<ConstString> m_local_func;
};

} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CANGJIE_DECL_MAP_H
