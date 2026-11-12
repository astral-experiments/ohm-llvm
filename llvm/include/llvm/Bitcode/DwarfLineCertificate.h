//===- DwarfLineCertificate.h - Compare debug line table inputs -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_BITCODE_DWARFLINECERTIFICATE_H
#define LLVM_BITCODE_DWARFLINECERTIFICATE_H
#define LLVM_OHM_CLOSED_DWARF_LINE_CERTIFICATE_V1 1
#define LLVM_OHM_REPLAYABLE_DWARF_LINE_SCOPES_V1 1

#include "llvm/Support/Compiler.h"

namespace llvm {
class Module;
class raw_ostream;

/// Write an ordered bitcode fingerprint, source filenames and source coordinates.
/// The fingerprint preserves location order and equality within each file. Only
/// line numbers and strings used exclusively as DIFile checksums may vary. The
/// caller must also compare filenames, checksums and a monotone map of the old
/// coordinates before attempting to reuse native code or DWARF.
///
/// Returns false without writing anything for unsupported debug information. The
/// format is private to a single compiler version and its codegen configuration.
LLVM_ABI bool writeClosedDwarfLineCertificateV1(const Module &M, raw_ostream &Out);
} // namespace llvm

#endif
