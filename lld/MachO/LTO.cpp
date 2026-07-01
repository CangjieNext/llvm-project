//===- LTO.cpp ------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "LTO.h"
#include "Config.h"
#include "Driver.h"
#include "InputFiles.h"
#include "Symbols.h"
#include "Target.h"

#include "lld/Common/Args.h"
#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/Strings.h"
#include "lld/Common/TargetOptionsCommandFlags.h"
#include "llvm/LTO/Config.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Support/Caching.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/ObjCARC.h"

using namespace lld;
using namespace lld::macho;
using namespace llvm;
using namespace llvm::MachO;
using namespace llvm::sys;

static Optional<StringRef> getBitcodePackageName(const BitcodeFile &file) {
  if (!file.obj)
    return None;

  // For Cangjie bitcode, the module identifier is:
  //   <subCHIRPackageIdx>-<pkgName>
  StringRef pkgName = file.obj->getSourceFileName();
  unsigned subCHIRPackageIdx = 0;
  constexpr unsigned decimalBase = 10;
  if (pkgName.consumeInteger(decimalBase, subCHIRPackageIdx) ||
      !pkgName.consume_front("-") || pkgName.empty())
    return None;
  return pkgName;
}

static bool shouldKeepVisibleForPkg(const BitcodeFile &file) {
  if (config->hideAllVisiblePkgs)
    return false;
  if (!config->hasMatchedVisiblePkg || config->visiblePkgs.empty())
    return true;
  Optional<StringRef> pkgName = getBitcodePackageName(file);
  return pkgName && config->visiblePkgs.count(*pkgName);
}

static lto::Config createConfig() {
  lto::Config c;
  c.Options = initTargetOptionsFromCodeGenFlags();
  c.Options.EmitAddrsig = config->icfLevel == ICFLevel::safe;
  c.CodeModel = getCodeModelFromCMModel();
  c.CPU = getCPUStr();
  c.MAttrs = getMAttrs();
  c.DiagHandler = diagnosticHandler;
  c.PreCodeGenPassesHook = [](legacy::PassManager &pm) {
    pm.add(createObjCARCContractPass());
  };
  c.TimeTraceEnabled = config->timeTraceEnabled;
  c.TimeTraceGranularity = config->timeTraceGranularity;
  c.OptLevel = config->ltoo;
  c.CGOptLevel = args::getCGOptLevel(config->ltoo);
  c.OpaquePointers =false;
  if (config->saveTemps)
    checkError(c.addSaveTemps(config->outputFile.str() + ".",
                              /*UseInputModulePath=*/true));
  return c;
}

BitcodeCompiler::BitcodeCompiler() {
  lto::ThinBackend backend = lto::createInProcessThinBackend(
      heavyweight_hardware_concurrency(config->thinLTOJobs));
  ltoObj = std::make_unique<lto::LTO>(createConfig(), backend);
}

void BitcodeCompiler::add(BitcodeFile &f) {
  ArrayRef<lto::InputFile::Symbol> objSyms = f.obj->symbols();
  std::vector<lto::SymbolResolution> resols;
  resols.reserve(objSyms.size());
  bool exportForPkg = shouldKeepVisibleForPkg(f);

  // Provide a resolution to the LTO API for each symbol.
  bool defaultExportDynamic =
      config->outputType != MH_EXECUTE || config->exportDynamic;
  bool exportDynamic = defaultExportDynamic && exportForPkg;
  auto symIt = f.symbols.begin();
  for (const lto::InputFile::Symbol &objSym : objSyms) {
    resols.emplace_back();
    lto::SymbolResolution &r = resols.back();
    Symbol *sym = *symIt++;

    // Ideally we shouldn't check for SF_Undefined but currently IRObjectFile
    // reports two symbols for module ASM defined. Without this check, lld
    // flags an undefined in IR with a definition in ASM as prevailing.
    // Once IRObjectFile is fixed to report only one symbol this hack can
    // be removed.
    r.Prevailing = !objSym.isUndefined() && sym->getFile() == &f;

    if (const auto *defined = dyn_cast<Defined>(sym)) {
      r.ExportDynamic =
          defined->isExternal() && !defined->privateExtern && exportDynamic;
      r.FinalDefinitionInLinkageUnit =
          !defined->isExternalWeakDef() && !defined->interposable;
    } else if (const auto *common = dyn_cast<CommonSymbol>(sym)) {
      r.ExportDynamic = !common->privateExtern && exportDynamic;
      r.FinalDefinitionInLinkageUnit = true;
    }

    r.VisibleToRegularObj =
        sym->isUsedInRegularObj || (r.Prevailing && r.ExportDynamic);

    // Un-define the symbol so that we don't get duplicate symbol errors when we
    // load the ObjFile emitted by LTO compilation.
    if (r.Prevailing)
      replaceSymbol<Undefined>(sym, sym->getName(), sym->getFile(),
                               RefState::Strong);

    // TODO: set the other resolution configs properly
  }
  checkError(ltoObj->add(std::move(f.obj), resols));
}

