// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LLVM_CODEGEN_OHMEMITTEDGLOBALS_H
#define LLVM_CODEGEN_OHMEMITTEDGLOBALS_H

#include "llvm/Support/Compiler.h"

namespace llvm {
class AsmPrinter;
class Module;

namespace ohm {
/// Observe this thread's exact module after its globals were emitted, while
/// their target symbols are still alive. Nested emission cannot claim the
/// outer observation. Clients must reject multiple callbacks for one object.
class LLVM_ABI ScopedEmittedGlobals {
  friend class llvm::AsmPrinter;
  using Callback = void (*)(void *, const AsmPrinter &, const Module &);
  static thread_local ScopedEmittedGlobals *Current;
  const Module &Expected;
  Callback Observe;
  void *Context;

  static void notify(const AsmPrinter &Printer, const Module &M);

public:
  ScopedEmittedGlobals(const Module &Expected, Callback Observe, void *Context);
  ~ScopedEmittedGlobals();
  ScopedEmittedGlobals(const ScopedEmittedGlobals &) = delete;
  ScopedEmittedGlobals &operator=(const ScopedEmittedGlobals &) = delete;
};
} // namespace ohm
} // namespace llvm
#endif
