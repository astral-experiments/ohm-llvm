//===- OhmLeafObject.h - Reuse objects across enum edits -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_CODEGEN_OHMLEAFOBJECT_H
#define LLVM_LIB_CODEGEN_OHMLEAFOBJECT_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"
#include <string>

namespace llvm {
class Module;

namespace ohm_leaf {

/// Cheap preflight for modules returning large 16-bit discriminants.
LLVM_ABI_FOR_TEST bool candidate(const Module &M);

/// For an admitted ordinary, non-LTO x86-64 ELF compilation, identify the
/// ordered IR and record the literal returned by each isolated leaf. The
/// configuration must identify the compiler, target and codegen options.
///
/// Skip the input digest when Previous already has these literals. The
/// ordinary object caches can handle edits that leave them alone.
LLVM_ABI_FOR_TEST std::string prepare(Module &M, StringRef Configuration,
                                      StringRef Previous = {});

/// Bind the proof to the actual object from that compilation. The caller
/// must ensure compilation succeeded with no diagnostics and store the two
/// together in the same trusted object cache.
LLVM_ABI_FOR_TEST std::string enroll(StringRef Proof, StringRef Object);

/// On a match, return an object with the requested leaf literals or an
/// empty string if any part of the object cannot be reused.
LLVM_ABI_FOR_TEST std::string tryReuse(StringRef Proof, StringRef Witness,
                                       StringRef Prior);

} // namespace ohm_leaf
} // namespace llvm

#endif // LLVM_LIB_CODEGEN_OHMLEAFOBJECT_H
