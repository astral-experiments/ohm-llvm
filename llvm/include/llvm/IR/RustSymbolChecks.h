//===- RustSymbolChecks.h - Check for assembler name conflicts -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_RUSTSYMBOLCHECKS_H
#define LLVM_IR_RUSTSYMBOLCHECKS_H

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

namespace llvm {

/// Check that introducing an ordinary Rust symbol or private lookup table
/// cannot conflict with explicit names outside the IR value symbol table.
/// Callers still need to check for existing IR names.
inline bool canIntroduceRustSymbols(const Module &M) {
  const Triple &T = M.getTargetTriple();
  if (T.getArch() != Triple::x86_64 || !T.isOSLinux() ||
      !T.isOSBinFormatELF() || M.getDataLayout().getGlobalPrefix() ||
      !M.getComdatSymbolTable().empty() || !M.getModuleInlineAsm().empty())
    return false;
  for (const GlobalValue &GV : M.global_values()) {
    // Leading dots can name .L private symbols and implicit ELF sections
    // (e.g. .text.<function>). Escapes and versions can give distinct IR
    // values the same assembler name.
    StringRef Name = GV.getName();
    if (Name.starts_with(".") || Name.starts_with("\1") || Name.contains('@'))
      return false;

    // Rust uses these fixed sections for initializers and metadata. A
    // custom section name can itself define a symbol.
    StringRef Section = GV.getSection();
    if (!Section.empty() && Section != ".init_array" &&
        Section != ".text.startup" && Section != ".debug_gdb_scripts" &&
        Section != "llvm.metadata")
      return false;
    if (const auto *Var = dyn_cast<GlobalVariable>(&GV);
        Var && Var->hasImplicitSection())
      return false;
  }
  for (const Function &F : M)
    for (const BasicBlock &BB : F)
      for (const Instruction &I : BB)
        if (const auto *CB = dyn_cast<CallBase>(&I); CB && CB->isInlineAsm())
          return false;
  return true;
}

} // namespace llvm

#endif // LLVM_IR_RUSTSYMBOLCHECKS_H
