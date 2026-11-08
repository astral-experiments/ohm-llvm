//===- OhmDebugObjectSourcesTest.cpp --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/OhmDebugObjectSources.h"
#include "llvm-c/Core.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CachePruning.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"
#ifdef __linux__
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace llvm;
namespace {
struct Result {
  int Status;
  std::string Manifest, Map, Proof, Reason;
};
class OhmDebugObjectSources : public testing::Test {
protected:
  SmallString<128> Root, Bank, Source;
  void SetUp() override {
#if !defined(__linux__) || !defined(SYS_renameat2)
    GTEST_SKIP() << "Requires the Linux source-store implementation";
#endif
    ASSERT_FALSE(sys::fs::createUniqueDirectory("ohm-dbg-source", Root));
    Bank = Root;
    sys::path::append(Bank, "blobs");
    ASSERT_FALSE(sys::fs::create_directory(Bank));
    ASSERT_FALSE(sys::fs::setPermissions(Bank, sys::fs::owner_all));
    Source = Root;
    sys::path::append(Source, "lib.rs");
  }
  void TearDown() override {
    if (!Root.empty())
      EXPECT_FALSE(sys::fs::remove_directories(Root));
  }
  void write(StringRef B) {
    std::error_code EC;
    raw_fd_ostream S(Source, EC);
    ASSERT_FALSE(EC);
    S << B;
    S.close();
    ASSERT_FALSE(S.has_error());
  }
  Result run(StringRef Expected, StringRef Prior = {},
             StringRef Name = "lib.rs",
             std::optional<StringRef> External = std::nullopt) {
    LLVMContext C;
    Module M("module", C);
    auto *Files = M.getOrInsertNamedMetadata("test.files");
    auto Add = [&](StringRef Filename, StringRef Directory, StringRef B) {
      std::string H = toHex(MD5::hash(arrayRefFromStringRef(B)), true);
      auto Check = DIFile::ChecksumInfo<StringRef>(DIFile::CSK_MD5, H);
      Files->addOperand(DIFile::get(C, Filename, Directory, Check));
    };
    Add(Name, "/workspace", Expected);
    if (External)
      Add("/unavailable-sdk/library.rs", "", *External);
    LLVMMemoryBufferRef N = nullptr, A = nullptr, B = nullptr;
    const char *Why = nullptr;
    // An unambiguous mapping needs no object walk. The separate ELF API is
    // responsible for accepting the object when the caller applies this map.
    int Status =
        ohm_debug_sources_v1(wrap(&M), Root.c_str(), "/workspace", Bank.c_str(),
                             Prior.empty() ? nullptr : Prior.data(),
                             Prior.size(), Prior.empty() ? nullptr : "object",
                             Prior.empty() ? 0 : 6, &N, &A, &B, &Why);
    auto Take = [](LLVMMemoryBufferRef P) -> std::string {
      if (!P)
        return {};
      std::string Bytes(LLVMGetBufferStart(P), LLVMGetBufferSize(P));
      LLVMDisposeMemoryBuffer(P);
      return Bytes;
    };
    return {Status, Take(N), Take(A), Take(B), Why ? Why : ""};
  }
};
TEST_F(OhmDebugObjectSources, CapturesOriginAndProvesExactOrderedLines) {
  StringRef Old = "first\nsecond\n", New = "first\ninserted\nsecond\n";
  write(Old);
  auto O = run(Old);
  ASSERT_EQ(O.Status, 2);
  ASSERT_FALSE(O.Manifest.empty());
  auto Identity = run(Old, O.Manifest);
  EXPECT_EQ(Identity.Status, 2);
  EXPECT_EQ(Identity.Manifest, O.Manifest);
  write(New);
  auto N = run(New, O.Manifest);
  ASSERT_EQ(N.Status, 0) << N.Reason;
  EXPECT_EQ(
      N.Map,
      R"({"version":1,"files":[{"directory":"/workspace","filename":"lib.rs","lines":[[2,3]]}]})"
      "\n");
  EXPECT_NE(
      N.Proof.find(toHex(SHA256::hash(arrayRefFromStringRef(N.Map)), true)),
      std::string::npos);
  write("different\nsecond\n");
  auto M = run("different\nsecond\n", O.Manifest);
  EXPECT_EQ(M.Status, 2);
  EXPECT_FALSE(M.Manifest.empty());
  EXPECT_TRUE(M.Map.empty());
}
TEST_F(OhmDebugObjectSources, MissingPrunedCorruptAndTraversingInputsMiss) {
  StringRef Old = "first\nsecond\n", New = "added\nfirst\nsecond\n";
  write(Old);
  auto O = run(Old);
  ASSERT_FALSE(O.Manifest.empty());
  auto Mismatch = run(New);
  EXPECT_EQ(Mismatch.Status, 2);
  EXPECT_TRUE(Mismatch.Manifest.empty());
  auto Escape = run(Old, {}, "../lib.rs");
  EXPECT_EQ(Escape.Status, 2);
  EXPECT_TRUE(Escape.Manifest.empty());
  write(New);
  auto BadPrior = run(New, "[broken");
  EXPECT_EQ(BadPrior.Status, 2);
  EXPECT_FALSE(BadPrior.Manifest.empty());
  SmallString<128> Blob(Bank);
  sys::path::append(Blob,
                    "llvmcache-" +
                        toHex(SHA256::hash(arrayRefFromStringRef(Old)), true));
  ASSERT_FALSE(sys::fs::remove(Blob));
  auto Gone = run(New, O.Manifest);
  EXPECT_EQ(Gone.Status, 2);
  EXPECT_FALSE(Gone.Manifest.empty());
  EXPECT_TRUE(Gone.Map.empty());
}
TEST_F(OhmDebugObjectSources,
       ExternalIdentityMustMatchWithoutReadingOutsideWorkspace) {
  StringRef Old = "body\n", New = "inserted\nbody\n";
  write(Old);
  auto O = run(Old, {}, "lib.rs", "old external");
  ASSERT_FALSE(O.Manifest.empty());
  write(New);
  auto Same = run(New, O.Manifest, "lib.rs", "old external");
  EXPECT_EQ(Same.Status, 0) << Same.Reason;
  auto Edited = run(New, O.Manifest, "lib.rs", "new external");
  EXPECT_EQ(Edited.Status, 2);
  EXPECT_FALSE(Edited.Manifest.empty());
  EXPECT_TRUE(Edited.Map.empty());
  auto Missing = run(New, O.Manifest);
  EXPECT_EQ(Missing.Status, 2);
  EXPECT_TRUE(Missing.Map.empty());
}
#if defined(__linux__) && defined(SYS_renameat2) &&                            \
    defined(GTEST_HAS_DEATH_TEST)
TEST_F(OhmDebugObjectSources, PrunesInterruptedSourceBlob) {
  StringRef Contents = "first\nsecond\n";
  write(Contents);
  // The first write reaches the one-byte limit; the next write terminates
  // the child before RAII cleanup. No partial blob is published.
  ASSERT_EXIT(
      {
        struct rlimit Core {};
        struct rlimit Size {};
        Size.rlim_cur = Size.rlim_max = 1;
        sigset_t Signals;
        sigemptyset(&Signals);
        sigaddset(&Signals, SIGXFSZ);
        if (setrlimit(RLIMIT_CORE, &Core) || setrlimit(RLIMIT_FSIZE, &Size) ||
            signal(SIGXFSZ, SIG_DFL) == SIG_ERR ||
            sigprocmask(SIG_UNBLOCK, &Signals, nullptr))
          _exit(91);
        (void)run(Contents);
        _exit(92);
      },
      ::testing::KilledBySignal(SIGXFSZ), "");

  std::error_code EC;
  sys::fs::directory_iterator It(Bank, EC), End;
  ASSERT_FALSE(EC);
  ASSERT_NE(It, End);
  std::string Incomplete = It->path();
  It.increment(EC);
  ASSERT_FALSE(EC);
  EXPECT_EQ(It, End);
  uint64_t Length;
  ASSERT_FALSE(sys::fs::file_size(Incomplete, Length));
  ASSERT_EQ(Length, 1u);

  struct timespec Expired[2] = {{1, 0}, {1, 0}};
  ASSERT_EQ(utimensat(AT_FDCWD, Incomplete.c_str(), Expired, 0), 0);
  CachePruningPolicy Policy;
  Policy.Interval = std::chrono::seconds(0);
  Policy.Expiration = std::chrono::seconds(1);
  auto Pruned = pruneCache(Bank, Policy);
  ASSERT_TRUE(static_cast<bool>(Pruned)) << toString(Pruned.takeError());
  EXPECT_TRUE(*Pruned);
  EXPECT_FALSE(sys::fs::exists(Incomplete));
  EXPECT_FALSE(run(Contents).Manifest.empty());
}
#endif
} // namespace
