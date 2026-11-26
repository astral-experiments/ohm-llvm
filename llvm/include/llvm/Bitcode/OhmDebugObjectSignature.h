// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LLVM_BITCODE_OHMDEBUGOBJECTSIGNATURE_H
#define LLVM_BITCODE_OHMDEBUGOBJECTSIGNATURE_H
#include "llvm-c/Types.h"
#include "llvm/Support/Compiler.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
// Save the ordered, original bitcode of the actual live emission module.
// The caller owns successful output and releases it with ohm_bc_free_v1.
LLVM_ABI int ohm_bc_save_v1(LLVMModuleRef, char **data, size_t *length);
// Return one only for complete equality after reversing verified source moves.
// The caller checks the target, options, eligibility, and prior object
// identity.
LLVM_ABI int ohm_bc_match_v1(LLVMModuleRef, const char *origin,
                             size_t origin_length, const char *mapping,
                             size_t mapping_length, const char *proof,
                             size_t proof_length);
// The caller checks the proposed named records against its authenticated old
// object. Return two only after a checked runtime projection matches the full
// bitcode, one for an ordinary debug-only match, or zero on any mismatch.
typedef int (*OhmCheckRuntimeLocations)(void *context, const char *records,
                                        size_t records_length);
LLVM_ABI int ohm_bc_match_relocatable_v1(
    LLVMModuleRef, const char *origin, size_t origin_length,
    const char *mapping, size_t mapping_length, const char *proof,
    size_t proof_length, OhmCheckRuntimeLocations check, void *context);
LLVM_ABI void ohm_bc_free_v1(void *);
#ifdef __cplusplus
}
#endif

#endif
