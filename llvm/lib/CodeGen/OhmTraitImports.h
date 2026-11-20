//===- OhmTraitImports.h - Relocatable Rust trait imports -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_CODEGEN_OHMTRAITIMPORTS_H
#define LLVM_LIB_CODEGEN_OHMTRAITIMPORTS_H

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {
class Module;
namespace ohm {
/// Compute a closed line certificate with a provider-independent path for
/// each direct trait import. Key selects candidate donors. Manifest retains
/// their complete proof and original import names for checked rebinding.
bool writeTraitImportSignature(const Module &M, SmallVectorImpl<char> &Key,
                               SmallVectorImpl<char> &Manifest,
                               function_ref<bool(StringRef)> ShouldProbe = {});

/// Compare the old and new manifests, rebase DWARF and restore changed imports
/// in an LLVM-produced ELF object. The caller authenticates the old manifest
/// together with its object and matches the compiler invocation and target.
bool rebindTraitImports(StringRef Object, StringRef Old, StringRef Current,
                        SmallVectorImpl<char> &Output);
} // namespace ohm
} // namespace llvm
#endif
