//===- OhmDebugObjectCache.h - Checked incremental machine objects -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_CODEGEN_OHMDEBUGOBJECTCACHE_H
#define LLVM_LIB_CODEGEN_OHMDEBUGOBJECTCACHE_H
#include "llvm/ADT/STLFunctionalExtras.h"
namespace llvm {
class CodeGenTargetMachineImpl;
class Module;
class raw_svector_ostream;
namespace ohm {
bool canUseDebugObjectCache();
bool runDebugObjectCache(Module &, CodeGenTargetMachineImpl &,
                         raw_svector_ostream &, function_ref<bool()> Emit);
} // namespace ohm
} // namespace llvm
#endif
