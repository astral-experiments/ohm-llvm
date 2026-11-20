//===- LegacyModuleSuffix.h - Wrap a module pipeline suffix -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_LEGACYMODULESUFFIX_H
#define LLVM_IR_LEGACYMODULESUFFIX_H

#include "llvm/IR/LegacyPassManager.h"

namespace llvm::legacy {
/// Inside the builder of a wrapped pipeline, install a second one-shot
/// executor. It will see the module after all passes up to the boundary and
/// may run the remaining passes. The outer executor still runs first.
LLVM_ABI void requestWrappedModuleSuffix(PassManagerBase &PM,
                                        PassManagerBase::ModuleExecutor Suffix);

/// Begin a separate module pass manager only for a requested suffix. Place
/// this boundary where all non-immutable analyses can be recomputed.
LLVM_ABI void startWrappedModuleSuffix(PassManagerBase &PM);
} // namespace llvm::legacy

#endif // LLVM_IR_LEGACYMODULESUFFIX_H
