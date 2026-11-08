//===- OhmDebugObjectCache.h - Checked incremental machine objects -*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_CODEGEN_OHMDEBUGOBJECTCACHE_H
#define LLVM_LIB_CODEGEN_OHMDEBUGOBJECTCACHE_H
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Module.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
namespace llvm {
class CodeGenTargetMachineImpl;
class raw_svector_ostream;
namespace ohm {
#if defined(__linux__) && defined(__x86_64__)
struct DebugObjectImage {
  std::string Path;
  uintptr_t Address;
};
/// File-backed images in loader order, with an empty pathname for the
/// executable.
SmallVector<DebugObjectImage> getDebugObjectImages();
using DebugObjectImageGeneration = std::pair<uint64_t, uint64_t>;
std::optional<DebugObjectImageGeneration> getDebugObjectImageGeneration();
#endif
/// Small machine inputs cost less to compile than to certify and retain.
inline bool isLargeDebugObjectInput(const Module &M) {
  constexpr unsigned MinInstructions = 32 * 1024;
  return M.getInstructionCount() >= MinInstructions;
}
/// The Linux loader consumes its initial environment before constructors can
/// clear their process-visible variables. A truncated environment is unknown.
inline bool allowsDebugObjectCacheLoader(StringRef InitialEnvironment) {
  if (InitialEnvironment.empty() || InitialEnvironment.back() != '\0')
    return false;
  while (!InitialEnvironment.empty()) {
    auto [Entry, Rest] = InitialEnvironment.split('\0');
    InitialEnvironment = Rest;
    auto [Name, Value] = Entry.split('=');
    if ((Name == "LD_PRELOAD" || Name == "LD_AUDIT") && !Value.empty())
      return false;
  }
  return true;
}
bool canUseDebugObjectCache();
bool runDebugObjectCache(Module &, CodeGenTargetMachineImpl &,
                         raw_svector_ostream &, function_ref<bool()> Emit);
} // namespace ohm
} // namespace llvm
#endif
