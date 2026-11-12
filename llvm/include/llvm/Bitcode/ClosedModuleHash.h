//===- ClosedModuleHash.h - Identify a live LLVM input ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_BITCODE_CLOSEDMODULEHASH_H
#define LLVM_BITCODE_CLOSEDMODULEHASH_H
#define LLVM_OHM_CLOSED_MODULE_HASH_V1 1

#include "llvm/Support/Compiler.h"
#include <array>
#include <cstdint>

namespace llvm {
class Module;

/// Hash the module identifier, ordered bitcode (including debug locations),
/// and live optimizer state. Return false when the context has an input that
/// cannot be represented. Call before creating analyses. An object cache must
/// independently identify its compiler, target machine, options and external
/// inputs before reusing an output.
LLVM_ABI bool getClosedModuleHashV1(const Module &M,
                                    std::array<uint8_t, 32> &Digest);
} // namespace llvm

#endif
