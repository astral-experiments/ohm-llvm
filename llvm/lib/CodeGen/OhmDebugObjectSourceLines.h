//===- OhmDebugObjectSourceLines.h - Recover unchanged locations -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_CODEGEN_OHMDEBUGOBJECTSOURCELINES_H
#define LLVM_LIB_CODEGEN_OHMDEBUGOBJECTSOURCELINES_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>

namespace llvm::ohm {
/// Return the one-based new location of each old line which has the same
/// match on every shortest insertion/deletion path. Other entries are zero.
/// Decline edits too large to certify or without any mandatory matches.
bool getMandatoryDebugSourceLines(ArrayRef<StringRef> Old,
                                  ArrayRef<StringRef> New,
                                  MutableArrayRef<uint32_t> Forward);
} // namespace llvm::ohm

#endif
