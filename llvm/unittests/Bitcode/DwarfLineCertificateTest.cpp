//===- DwarfLineCertificateTest.cpp - Check live optimizer inputs ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Bitcode/DwarfLineCertificate.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitcode/ClosedModuleHash.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/GlobalIFunc.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/OptBisect.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {
class DwarfLineCertificateTest : public testing::Test {
protected:
  LLVMContext C;
  std::unique_ptr<Module> M;

  void SetUp() override {
    SMDiagnostic Error;
    M = parseAssemblyString(R"(
      target triple = "x86_64-unknown-linux-gnu"
      @storage = private constant [4 x i8] c"test"
      @pointer = global ptr @storage
      define i32 @run(i1 %choice) !dbg !5 {
        br i1 %choice, label %a, label %b, !dbg !7
      a: ret i32 1, !dbg !7
      b: ret i32 2, !dbg !8
      }
      !llvm.dbg.cu = !{!0}
      !llvm.module.flags = !{!9}
      !0 = distinct !DICompileUnit(language: DW_LANG_Rust, file: !1, producer: "test", isOptimized: true, runtimeVersion: 0, emissionKind: LineTablesOnly)
      !1 = !DIFile(filename: "crate.rs", directory: "/src")
      !3 = !DISubroutineType(types: !4)
      !4 = !{}
      !5 = distinct !DISubprogram(name: "run", scope: !1, file: !1, line: 1, type: !3, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)
      !7 = !DILocation(line: 3, column: 1, scope: !5)
      !8 = !DILocation(line: 4, column: 1, scope: !5)
      !9 = !{i32 2, !"Debug Info Version", i32 3}
    )", Error, C);
    ASSERT_NE(M, nullptr);
    ASSERT_TRUE(certificate());
  }

  std::optional<std::string> certificate() {
    std::string Bytes;
    raw_string_ostream Out(Bytes);
    if (writeClosedDwarfLineCertificateV1(*M, Out))
      return Bytes;
    EXPECT_TRUE(Bytes.empty());
    return std::nullopt;
  }

  void addSlice(Constant *Address) {
    (void)ConstantStruct::getAnon(
        {Address, PoisonValue::get(Type::getInt64Ty(C))});
  }
};

TEST_F(DwarfLineCertificateTest, IncludesTerminalStorageSlice) {
  auto Initial = certificate();
  addSlice(M->getNamedGlobal("storage"));
  auto WithSlice = certificate();
  ASSERT_TRUE(WithSlice);
  EXPECT_NE(Initial, WithSlice);
  M->getNamedGlobal("storage")->reverseUseList();
  auto OtherPosition = certificate();
  ASSERT_TRUE(OtherPosition);
  EXPECT_NE(OtherPosition, WithSlice);
}

TEST_F(DwarfLineCertificateTest, ExactInputIncludesLocationsIdentifierAndLiveUses) {
  std::array<uint8_t, 32> Initial, Other;
  ASSERT_TRUE(getClosedModuleHashV1(*M, Initial));
  ASSERT_TRUE(getClosedModuleHashV1(*M, Other));
  EXPECT_EQ(Initial, Other);

  auto OriginalID = M->getModuleIdentifier();
  M->setModuleIdentifier("other-input");
  ASSERT_TRUE(getClosedModuleHashV1(*M, Other));
  EXPECT_NE(Initial, Other);
  M->setModuleIdentifier(OriginalID);
  ASSERT_TRUE(getClosedModuleHashV1(*M, Other));
  EXPECT_EQ(Initial, Other);

  auto *F = M->getFunction("run");
  Instruction &I = *F->getEntryBlock().getTerminator();
  I.setDebugLoc(DILocation::get(C, 7, 1, F->getSubprogram()));
  ASSERT_TRUE(getClosedModuleHashV1(*M, Other));
  EXPECT_NE(Initial, Other);

  addSlice(F);
  EXPECT_FALSE(getClosedModuleHashV1(*M, Other));
}

TEST_F(DwarfLineCertificateTest, ExactHashIncludesUsesAbsentFromOrderedBitcode) {
  auto Bitcode = [&]() {
    std::string Bytes;
    raw_string_ostream OS(Bytes);
    WriteBitcodeToFile(*M, OS, /*ShouldPreserveUseListOrder=*/true);
    return Bytes;
  };
  auto Original = Bitcode();
  std::array<uint8_t, 32> Before, After;
  ASSERT_TRUE(getClosedModuleHashV1(*M, Before));
  addSlice(M->getNamedGlobal("storage"));
  EXPECT_EQ(Original, Bitcode());
  ASSERT_TRUE(getClosedModuleHashV1(*M, After));
  EXPECT_NE(Before, After);
}

TEST_F(DwarfLineCertificateTest, RejectsUnrepresentedFunctionUser) {
  addSlice(M->getFunction("run"));
  EXPECT_FALSE(certificate());
}

