//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_OHMLINECERTIFICATE_H
#define LLVM_CODEGEN_OHMLINECERTIFICATE_H
#include "llvm-c/Types.h"
#include "llvm-c/Visibility.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Begin and finish at the actual synchronous machine-emission boundary.
   An origin and its object share one compiler thread. The returned buffer
   belongs to the caller, which releases it via LLVMDisposeMemoryBuffer. */
LLVM_C_ABI void ohm_line_certificate_begin_v1(void);
LLVM_C_ABI LLVMMemoryBufferRef ohm_line_certificate_end_v1(void);
LLVM_C_ABI int ohm_line_certificate_check_v1(const void *certificate,
                                             size_t length,
                                             const char *map_json,
                                             size_t map_length);
/* Private DwarfDebug recorder callbacks. DIFile and Module are opaque here. */
LLVM_C_ABI void ohm_line_record_module_v1(const void *module);
LLVM_C_ABI void ohm_line_record_end_module_v1(void);
LLVM_C_ABI void ohm_line_record_file_v1(unsigned dwarf_number,
                                        const void *di_file);
LLVM_C_ABI void ohm_line_record_pair_v1(const void *first_file,
                                        unsigned first_line,
                                        const void *second_file,
                                        unsigned second_line,
                                        unsigned second_dwarf_file);
#ifdef __cplusplus
}
#endif
#endif
