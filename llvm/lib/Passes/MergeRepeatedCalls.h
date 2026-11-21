//===- MergeRepeatedCalls.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_PASSES_MERGEREPEATEDCALLS_H
#define LLVM_LIB_PASSES_MERGEREPEATEDCALLS_H

#include "llvm/IR/PassManager.h"

namespace llvm {
struct SimplifyRustMacroHelpersPass
    : OptionalPassInfoMixin<SimplifyRustMacroHelpersPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

struct InlineSmallRustFunctionsPass
    : OptionalPassInfoMixin<InlineSmallRustFunctionsPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

struct MergeIdenticalRustFunctionsPass
    : OptionalPassInfoMixin<MergeIdenticalRustFunctionsPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

struct MergeRepeatedCallsPass : OptionalPassInfoMixin<MergeRepeatedCallsPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

struct RustJumpThreadingPass : OptionalPassInfoMixin<RustJumpThreadingPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};
} // namespace llvm

#endif
