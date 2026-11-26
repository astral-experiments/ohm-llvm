//===- ClosedOptimizerContext.h - Check one live LLVM input ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_CLOSEDOPTIMIZERCONTEXT_H
#define LLVM_IR_CLOSEDOPTIMIZERCONTEXT_H
#define LLVM_OHM_CLOSED_OPTIMIZER_CONTEXT_V1 1

#include "llvm/Support/Compiler.h"
#include <array>
#include <cstdint>

namespace llvm {
class Module;

/// Fingerprint live use and context state omitted from ordered bitcode. Return
/// false if optimization can observe a constant graph or tracking owner which
/// cannot be described by the module. Call before constructing LLVM analyses.
LLVM_ABI bool getClosedOptimizerContextV1(const Module &M,
                                          std::array<uint8_t, 32> &Digest);
} // namespace llvm

#endif
