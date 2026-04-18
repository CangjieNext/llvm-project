//===---------------------- LayoutObfuscator.cpp --------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// The symbol name obfuscation pass.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Obfuscator/LayoutObfuscator.h"
#include <fstream>

using namespace llvm;

extern cl::opt<std::string> ObfInputSymMapFiles;
extern cl::opt<std::string> ObfOutputSymMapFile;
extern cl::opt<std::string> ObfUserSymMapFile;
extern cl::opt<std::string> ObfSymPrefix;
extern cl::opt<bool> EnableObfExportSymol;
extern cl::opt<bool> EnableObfLineNumber;
extern cl::opt<bool> EnableObfSourcePath;

void SymbolMapping::saveOutputMappingFile(const std::string &FileName) {
  std::ofstream F(FileName, std::ios::out);
  if (!F) {
    report_fatal_error("Cannot create mapping file: " + StringRef(FileName));
  }
  for (auto &Map : OutputMapping) {
    if (Map.second.OldName == Map.second.NewName && Map.second.Path.size() == 0) {
      continue;
    }
    F << Map.second.OldName << " " << Map.second.NewName << " " << Map.second.Path << "\n";
  }
  F.close();
}

static std::string SplitBySeparator(
    const std::string &Line, char Separator, size_t &Start, size_t &End, bool CloseBracket = false) {
  std::string Ret = "";
  int ParamNotClose = 0;
  int GenericNotClose = 0;
  static std::vector<std::string> SkipOperator = {
      ".<(",
      ".<<(",
      ".>(",
      ".>>(",
      ".>=(",
      ".<=(",
  };
  size_t Pos = Start;
  if (Start >= Line.size()) {
    return Ret;
  }

  while (Pos < Line.size()) {
    bool Skip = false;
    for (auto &SO : SkipOperator) {
      if (Line.size() - Pos < SO.size())
        continue;
      if (Line.compare(Pos, SO.size(), SO) == 0) {
        Pos += (SO.size() - 1); // don't skip the last bracket.
        Skip = true;
        break;
      }
    }
    if (Skip) {
      continue;
    }
    if (CloseBracket && Line[Pos] == CJSymbol::ParamBrackets[0]) {
      ParamNotClose++;
    } else if (CloseBracket && Line[Pos] == CJSymbol::ParamBrackets[1]) {
      ParamNotClose--;
    } else if (CloseBracket && Line[Pos] == CJSymbol::GenericBrackets[0]) {
      GenericNotClose++;
    } else if (CloseBracket && Line[Pos] == CJSymbol::GenericBrackets[1]) {
      GenericNotClose--;
    } else if ((CloseBracket && ParamNotClose == 0 && GenericNotClose == 0 && Line[Pos] == Separator) ||
              (!CloseBracket && Line[Pos] == Separator)) {
      End = Pos;
      break;
    }
    Pos++;
  }

  if (Pos == Line.size()) {
    End = Line.size();
  }

  Ret = Line.substr(Start, End - Start);
  Start = End + 1;
  return Ret;
}

void SymbolMapping::loadMappingFiles(const std::string &FileNames) {
  size_t Start = 0;
  size_t End = 0;
  while (true) {
    auto FileName = SplitBySeparator(FileNames, ',', Start, End, false);

    loadMappingFile(FileName, [&](const std::string &OldName, const std::string &NewName, const std::string &Path) {
      if (OldName.size() == 0 || NewName.size() == 0) {
        report_fatal_error("Invalid mapping file: " + StringRef(FileName));
      }
      InputMapping[NewName] = {NewName, OldName, Path};
    },
        false);

    if (End == FileNames.size()) {
      break;
    }
  }
}

void SymbolMapping::loadUserMappingFile(const std::string &FileName) {
  loadMappingFile(FileName, [&](const std::string &OldName, const std::string &NewName, const std::string &Path) {
    if (OldName.size() == 0 || NewName.size() == 0) {
      report_fatal_error("Invalid mapping file: " + StringRef(FileName));
    }
    std::string Name = OldName;
    while (true) {
      size_t Pos = Name.find(CJSymbol::IdentSeparator, 0);
      if (Pos == std::string::npos) {
        break;
      }
      Name.replace(Pos, CJSymbol::IdentSeparator.size(), std::string(1, CJSymbol::FieldSeparator));
    }
    Name.erase(std::remove(Name.begin(), Name.end(), ' '), Name.end());
    for (auto &Map : UserMapping) {
      if (Map.first == Name) {
        report_fatal_error("Duplicate symbol: " + StringRef(OldName));
      }
      if (Map.second == NewName) {
        report_fatal_error("Duplicate symbol: " + StringRef(NewName));
      }
    }
    UserMapping[Name] = NewName;
  },
      true);
}

