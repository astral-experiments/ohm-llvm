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

#define LLVM_OHM_X86_REGISTER_COMMENT_CERTIFICATE_V1 1

/// Recognize x86-64 assembly containing only an integer register inside a
/// block comment. Callers must still reject every other assembly input and
/// must only publish objects that produced no backend diagnostics.
LLVM_ABI bool hasOnlyX86RegisterCommentAsmV1(const Module &M);

/// Additionally normalize the private, nonzero source cookie for one such
/// comment. The original instruction and the live diagnostic handler retain
/// the actual cookie. A cookie used anywhere else in the module is rejected.
LLVM_ABI bool writeX86RegisterCommentDwarfCertificateV1(const Module &M,
                                                        raw_ostream &Out);
} // namespace llvm

#endif
