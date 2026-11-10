//===- DiagnosticObservation.h - Observe diagnostics during a phase -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_DIAGNOSTICOBSERVATION_H
#define LLVM_IR_DIAGNOSTICOBSERVATION_H

#include "llvm/Support/Compiler.h"
#include <cstdint>

namespace llvm {
class LLVMContext;

/// Records whether the context issued diagnostics within this scope. Disabled
/// optimization remarks are excluded. Nested observations are independent.
class DiagnosticObservation {
  LLVMContext &Context;
  uint64_t Before;

public:
  LLVM_ABI explicit DiagnosticObservation(LLVMContext &C);
  LLVM_ABI ~DiagnosticObservation();
  DiagnosticObservation(const DiagnosticObservation &) = delete;
  DiagnosticObservation &operator=(const DiagnosticObservation &) = delete;

  LLVM_ABI bool hasDiagnostics() const;
};
} // namespace llvm

#endif
