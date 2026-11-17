// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/DwarfLineCertificate.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {
struct Fields {
  std::string Input, Slots, Lines;
};

std::optional<Fields> key(bool Moved, bool Escape = false,
                          bool AddDuplicate = false) {
  LLVMContext Context;
  const char *Tail = Moved ? "\\0C" : "\\0A";
  std::string IR;
  raw_string_ostream Out(IR);
  Out << R"(target triple = "x86_64-unknown-linux-gnu"
target datalayout = "e-m:e-p:64:64"
@file = private unnamed_addr constant [5 x i8] c"f.rs\00", align 1
@loc = private unnamed_addr constant <{ptr, [16 x i8]}> <{ ptr @file, [16 x i8] c"\04\00\00\00\00\00\00\00)"
      << Tail << R"(\00\00\00\03\00\00\00" }>, align 8
@escaping = external global ptr
declare void @_Ropaque(ptr)
)";
  if (AddDuplicate)
    Out << R"(@other_file = private unnamed_addr constant [5 x i8] c"f.rs\00", align 1
@other_loc = private unnamed_addr constant <{ptr, [16 x i8]}> <{ptr @other_file, [16 x i8] c"\04\00\00\00\00\00\00\00\0A\00\00\00\03\00\00\00"}>, align 8
)";
  Out << "define void @call() !dbg !3 {\n";
  if (Escape)
    Out << "  store volatile ptr @loc, ptr @escaping\n";
  if (AddDuplicate)
    Out << "  store volatile ptr @other_loc, ptr @escaping\n";
  Out << R"(  call void @_Ropaque(ptr @loc), !dbg !6
  ret void
}
!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2}
!0 = distinct !DICompileUnit(language: DW_LANG_Rust, file: !1, producer: "test", isOptimized: true, runtimeVersion: 0, emissionKind: LineTablesOnly)
!1 = !DIFile(filename: "f.rs", directory: "/src")
!2 = !{i32 2, !"Debug Info Version", i32 3}
!3 = distinct !DISubprogram(name: "call", scope: !1, file: !1, line: 1, type: !4, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)
!4 = !DISubroutineType(types: !5)
!5 = !{null}
!6 = !DILocation(line: 10, column: 1, scope: !3)
)";
  SMDiagnostic Error;
  auto M = parseAssemblyString(IR, Error, Context);
  EXPECT_NE(M, nullptr);
  if (!M)
    return std::nullopt;
  std::string Data;
  raw_string_ostream Stream(Data);
  if (!writeClosedCallerLineCertificateV1(*M, Stream)) {
    EXPECT_TRUE(Data.empty());
    return std::nullopt;
  }
  StringRef Rest(Data);
  if (!Rest.consume_front("OHMTLC01"))
    return std::nullopt;
  Fields Cert;
  for (auto *Field : {&Cert.Input, &Cert.Slots, &Cert.Lines}) {
    if (Rest.size() < 8)
      return std::nullopt;
    uint64_t N = support::endian::read64le(Rest.data());
    Rest = Rest.drop_front(8);
    if (N > Rest.size())
      return std::nullopt;
    *Field = Rest.take_front(N).str();
    Rest = Rest.drop_front(N);
  }
  EXPECT_TRUE(Rest.empty());
  return Cert;
}

TEST(CallerLineCertificateTest, SeparatesTheCheckedCallerLine) {
  auto A = key(false), B = key(true);
  ASSERT_TRUE(A);
  ASSERT_TRUE(B);
  EXPECT_EQ(A->Input, B->Input);
  EXPECT_EQ(A->Slots, B->Slots);
  EXPECT_EQ(A->Lines, "[10]");
  EXPECT_EQ(B->Lines, "[12]");
}

TEST(CallerLineCertificateTest, RejectsEscapedOrAmbiguousCallerLocation) {
  EXPECT_FALSE(key(false, true));
  EXPECT_FALSE(key(false, false, true));
}
} // namespace
