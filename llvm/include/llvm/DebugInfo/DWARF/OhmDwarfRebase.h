//===- OhmDwarfRebase.h - Checked source coordinate rebasing -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DEBUGINFO_DWARF_OHMDWARFREBASE_H
#define LLVM_DEBUGINFO_DWARF_OHMDWARFREBASE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace llvm::ohm {
using LineMap = std::map<uint32_t, uint32_t>;
using SourceMap = std::map<std::string, LineMap, std::less<>>;

struct RebasedObject {
  std::vector<char> Bytes;
  unsigned LineRows = 0;
  unsigned DeclLines = 0;
  unsigned CallLines = 0;
};

/// Parse a versioned path and physical-line mapping, rejecting ambiguous keys.
LLVM_ABI llvm::Expected<SourceMap> parseSourceMap(llvm::StringRef Json);
/// Rewrite only representable source coordinates in an existing DWARF4 object.
/// The caller authenticates the old object, map and machine-code equivalence.
LLVM_ABI llvm::Expected<RebasedObject> rebaseDwarf4(llvm::MemoryBufferRef Old,
                                                    const SourceMap &Map);
} // namespace llvm::ohm
#endif
