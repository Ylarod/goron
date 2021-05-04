//===- LTO.cpp ------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "LTO.h"
#include "Config.h"
#include "InputFiles.h"
#include "Symbols.h"
#include "lld/Common/Args.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Strings.h"
#include "lld/Common/TargetOptionsCommandFlags.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/LTO/Caching.h"
#include "llvm/LTO/Config.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Object/SymbolicFile.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace llvm;
using namespace llvm::object;

using namespace lld;
using namespace lld::coff;

static std::unique_ptr<lto::LTO> createLTO() {
  lto::Config c;
  c.Options = initTargetOptionsFromCodeGenFlags();

  // Always emit a section per function/datum with LTO. LLVM LTO should get most
  // of the benefit of linker GC, but there are still opportunities for ICF.
  c.Options.FunctionSections = true;
  c.Options.DataSections = true;

  // Use static reloc model on 32-bit x86 because it usually results in more
  // compact code, and because there are also known code generation bugs when
  // using the PIC model (see PR34306).
  if (config->machine == COFF::IMAGE_FILE_MACHINE_I386)
    c.RelocModel = Reloc::Static;
  else
    c.RelocModel = Reloc::PIC_;
  c.DisableVerify = true;
  c.DiagHandler = diagnosticHandler;
  c.OptLevel = config->ltoo;
  c.CPU = getCPUStr();
  c.MAttrs = getMAttrs();
  c.CGOptLevel = args::getCGOptLevel(config->ltoo);

  if (config->saveTemps)
    checkError(c.addSaveTemps(std::string(config->outputFile) + ".",
                              /*UseInputModulePath*/ true));
  lto::ThinBackend backend;
  if (config->thinLTOJobs != 0)
    backend = lto::createInProcessThinBackend(config->thinLTOJobs);
  return llvm::make_unique<lto::LTO>(std::move(c), backend,
                                     config->ltoPartitions);
}

BitcodeCompiler::BitcodeCompiler() : ltoObj(createLTO()) {}

BitcodeCompiler::~BitcodeCompiler() = default;

static void undefine(Symbol *s) { replaceSymbol<Undefined>(s, s->getName()); }

void BitcodeCompiler::add(BitcodeFile &f) {
  lto::InputFile &obj = *f.obj;
  unsigned symNum = 0;
  std::vector<Symbol *> symBodies = f.getSymbols();
  std::vector<lto::SymbolResolution> resols(symBodies.size());

  // Provide a resolution to the LTO API for each symbol.
  for (const lto::InputFile::Symbol &objSym : obj.symbols()) {
    Symbol *sym = symBodies[symNum];
    lto::SymbolResolution &r = resols[symNum];
    ++symNum;

    // Ideally we shouldn't check for SF_Undefined but currently IRObjectFile
    // reports two symbols for module ASM defined. Without this check, lld
    // flags an undefined in IR with a definition in ASM as prevailing.
    // Once IRObjectFile is fixed to report only one symbol this hack can
    // be removed.
    r.Prevailing = !objSym.isUndefined() && sym->getFile() == &f;
    r.VisibleToRegularObj = sym->isUsedInRegularObj;
    if (r.Prevailing)
      undefine(sym);
  }
  checkError(ltoObj->add(std::move(f.obj), resols));
}

// Merge all the bitcode files we have seen, codegen the result
// and return the resulting objects.
std::vector<StringRef> BitcodeCompiler::compile() {
  unsigned maxTasks = ltoObj->getMaxTasks();
  buf.resize(maxTasks);
  files.resize(maxTasks);

  // The /lldltocache option specifies the path to a directory in which to cache
  // native object files for ThinLTO incremental builds. If a path was
  // specified, configure LTO to use it as the cache directory.
  lto::NativeObjectCache cache;
  if (!config->ltoCache.empty())
    cache = check(lto::localCache(
        config->ltoCache, [&](size_t task, std::unique_ptr<MemoryBuffer> mb) {
          files[task] = std::move(mb);
        }));

  checkError(ltoObj->run(
      [&](size_t task) {
        return llvm::make_unique<lto::NativeObjectStream>(
            llvm::make_unique<raw_svector_ostream>(buf[task]));
      },
      cache));

  if (!config->ltoCache.empty())
    pruneCache(config->ltoCache, config->ltoCachePolicy);

  std::vector<StringRef> ret;
  for (unsigned i = 0; i != maxTasks; ++i) {
    if (buf[i].empty())
      continue;
    if (config->saveTemps) {
      if (i == 0)
        saveBuffer(buf[i], config->outputFile + ".lto.obj");
      else
        saveBuffer(buf[i], config->outputFile + Twine(i) + ".lto.obj");
    }
    ret.emplace_back(buf[i].data(), buf[i].size());
  }

  for (std::unique_ptr<MemoryBuffer> &file : files)
    if (file)
      ret.push_back(file->getBuffer());

  return ret;
}