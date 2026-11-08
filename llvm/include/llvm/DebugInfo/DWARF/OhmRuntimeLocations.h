//===- OhmRuntimeLocations.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DEBUGINFO_DWARF_OHMRUNTIMELOCATIONS_H
#define LLVM_DEBUGINFO_DWARF_OHMRUNTIMELOCATIONS_H

#include "llvm-c/Types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/DebugInfo/DWARF/OhmDwarfRebase.h"
#include "llvm/Support/Compiler.h"

namespace llvm::ohm {
/// Apply source-qualified integer changes to independently identified data
/// globals. The caller has proved the complete old/new optimized IR relation
/// and the allowed uses of the original globals before entering this routine.
LLVM_ABI Error rebaseRuntimeLocations(MemoryBufferRef Old, const SourceMap &Map,
                                      StringRef Witness,
                                      MutableArrayRef<char> Result);
} // namespace llvm::ohm

extern "C" LLVM_ABI int
ohm_debug_rebase_locations_v1(const void *Map, size_t MapSize,
                              const void *Locations, size_t LocationsSize,
                              const void *Old, size_t OldSize,
                              LLVMMemoryBufferRef *Output, const char **Reason);
#endif