// Merge all the bitcode files we have seen, codegen the result
// and return the resulting ObjectFile(s).
std::vector<ObjFile *> BitcodeCompiler::compile() {
  unsigned maxTasks = ltoObj->getMaxTasks();
  buf.resize(maxTasks);
  files.resize(maxTasks);

  // The -cache_path_lto option specifies the path to a directory in which
  // to cache native object files for ThinLTO incremental builds. If a path was
  // specified, configure LTO to use it as the cache directory.
  FileCache cache;
  if (!config->thinLTOCacheDir.empty())
    cache =
        check(localCache("ThinLTO", "Thin", config->thinLTOCacheDir,
                         [&](size_t task, std::unique_ptr<MemoryBuffer> mb) {
                           files[task] = std::move(mb);
                         }));

  checkError(ltoObj->run(
      [&](size_t task) {
        return std::make_unique<CachedFileStream>(
            std::make_unique<raw_svector_ostream>(buf[task]));
      },
      cache));

  if (!config->thinLTOCacheDir.empty())
    pruneCache(config->thinLTOCacheDir, config->thinLTOCachePolicy);

  if (config->saveTemps) {
    if (!buf[0].empty())
      saveBuffer(buf[0], config->outputFile + ".lto.o");
    for (unsigned i = 1; i != maxTasks; ++i)
      saveBuffer(buf[i], config->outputFile + Twine(i) + ".lto.o");
  }

  // In ThinLTO mode, Clang passes a temporary directory in -object_path_lto,
  // while the argument is a single file in FullLTO mode.
  bool objPathIsDir = true;
  if (!config->ltoObjPath.empty()) {
    if (std::error_code ec = fs::create_directories(config->ltoObjPath))
      fatal("cannot create LTO object path " + config->ltoObjPath + ": " +
            ec.message());

    if (!fs::is_directory(config->ltoObjPath)) {
      objPathIsDir = false;
      unsigned objCount =
          count_if(buf, [](const SmallString<0> &b) { return !b.empty(); });
      if (objCount > 1)
        fatal("-object_path_lto must specify a directory when using ThinLTO");
    }
  }

  // --lto-object-path: merge LTO native objects into a single relocatable output.
  if (!config->ltoObjectPath.empty()) {
    SmallVector<std::string, 8> tmpObjPaths;

    // RAII guard: remove temp files on any exit path.
    // Released paths are excluded from cleanup.
    auto cleanup = [&] {
      for (const std::string &path : tmpObjPaths)
        fs::remove(path);
    };
    auto release = [&](const std::string &path) {
      tmpObjPaths.erase(
          std::remove(tmpObjPaths.begin(), tmpObjPaths.end(), path),
          tmpObjPaths.end());
    };

    for (unsigned i = 0; i != maxTasks; ++i) {
      if (buf[i].empty())
        continue;
      SmallString<128> tmpPath;
      if (auto ec = fs::createTemporaryFile("lto", "o", tmpPath))
        fatal("cannot create temporary file for LTO object: " + ec.message());
      saveBuffer(buf[i], tmpPath);
      // saveBuffer calls error() internally on failure; escalate to fatal.
      if (errorCount())
        fatal("failed to write LTO object to " + tmpPath);
      tmpObjPaths.push_back(tmpPath.str().str());
    }

    if (tmpObjPaths.empty())
      fatal("no LTO native object was produced");

    if (tmpObjPaths.size() == 1) {
      // Single object: write to a temp output first, then atomically rename.
      SmallString<128> tmpOutPath;
      if (auto ec = fs::createTemporaryFile("lto-merged", "o", tmpOutPath))
        fatal("cannot create temporary output file: " + ec.message());
      std::error_code ec = fs::rename(tmpObjPaths[0], tmpOutPath);
      if (ec) {
        ec = fs::copy_file(tmpObjPaths[0], tmpOutPath);
        if (ec)
          fatal("cannot copy LTO object to output: " + ec.message());
      }
      release(tmpObjPaths[0]);
      ec = fs::rename(tmpOutPath, config->ltoObjectPath);
      if (ec)
        fatal("cannot rename " + tmpOutPath + " to " + config->ltoObjectPath +
              ": " + ec.message());
    } else {
      // Multiple objects: spawn ld64.lld -r to merge them.
      std::string lldPath = sys::fs::getMainExecutable(nullptr, nullptr);
      if (lldPath.empty() || !fs::exists(lldPath))
        fatal("cannot locate ld64.lld executable for LTO object merging");

      SmallVector<StringRef, 16> args;
      args.push_back(lldPath);
      args.push_back("-r");
      args.push_back("-o");
      args.push_back(config->ltoObjectPath);
      for (const std::string &p : tmpObjPaths)
        args.push_back(p);

      std::string errMsg;
      raw_string_ostream errOS(errMsg);
      bool executionFailed = false;
      int rc = sys::ExecuteAndWait(lldPath, args, /*Env=*/std::nullopt,
                                   /*Redirects=*/{}, /*SecondsToWait=*/0,
                                   /*MemoryLimit=*/0, &errMsg,
                                   &executionFailed);
      if (executionFailed)
        fatal("failed to execute ld64.lld for LTO object merging: " + errMsg);
      if (rc != 0)
        fatal("ld64.lld -r exited with " + Twine(rc) +
              (errMsg.empty() ? "" : ": " + errMsg));
      if (!fs::exists(config->ltoObjectPath))
        fatal("ld64.lld -r did not produce " + config->ltoObjectPath);
    }

    cleanup();
    return {};
  }

  std::vector<ObjFile *> ret;
  for (unsigned i = 0; i != maxTasks; ++i) {
    if (buf[i].empty())
      continue;
    SmallString<261> filePath("/tmp/lto.tmp");
    uint32_t modTime = 0;
    if (!config->ltoObjPath.empty()) {
      filePath = config->ltoObjPath;
      if (objPathIsDir)
        path::append(filePath, Twine(i) + "." +
                                   getArchitectureName(config->arch()) +
                                   ".lto.o");
      saveBuffer(buf[i], filePath);
      modTime = getModTime(filePath);
    }
    ret.push_back(make<ObjFile>(
        MemoryBufferRef(buf[i], saver().save(filePath.str())), modTime, ""));
  }
  for (std::unique_ptr<MemoryBuffer> &file : files)
    if (file)
      ret.push_back(make<ObjFile>(*file, 0, ""));
  return ret;
}
