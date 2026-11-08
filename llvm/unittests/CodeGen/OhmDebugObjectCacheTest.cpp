//===- OhmDebugObjectCacheTest.cpp - Incremental object admission tests ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../../lib/CodeGen/OhmDebugObjectCache.h"
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
