//===- OhmOptimizerState.h - Context inputs for checked optimization ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_IR_OHMOPTIMIZERSTATE_H
#define LLVM_IR_OHMOPTIMIZERSTATE_H
#include "llvm/Support/Compiler.h"
namespace llvm {
class Module;
template <typename T> class SmallVectorImpl;
namespace ohm {
/// Bind context-only value/metadata wrapper state in deterministic operand
/// encounter order. Unrepresented references or externally owned state miss.
LLVM_ABI bool getOptimizerContextState(Module &M, SmallVectorImpl<char> &Bits);
} // namespace ohm
} // namespace llvm
#endif