TEST_F(DwarfLineCertificateTest, RequiresLexicalBlockFileToOwnNestedLines) {
  Function *F = M->getFunction("run");
  auto *SP = F->getSubprogram();
  auto *MacroFile = DIFile::get(C, "macro.rs", "/src");
  auto *Wrapper = DILexicalBlockFile::get(C, SP, MacroFile, 0);
  auto &Term = *F->getEntryBlock().getTerminator();
  auto *SameFile = DILexicalBlock::get(C, Wrapper, MacroFile, 10, 1);
  Term.setDebugLoc(DILocation::get(C, 10, 1, SameFile));
  EXPECT_TRUE(certificate());

  auto *Inner = DILexicalBlock::get(C, SameFile, SP->getFile(), 10, 1);
  Term.setDebugLoc(DILocation::get(C, 10, 1, Inner));
  EXPECT_FALSE(certificate());
}

TEST_F(DwarfLineCertificateTest, RejectsUnrepresentedBlockAddress) {
  auto *F = M->getFunction("run");
  (void)BlockAddress::get(F, &F->back());
  EXPECT_FALSE(certificate());
}

TEST_F(DwarfLineCertificateTest, RejectsDetachedConstantGraph) {
  auto *Address = ConstantExpr::getIntToPtr(
      ConstantInt::get(Type::getInt64Ty(C), 127), PointerType::getUnqual(C));
  addSlice(Address);
  EXPECT_FALSE(certificate());
}

TEST_F(DwarfLineCertificateTest, RejectsHiddenMetadataWrapper) {
  (void)ValueAsMetadata::get(M->getNamedGlobal("storage"));
  EXPECT_FALSE(certificate());
}

TEST_F(DwarfLineCertificateTest, RejectsDetachedScalarMetadataWrapper) {
  (void)ValueAsMetadata::get(ConstantInt::get(Type::getInt64Ty(C), 217));
  EXPECT_FALSE(certificate());
}

TEST_F(DwarfLineCertificateTest, IncludesTrackingOrderWithinMetadataOwner) {
  auto *Value = ValueAsMetadata::get(M->getNamedGlobal("storage"));
  auto *N = MDNode::get(C, {Value, Value});
  M->getOrInsertNamedMetadata("test.tracking.order")->addOperand(N);
  auto Initial = certificate();
  ASSERT_TRUE(Initial);
  // Replacing a ConstantAsMetadata with null makes this a distinct node.
  // Keep the uniqued owner so changing the slot only changes callback order.
  N->replaceOperandWith(0, MDString::get(C, "temporary operand"));
  N->replaceOperandWith(0, Value);
  auto OtherPosition = certificate();
  ASSERT_TRUE(OtherPosition);
  EXPECT_NE(Initial, OtherPosition);
}

TEST_F(DwarfLineCertificateTest, IncludesOnlyTheDisabledDefaultPassGate) {
  auto Initial = certificate();
  (void)C.getOptPassGate();
  EXPECT_EQ(certificate(), Initial);
  OptPassGate Other;
  C.setOptPassGate(Other);
  EXPECT_FALSE(certificate());
}

TEST_F(DwarfLineCertificateTest, RejectsSecondModuleInSameContext) {
  Module Other("other", C);
  EXPECT_FALSE(certificate());
}

TEST(DwarfLineCertificateIFuncTest, SharedChecksumRemainsLiteral) {
  auto Read = [](StringRef Checksum, std::array<uint8_t, 32> &Raw,
                 std::string &Normalized) {
    LLVMContext Context;
    SMDiagnostic Error;
    std::string IR = R"(
      target triple = "x86_64-unknown-linux-gnu"
      @redirect = ifunc i32 (), ptr @resolve
      define ptr @resolve() { ret ptr @run }
      define i32 @run() !dbg !3 { ret i32 0, !dbg !4 }
      !llvm.dbg.cu = !{!0}
      !llvm.module.flags = !{!2}
      !0 = distinct !DICompileUnit(language: DW_LANG_Rust, file: !1, producer: "test", isOptimized: true, runtimeVersion: 0, emissionKind: LineTablesOnly)
      !1 = !DIFile(filename: "a.rs", directory: "/src", checksumkind: CSK_MD5, checksum: "CHECKSUM")
      !2 = !{i32 2, !"Debug Info Version", i32 3}
      !3 = distinct !DISubprogram(name: "run", scope: !1, file: !1, line: 1, type: !5, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)
      !4 = !DILocation(line: 1, column: 1, scope: !3)
      !5 = !DISubroutineType(types: !6)
      !6 = !{}
    )";
    IR.replace(IR.find("CHECKSUM"), 8, Checksum);
    auto M = parseAssemblyString(IR, Error, Context);
    ASSERT_TRUE(M);
    M->getNamedIFunc("redirect")
        ->setMetadata("also.uses.checksum",
                      MDNode::get(Context, MDString::get(Context, Checksum)));
    ASSERT_TRUE(getClosedModuleHashV1(*M, Raw));
    std::string Bytes;
    raw_string_ostream Out(Bytes);
    ASSERT_TRUE(writeClosedDwarfLineCertificateV1(*M, Out));
    // The compilation directory "/src" is a length-prefixed string immediately
    // followed by the normalized bitcode digest.
    ASSERT_GT(Bytes.size(), 8u + Raw.size());
    Normalized = Bytes.substr(8, Raw.size());
  };
  std::array<uint8_t, 32> RawA, RawB;
  std::string NormalizedA, NormalizedB;
  Read(std::string(32, 'a'), RawA, NormalizedA);
  Read(std::string(32, 'b'), RawB, NormalizedB);
  EXPECT_NE(RawA, RawB);
  EXPECT_NE(NormalizedA, NormalizedB);
}
} // namespace
