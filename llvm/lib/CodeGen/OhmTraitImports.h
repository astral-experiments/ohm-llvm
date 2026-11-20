//===- OhmTraitImports.h - Relocatable Rust trait imports -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_CODEGEN_OHMTRAITIMPORTS_H
#define LLVM_LIB_CODEGEN_OHMTRAITIMPORTS_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {
class Module;
namespace ohm {
/// Produce an ordered, closed optimizer signature, separating the names of
/// directly called v0 trait methods from their instantiating crates.
bool writeTraitImportSignature(const Module &M, SmallVectorImpl<char> &Input,
                               SmallVectorImpl<char> &Imports);

/// Restore each changed import in an LLVM-produced ELF relocatable object.
/// The caller must match the complete signatures before passing these lists.
bool rebindTraitImports(StringRef Object, StringRef Old, StringRef Current,
                        SmallVectorImpl<char> &Output);
} // namespace ohm
} // namespace llvm
#endif
