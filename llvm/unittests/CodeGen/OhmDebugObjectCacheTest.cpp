//===- OhmDebugObjectCacheTest.cpp - Incremental object admission tests ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../../lib/CodeGen/OhmDebugObjectCache.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/IRBuilder.h"
#include "gtest/gtest.h"
#if defined(__linux__) && defined(__x86_64__)
#include <dlfcn.h>
#if defined(__GLIBC__)
#include <link.h>
#endif
#endif

using namespace llvm;

#if defined(__linux__) && defined(__x86_64__)
#if defined(__GLIBC__)
TEST(OhmDebugObjectCache, DetectsCompilerImageLoadsAndUnloads) {
  auto Before = ohm::getDebugObjectImageGeneration();
  ASSERT_TRUE(Before);
  void *Library = ::dlmopen(LM_ID_NEWLM, "libm.so.6", RTLD_NOW | RTLD_LOCAL);
  ASSERT_NE(Library, nullptr);
  auto Loaded = ohm::getDebugObjectImageGeneration();
  EXPECT_EQ(::dlclose(Library), 0);
  EXPECT_NE(Before, Loaded);
  EXPECT_NE(Loaded, ohm::getDebugObjectImageGeneration());
  EXPECT_NE(Before, ohm::getDebugObjectImageGeneration());
}
#endif

TEST(OhmDebugObjectCache, IncludesOrdinaryLinkedLibraries) {
  Dl_info Lib{};
  ASSERT_NE(::dladdr(::dlsym(RTLD_DEFAULT, "dlopen"), &Lib), 0);
  ASSERT_NE(Lib.dli_fname, nullptr);
  auto Images = ohm::getDebugObjectImages();
  ASSERT_FALSE(Images.empty());
  EXPECT_TRUE(Images.front().Path.empty());
  EXPECT_TRUE(llvm::any_of(
      Images, [&](const auto &I) { return I.Path == Lib.dli_fname; }));
}
#endif

TEST(OhmDebugObjectCache, OriginalLoaderEnvironment) {
  EXPECT_TRUE(ohm::allowsDebugObjectCacheLoader(
      StringLiteral::withInnerNUL("PATH=/bin\0LD_PRELOAD=\0LD_AUDIT=\0")));
  EXPECT_TRUE(ohm::allowsDebugObjectCacheLoader(
      StringLiteral::withInnerNUL("X=LD_PRELOAD=lib.so\0LD_AUDITOR=lib.so\0")));
  EXPECT_FALSE(ohm::allowsDebugObjectCacheLoader(StringRef()));
  EXPECT_FALSE(ohm::allowsDebugObjectCacheLoader(StringLiteral("PATH=/bin")));
  EXPECT_FALSE(ohm::allowsDebugObjectCacheLoader(
      StringLiteral::withInnerNUL("PATH=/bin\0LD_PRELOAD=lib.so\0")));
  EXPECT_FALSE(ohm::allowsDebugObjectCacheLoader(
      StringLiteral::withInnerNUL("LD_AUDIT=lib.so\0PATH=/bin\0")));
  EXPECT_FALSE(ohm::allowsDebugObjectCacheLoader(
      StringLiteral::withInnerNUL("LD_PRELOAD=\0LD_PRELOAD=lib.so\0")));
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
