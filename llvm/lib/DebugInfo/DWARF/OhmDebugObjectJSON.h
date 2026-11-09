//===- OhmDebugObjectJSON.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_LIB_DEBUGINFO_DWARF_OHMDEBUGOBJECTJSON_H
#define LLVM_LIB_DEBUGINFO_DWARF_OHMDEBUGOBJECTJSON_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include <set>
#include <string>
#include <vector>

namespace llvm::ohm {
// JSON object names must be unique independently of the consumer's choice of
// first/last-wins semantics. The LLVM parser first validates the full syntax.
inline bool uniqueObjectKeys(StringRef Text) {
  std::vector<std::set<std::string>> Keys;
  for (size_t I = 0; I < Text.size(); ++I) {
    char Ch = Text[I];
    if (Ch == '{') {
      Keys.emplace_back();
      continue;
    }
    if (Ch == '}') {
      if (Keys.empty())
        return false;
      Keys.pop_back();
      continue;
    }
    if (Ch != '"')
      continue;
    size_t Begin = I;
    while (++I < Text.size()) {
      if (Text[I] == '\\') {
        ++I;
        continue;
      }
      if (Text[I] == '"')
        break;
    }
    const size_t End = I + 1;
    size_t J = End;
    while (J < Text.size() && (Text[J] == ' ' || Text[J] == '\t' ||
                               Text[J] == '\r' || Text[J] == '\n'))
      ++J;
    if (J < Text.size() && Text[J] == ':') {
      if (Keys.empty())
        return false;
      auto Key = json::parse(Text.slice(Begin, End));
      if (!Key) {
        consumeError(Key.takeError());
        return false;
      }
      auto S = Key->getAsString();
      if (!S || !Keys.back().insert(S->str()).second)
        return false;
    }
  }
  return Keys.empty();
}

} // namespace llvm::ohm
#endif