void SymbolMapping::loadMappingFile(
    const std::string &FileName,
    const std::function<void(const std::string &, const std::string &, const std::string &)> &UpdateMapping,
    const bool CloseBracket) {
  std::ifstream InputFile(FileName);
  std::string Line;
  size_t Start = 0;
  size_t End = 0;
  bool Success = true;
  if (!InputFile.is_open()) {
    report_fatal_error("Cannot open file: " + StringRef(FileName));
    return;
  }

  while (getline(InputFile, Line)) {
    std::string SLine = "";
    for (auto C : Line) {
      if (C != '\n' && C != '\r')
        SLine += C;
    }
    if (SLine == "")
      continue;

    Start = 0;
    End = 0;
    std::string OldName = SplitBySeparator(SLine, ' ', Start, End, CloseBracket);
    std::string NewName = SplitBySeparator(SLine, ' ', Start, End, CloseBracket);
    std::string Path = SplitBySeparator(SLine, ' ', Start, End, CloseBracket);

    if (NewName.size() == 0 || OldName.size() == 0) {
      Success = false;
      break;
    }
    UpdateMapping(OldName, NewName, Path);
  }

  if (!Success) {
    report_fatal_error("Invalid mapping file: " + StringRef(FileName));
  }
}

void SymbolMapping::addToOutputMapping(
    const std::string &OldName, const std::string &NewName, const std::string &Path) {
  OutputMapping[OldName] = {NewName, OldName, Path};
}

bool SymbolMapping::isObfuscatedSymbol(const std::string &Name) {
  return OutputMapping.find(Name) != OutputMapping.end();
}

static std::string trimDemangleName(std::string Name) {
  if (!ObfConfig::isValidMangleName(Name)) {
    return Name;
  }

  auto DName = ObfConfig::getDemangleName(Name);
  while (true) {
    size_t Pos = DName.find(CJSymbol::IdentSeparator, 0);
    if (Pos == std::string::npos) {
      break;
    }
    DName.replace(Pos, CJSymbol::IdentSeparator.size(), std::string(1, CJSymbol::FieldSeparator));
  }
  DName.erase(std::remove(DName.begin(), DName.end(), ' '), DName.end());
  return DName;
}

std::string SymbolMapping::getMappedName(const std::string &Name) {
  for (auto &Map : InputMapping) {
    if (Map.second.OldName == Name) {
      return Map.second.NewName;
    }
  }
  std::string DemangleName = trimDemangleName(Name);
  if (UserMapping.find(DemangleName) != UserMapping.end()) {
    return UserMapping[DemangleName];
  }
  return "";
}

LayoutObfuscator::~LayoutObfuscator() {}

std::string LayoutObfuscator::generateUniqueName(void) {
  while (true) {
    unsigned int RD = this->ObfConfig->getRandNum() % LIST_LENGTH;
    std::string Name(1, char('a' + RD));
    Name = ObfSymPrefix + Name;
    if (IndexList[RD] != -1) {
      Name = Name + (std::to_string(IndexList[RD]));
    }
    IndexList[RD]++;
    for (auto &Map : SymbolMap.UserMapping) {
      if (Map.first == Name || Map.second == Name)
        continue;
    }
    if (AllSymbols.count(Name) == 0 && !SymbolMap.isObfuscatedSymbol(Name)) {
      return Name;
    }
  }
}

void LayoutObfuscator::doObfuscation(GlobalVariable &G) {
  if (!needObfuse(G)) {
    return;
  }

  std::string NewName;
  std::string OldName = G.getName().str();
  std::string MappedName = SymbolMap.getMappedName(OldName);
  if (MappedName.size() != 0) {
    NewName = MappedName;
  } else {
    NewName = generateUniqueName();
  }
  G.setName(NewName);

  if (llvm::GlobalVariable::isExternalLinkage(G.getLinkage())) {
    SymbolMap.addToOutputMapping(OldName, NewName);
  }
  AllSymbols.erase(OldName);
  AllSymbols.insert(NewName);
}

bool LayoutObfuscator::isPublicGlobal(const GlobalVariable &G) {
  return !G.hasInitializer() || G.hasExternalLinkage() || G.hasWeakODRLinkage() || G.hasLinkOnceLinkage();
}

