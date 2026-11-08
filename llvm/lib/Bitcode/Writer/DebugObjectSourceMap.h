// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LLVM_LIB_BITCODE_WRITER_DEBUGOBJECTSOURCEMAP_H
#define LLVM_LIB_BITCODE_WRITER_DEBUGOBJECTSOURCEMAP_H
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Metadata.h"
#include <map>
#include <string>
#include <vector>

namespace llvm {
class Module;
struct DebugObjectFile {
  // Zero is the normal debug sentinel. Other zero entries have no predecessor.
  std::vector<unsigned> Lines;
  std::map<std::string, std::string> Checksums;
};
struct DebugObjectSourceMap {
  std::map<std::pair<std::string, std::string>, DebugObjectFile> Files;
  llvm::DenseMap<const llvm::DIFile *, const DebugObjectFile *> Resolved;
  llvm::StringMap<std::string> Strings;
  bool Valid = true;

  void prepare();
  const DebugObjectFile *lookup(const DIFile *F);
  unsigned line(const Metadata *N, unsigned L);
  StringRef string(const MDString *S) const;
  void checkFileChecksum(const DIFile *F);
  // An interned MDString may be shared by the DIFile checksum and ordinary
  // metadata. A substitution is valid only for the file checksum operand.
  void checkReference(const Metadata *MD) {
    if (MD && isa<MDString>(MD) &&
        Strings.contains(cast<MDString>(MD)->getString()))
      Valid = false;
  }
};

bool writeDebugObjectBitcode(const Module &M, SmallVectorImpl<char> &Buffer,
                             DebugObjectSourceMap *Map);
} // namespace llvm
#endif
