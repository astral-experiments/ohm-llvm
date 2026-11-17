//===- OhmDebugObjectSources.h - Verified compiler source snapshots -*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_OHMDEBUGOBJECTSOURCES_H
#define LLVM_CODEGEN_OHMDEBUGOBJECTSOURCES_H
#include "llvm-c/Types.h"
#include "llvm-c/Visibility.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Consume a live machine-input module, physical cwd, its rustc debug-path
 * remapping, and an existing private immutable-blob directory. OldManifest and
 * OldObject must come from the same authenticated compiler generation. Pass
 * NULL/0 for both on first capture. All three returned buffers begin null and
 * are independently owned; dispose them using LLVMDisposeMemoryBuffer.
 *
 * 0 supplies a complete line map and source proof for this ELF and module.
 * 2 means ordinary codegen. A verified NextManifest may still be returned on
 * a miss and can be attached to the successful new native work product.
 * Reason is optional and valid until the next provider call on this thread.
 */
LLVM_C_ABI int ohm_debug_sources_v1(
    LLVMModuleRef Module, const char *PhysicalCwd, const char *RemappedCwd,
    const char *BlobsDir, const void *OldManifest, size_t OldManifestSize,
    const void *OldObject, size_t OldObjectSize,
    LLVMMemoryBufferRef *NextManifest, LLVMMemoryBufferRef *LineMap,
    LLVMMemoryBufferRef *SourceProof, const char **Reason);

/* Generic cached-ELF transform. All input bytes are borrowed. Returns 0 and
 * one LLVM-owned buffer on success; 2 and a null buffer requests native
 * codegen. Reason has the same lifetime as above. */
LLVM_C_ABI int ohm_debug_rebase_v1(const void *Map, size_t MapSize,
                                   const void *OldObject, size_t OldObjectSize,
                                   LLVMMemoryBufferRef *Output,
                                   const char **Reason);

#ifdef __cplusplus
}
#endif
#endif
