//===- OhmEarlyObjectCache.h - Checked object-only compiler work items ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CODEGEN_OHMEARLYOBJECTCACHE_H
#define LLVM_CODEGEN_OHMEARLYOBJECTCACHE_H
#include "llvm-c/TargetMachine.h"
#include "llvm/Support/Compiler.h"

#ifdef __cplusplus
extern "C" {
#endif
/// For a regular object-only work item, try to atomically materialize the
/// previously checked native object at Output. On a miss the caller retains
/// its Module and may receive an origin ticket. Finish or abandon that ticket
/// before destroying the Module's context. Learning may cross threads, but in
/// that case the late emitter is allowed to decline the association.
LLVM_ABI int ohm_early_object_begin_v1(LLVMModuleRef M, LLVMTargetMachineRef TM,
                                       const char *Output, size_t Length,
                                       void **Ticket);
/// Completed is true only after ordinary code generation succeeded. Both
/// outcomes release the ticket, including its context diagnostic observation.
LLVM_ABI void ohm_early_object_end_v1(void *Ticket, int Completed);
#ifdef __cplusplus
}
#endif
#endif
