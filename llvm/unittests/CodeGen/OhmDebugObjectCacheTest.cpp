//===- OhmDebugObjectCacheTest.cpp - Incremental object admission tests ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../../lib/CodeGen/OhmDebugObjectCache.h"
#include "llvm/IR/IRBuilder.h"
#include "gtest/gtest.h"

using namespace llvm;

TEST(OhmDebugObjectCache, OriginalLoaderEnvironment) {
  EXPECT_TRUE(ohm::allowsDebugObjectCacheLoader(
      StringLiteral("PATH=/bin\0LD_PRELOAD=\0LD_AUDIT=\0")));
  EXPECT_TRUE(ohm::allowsDebugObjectCacheLoader(
      StringLiteral("X=LD_PRELOAD=lib.so\0LD_AUDITOR=lib.so\0")));
  EXPECT_FALSE(ohm::allowsDebugObjectCacheLoader(StringRef()));
  EXPECT_FALSE(ohm::allowsDebugObjectCacheLoader(StringLiteral("PATH=/bin")));
  EXPECT_FALSE(ohm::allowsDebugObjectCacheLoader(
      StringLiteral("PATH=/bin\0LD_PRELOAD=lib.so\0")));
  EXPECT_FALSE(ohm::allowsDebugObjectCacheLoader(
      StringLiteral("LD_AUDIT=lib.so\0PATH=/bin\0")));
  EXPECT_FALSE(ohm::allowsDebugObjectCacheLoader(
      StringLiteral("LD_PRELOAD=\0LD_PRELOAD=lib.so\0")));
}

TEST(OhmDebugObjectCache, AdmissionCountsTheOptimizedModule) {
  LLVMContext C;
  Module M("machine-input", C);
  auto *Ty = FunctionType::get(Type::getVoidTy(C), false);
  auto *F = Function::Create(Ty, GlobalValue::ExternalLinkage, "body", M);
  auto Callee = M.getOrInsertFunction("opaque", Ty);
  IRBuilder<> B(BasicBlock::Create(C, "entry", F));
  auto *Ret = B.CreateRetVoid();
  B.SetInsertPoint(Ret);
  for (unsigned I = 1; I < 32767; ++I)
    B.CreateCall(Callee);
  EXPECT_FALSE(ohm::isLargeDebugObjectInput(M));
  B.CreateCall(Callee);
  EXPECT_TRUE(ohm::isLargeDebugObjectInput(M));
}