bool LayoutObfuscator::needObfuse(const GlobalVariable &G) {
  std::string GlobalName = G.getName().str();
  std::vector<std::string> AvoidAttribute = {
    "cj-native",
    "CFileReflect",
    "CFileKlass",
    "CFileVTable",
    "CFileMTable",
    "CFileStaticGenericTI",
    "Reflection",
    "ReflectionGV",
    "CFileTIExt",
  };
  if (GlobalName.empty() || G.getName().startswith("llvm.")) {
    return false;
  }
  if (G.getName().endswith(".ti") || G.getName().endswith(".tt")) {
    return false;
  }
  if (G.getName().equals("cj.sdk.version")) {
    return false;
  }
  if (SymbolMap.isObfuscatedSymbol(GlobalName)) {
    return false;
  }
  if (!EnableObfExportSymol && isPublicGlobal(G)) {
    return false;
  }

  for (auto Attr : AvoidAttribute) {
    if (G.hasAttribute(Attr)) {
      return false;
    }
  }

  std::string DN = trimDemangleName(GlobalName);
  for (auto &IS : InternalSymbols) {
    if (DN == IS) {
      return false;
    }
  }

  // The global is from other package and has no mapping.
  if (SymbolMap.getMappedName(GlobalName).empty() && G.isDeclaration()) {
    return false;
  }

  if (!this->ObfConfig->needObfuse(GlobalName, "obf-layout")) {
    return false;
  }
  return true;
}

bool LayoutObfuscator::isPublicFunction(const Function &F) {
  return (F.isDeclaration() || !F.hasInternalLinkage() ||
      F.hasAvailableExternallyLinkage());
}

bool LayoutObfuscator::needObfuse(const Function &F) {
  std::vector<std::string> ReserveVec = {"main"};
  std::string FuncName = F.getName().str();
  std::vector<std::string> AvoidAttribute = {
    "c2cj",
    "cj2c",
    "cjstub",
  };

  if (FuncName.size() == 0 || F.getName().startswith("llvm.")) {
    return false;
  }

  if (F.isDeclaration() && SymbolMap.getMappedName(FuncName).size() == 0) {
    return false;
  }

  if (F.hasPrivateLinkage()) {
    return false;
  }

  if (F.getName().endswith(".exception_outlined_func")) {
    return false;
  }

  if (!EnableObfExportSymol && isPublicFunction(F)) {
    return false;
  }

  if (SymbolMap.isObfuscatedSymbol(FuncName)) {
    return false;
  }

  for (auto R : ReserveVec) {
    if (R == FuncName)
      return false;
  }
  for (auto Attr : AvoidAttribute) {
    if (F.hasFnAttribute(Attr))
      return false;
  }

  std::string DN = trimDemangleName(FuncName);
  for (auto &IS : InternalSymbols) {
    if (DN == IS)
      return false;
  }

  if (!this->ObfConfig->needObfuse(FuncName, "obf-layout")) {
    return false;
  }

  return true;
}

// Filter out the basic library functions when the exception stacktrace is
// dumped.
std::set<std::string> FiltMangleNameSet = {
    "user.main",
    "cj_entry$",
    "_CNat",
    "rt$",
};

bool isNeedFilt(std::string MangleName) {
  for (auto FiltMangleName : FiltMangleNameSet) {
    if (MangleName.find(FiltMangleName) == 0)
      return true;
  }
  return false;
}

void LayoutObfuscator::doObfuscation(Function &F) {
  if (!needObfuse(F))
    return;
  std::string NewName;
  std::string OldName = F.getName().str();
  std::string MappedName = SymbolMap.getMappedName(OldName);
  if (isNeedFilt(OldName))
    F.addFnAttr("cj_stack_trace_omit");
  if (MappedName.size() != 0) {
    NewName = MappedName;
  } else {
    NewName = generateUniqueName();
  }
  F.setName(NewName);
  SymbolMap.addToOutputMapping(OldName, NewName);
  AllSymbols.erase(OldName);
  AllSymbols.insert(NewName);
}

void LayoutObfuscator::reorderFunctions(Module &M) {
  std::vector<Function *> FuncVec;
  std::vector<int> IndexVec;
  int FuncNum = 0;
  int Index = 0;

  for (auto &F : M) {
    FuncVec.push_back(&F);
    IndexVec.push_back(FuncNum);
    FuncNum++;
  }

  for (int Idx = 0; Idx < FuncNum; Idx++) {
    M.getFunctionList().remove(FuncVec[Idx]);
  }

  for (int Count = 0; Count < FuncNum; Count++) {
    Index = this->ObfConfig->getRandNum() % IndexVec.size();
    Function *F = FuncVec[IndexVec[Index]];
    M.getFunctionList().push_back(F);
    IndexVec.erase(IndexVec.begin() + Index);
  }
}

