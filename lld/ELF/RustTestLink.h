//===- RustTestLink.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_RUSTTESTLINK_H
#define LLD_ELF_RUSTTESTLINK_H

#include "llvm/ADT/StringMap.h"
#include "llvm/Support/MemoryBuffer.h"
#include <array>

namespace llvm::opt {
class InputArgList;
}

namespace lld::elf {
struct Ctx;

// Cargo's retained test objects can be regenerated under new session names
// even when none of the bytes read by the final ELF link changed.
class RustTestLink {
public:
  void configure(Ctx &ctx, const llvm::opt::InputArgList &args);
  void disable() {
    output.clear();
    key.clear();
  }
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
  readFile(llvm::StringRef path);
  bool reuse(Ctx &ctx);
  void observeOutput(llvm::StringRef bytes);
  void publish(Ctx &ctx);

private:
  using Generation = std::array<uint64_t, 6>;
  std::string output, objectDir, scratchName, argsHash, key, outputHash;
  llvm::StringMap<Generation> archives;
  std::string normalize(llvm::StringRef path) const;
  bool inputsStillPresent() const;
  std::string inputKey(Ctx &ctx) const;
};

} // namespace lld::elf
#endif
