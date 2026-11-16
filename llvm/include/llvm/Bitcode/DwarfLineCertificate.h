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
#define LLVM_OHM_CALLER_LINE_CERTIFICATE_V1 1
#define LLVM_OHM_REPLAYABLE_DWARF_LINE_SCOPES_V1 1

#include "llvm/Support/Compiler.h"

namespace llvm {
class Module;
class raw_ostream;

/// Write a fingerprint of the module identifier, ordered bitcode and live
/// optimizer context together with source filenames and coordinates.
/// The fingerprint preserves location order and equality within each file. Only
/// line numbers and strings used exclusively as DIFile checksums may vary. The
/// caller must also compare filenames, checksums and a monotone map of the old
/// coordinates before attempting to reuse native code or DWARF.
///
/// Returns false without writing anything for unsupported debug information. The
/// format is private to a single compiler version and its codegen configuration.
LLVM_ABI bool writeClosedDwarfLineCertificateV1(const Module &M, raw_ostream &Out);

/// Write a closed optimizer input with checked Rust caller-line constants and
/// debug coordinates represented separately. The three length-prefixed fields
/// following the version are a line certificate, caller-slot identities and
/// their actual lines. A consumer must compare the full input and identities,
/// prove every moved coordinate against both source files, and resolve each
/// static's actual old native owner before reusing an object.
LLVM_ABI bool writeClosedCallerLineCertificateV1(const Module &M,
                                                raw_ostream &Out);
} // namespace llvm

#endif