void LayoutObfuscator::recordDebugInfo(const Module &M) {
#if defined(_WIN32)
  const char *Separator = "\\";
#else
  const char *Separator = "/";
#endif
  for (auto &F : M) {
    MDNode *N = F.getMetadata("dbg");
    if (N == nullptr) {
      continue;
    }

    auto DS = dyn_cast<DISubprogram>(N);
    if (DS == nullptr) {
      continue;
    }
    std::string Path = DS->getFile()->getFilename().str();
    std::string Dir = DS->getFile()->getDirectory().str();
    if (Path.size() == 0) {
      continue;
    }
    for (auto &Map : SymbolMap.OutputMapping) {
      if (Map.second.NewName == F.getName().str() || Map.second.OldName == F.getName().str()) {
        Map.second.Path = Dir + Separator + Path;
      }
    }
  }
}

void LayoutObfuscator::obfuscateDebugInfo(Module &M) {
  for (auto &F : M) {
    for (auto I = inst_begin(F); I != inst_end(F); I++) {
      MDNode *N = I->getMetadata("dbg");
      if (N == nullptr) {
        continue;
      }
      auto DL = dyn_cast<DILocation>(N);

      if (EnableObfSourcePath && DL->getFile()->getDirectory() != OBF_DEBUG_DIR_NAME) {
        DL->getFile()->replaceOperandWith(1, MDString::get(M.getContext(), OBF_DEBUG_DIR_NAME));
      }
      if (EnableObfSourcePath && DL->getFile()->getFilename() != OBF_DEBUG_FILE_NAME) {
        DL->getFile()->replaceOperandWith(0, MDString::get(M.getContext(), OBF_DEBUG_FILE_NAME));
      }

      if (EnableObfLineNumber) {
        int NewNo = OBF_DEBUG_LINE_NO;
        auto NewDL = DILocation::get(
            DL->getContext(), NewNo, NewNo, DL->getScope(), DL->getInlinedAt(), DL->isImplicitCode());
        I->setMetadata("dbg", NewDL);
      }
    }
  }
}

void LayoutObfuscator::recordAllSymbols(const Module &M) {
  for (auto &F : M) {
    AllSymbols.insert(F.getName().str());
  }

  for (auto &G : M.globals()) {
    AllSymbols.insert(G.getName().str());
  }

  for (auto &Map : SymbolMap.InputMapping) {
    AllSymbols.insert(Map.second.NewName);
    AllSymbols.insert(Map.second.OldName);
  }
  for (auto &Map : SymbolMap.UserMapping) {
    AllSymbols.insert(Map.first);
    AllSymbols.insert(Map.second);
  }
}

void LayoutObfuscator::getCurrentPackage(const Module &M) {
  for (auto &F : M) {
    MDNode *N = F.getMetadata("dbg");
    if (N == nullptr) {
      continue;
    }
    auto DIS = dyn_cast<DISubprogram>(N);
    if (DIS == nullptr) {
      continue;
    }
    auto DNS = dyn_cast<DINamespace>(DIS->getScope());
    if (DNS == nullptr) {
      continue;
    }
    std::string Package = DNS->getName().str();
    CurrentPackage = Package;
    break;
  }
#if defined(_WIN32)
  const char *Separator = "\\";
#else
  const char *Separator = "/";
#endif
  while (true) {
    size_t Pos = CurrentPackage.find(Separator, 0);
    if (Pos == std::string::npos) {
      break;
    }
    CurrentPackage.replace(Pos, 1, std::string(1, CJSymbol::FieldSeparator));
  }
}

PreservedAnalyses LayoutObfuscator::run(Module &M, ModuleAnalysisManager &AM) {
  this->ObfConfig = AM.getResult<ObfConfigAnalysis>(M);
  getCurrentPackage(M);
  if (ObfOutputSymMapFile.size() == 0) {
    ObfOutputSymMapFile = CurrentPackage + ".obf.map";
  }
  if (ObfInputSymMapFiles.size() != 0)
    SymbolMap.loadMappingFiles(ObfInputSymMapFiles);
  if (ObfUserSymMapFile.size() != 0)
    SymbolMap.loadUserMappingFile(ObfUserSymMapFile);
  recordAllSymbols(M);
  for (auto I = 0; I < LIST_LENGTH; I++) {
    IndexList[I] = -1;
  }
  for (auto &F : M)
    doObfuscation(F);
  for (auto &G : M.globals())
    doObfuscation(G);
  recordDebugInfo(M);
  obfuscateDebugInfo(M);
  reorderFunctions(M);
  SymbolMap.saveOutputMappingFile(ObfOutputSymMapFile);
  return PreservedAnalyses::all();
}
