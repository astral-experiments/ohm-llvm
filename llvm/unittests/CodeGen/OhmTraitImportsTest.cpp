//===- OhmTraitImportsTest.cpp - Trait import cache tests -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../../lib/CodeGen/OhmTraitImports.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {
using Bytes = SmallVector<char, 0>;
constexpr StringLiteral
    Name("_RNvYNtCs1_5types4ItemNtCs2_6traits5Trait6methodCs3_3old");
constexpr StringLiteral
    Longer("_RNvYNtCs1_5types4ItemNtCs2_6traits5Trait6methodCs4_8newcrate");
constexpr StringLiteral
    Other("_RNvYNtCs1_5types4ItemNtCs2_6traits5Trait6secondCs4_8newcrate");

static std::string withLines(StringRef Provider, unsigned Line, char Checksum) {
  return ("target triple = \"x86_64-unknown-linux-gnu\"\n"
          "declare i32 @" +
          Provider +
          "()\n"
          "define i32 @caller() !dbg !4 {\n"
          "  %v = call i32 @" +
          Provider +
          "(), !dbg !7\n"
          "  ret i32 %v, !dbg !8\n}\n"
          "!llvm.dbg.cu = !{!0}\n"
          "!llvm.module.flags = !{!1, !2}\n"
          "!0 = distinct !DICompileUnit(language: DW_LANG_Rust, file: !3, "
          "producer: \"rustc\", isOptimized: true, runtimeVersion: 0, "
          "emissionKind: LineTablesOnly)\n"
          "!1 = !{i32 7, !\"Dwarf Version\", i32 4}\n"
          "!2 = !{i32 2, !\"Debug Info Version\", i32 3}\n"
          "!3 = !DIFile(filename: \"lib.rs\", directory: \"/test\", "
          "checksumkind: CSK_MD5, checksum: \"" +
          std::string(32, Checksum) +
          "\")\n"
          "!4 = distinct !DISubprogram(name: \"caller\", scope: !3, file: !3, "
          "line: 1, type: !5, scopeLine: 1, spFlags: DISPFlagDefinition | "
          "DISPFlagOptimized, unit: !0)\n"
          "!5 = !DISubroutineType(types: !6)\n!6 = !{}\n"
          "!7 = !DILocation(line: " +
          Twine(Line) +
          ", column: 1, scope: !4)\n"
          "!8 = !DILocation(line: " +
          Twine(Line + 1) + ", column: 1, scope: !4)\n")
      .str();
}

static std::string source(StringRef Import, StringRef Literal = Name) {
  // The symbol @old and section "ld" share suffixes with the provider.
  return (Twine(withLines(Import, 80, 'a')) +
          "@old = constant i32 1\n"
          "@literal = constant [" +
          Twine(Literal.size()) + " x i8] c\"" + Literal +
          "\", section \"ld\"\n")
      .str();
}
static std::unique_ptr<Module> read(LLVMContext &C, StringRef IR) {
  SMDiagnostic Error;
  auto M = parseAssemblyString(IR, Error, C);
  EXPECT_TRUE(M) << Error.getMessage().str();
  return M;
}

static bool signature(StringRef IR, Bytes &Input) {
  LLVMContext C;
  auto M = read(C, IR);
  Bytes Names;
  return M && ohm::writeTraitImportSignature(*M, Input, Names);
}

TEST(OhmTraitImports, KeepsLiteralAndMethodInKey) {
  LLVMContext C;
  auto M = read(C, source(Name));
  ASSERT_TRUE(M);
  Bytes Base, Names, Current, ChangedLiteral, ChangedMethod;
  ASSERT_TRUE(ohm::writeTraitImportSignature(*M, Base, Names));
  EXPECT_TRUE(M->getFunction(Name));
  EXPECT_TRUE(StringRef(Names.data(), Names.size()).drop_back().ends_with(Name));
  ASSERT_TRUE(signature(source(Longer), Current));
  EXPECT_EQ(Base, Current);
  ASSERT_TRUE(signature(source(Longer, Longer), ChangedLiteral));
  EXPECT_NE(Base, ChangedLiteral);
  ASSERT_TRUE(signature(source(Other), ChangedMethod));
  EXPECT_NE(Base, ChangedMethod);
}

TEST(OhmTraitImports, SkipsSerializationForUnchangedImports) {
  LLVMContext C;
  auto M = read(C, source(Name));
  ASSERT_TRUE(M);
  Bytes Input, Names;
  bool Checked = false;
  auto ShouldProbe = [&](StringRef Imports) {
    Checked = true;
    EXPECT_EQ(Imports.drop_back(), Name);
    EXPECT_TRUE(Imports.ends_with(StringRef("\0", 1)));
    return false;
  };
  EXPECT_FALSE(ohm::writeTraitImportSignature(*M, Input, Names, ShouldProbe));
  EXPECT_TRUE(Checked);
  EXPECT_TRUE(Input.empty());
}

TEST(OhmTraitImports, ChecksAssemblerNameAliasesInBothGenerations) {
  auto withAlias = [](StringRef Provider, StringRef Raw) {
    return (source(Provider) + "declare i32 @\"\\01" + Raw + "\"()\n" +
            "define i32 @other() { %v = call i32 @\"\\01" + Raw +
            "\"() ret i32 %v }\n")
        .str();
  };
  Bytes Key;
  // A second call has an explicit assembler name. Rebinding its shared
  // relocation in the donor or merging it with the new provider is ambiguous.
  EXPECT_FALSE(signature(withAlias(Name, Name), Key));
  EXPECT_TRUE(signature(withAlias(Longer, Name), Key));
  EXPECT_TRUE(signature(withAlias(Name, Longer), Key));
  EXPECT_FALSE(signature(withAlias(Longer, Longer), Key));
}

TEST(OhmTraitImports, RequiresUnambiguousDirectImport) {
  Bytes B;
  EXPECT_FALSE(signature(source("ordinary_function"), B));
  EXPECT_FALSE(signature(source("_RNvYNtCs1_5types4ItemCs3_3old"), B));
  EXPECT_FALSE(signature(
      source(Name) + ("@ptr = constant ptr @" + Name + "\n").str(), B));
  EXPECT_FALSE(signature(source(Name) + "module asm \"nop\"\n", B));
  EXPECT_FALSE(signature(source(Name) + "define void @f() { call void asm "
                                        "sideeffect \"\", \"\"() ret void }\n",
                         B));
  EXPECT_FALSE(signature(source(Name) + ("declare i32 @" + Longer +
                                         "()\n"
                                         "define i32 @f() { %v = call i32 @" +
                                         Longer + "() ret i32 %v }\n")
                                            .str(),
                         B));
}

#if defined(__linux__) && defined(__x86_64__) && defined(LLVM_NATIVE_TARGET)
static bool emit(StringRef IR, Bytes &Object) {
  if (InitializeNativeTarget() || InitializeNativeTargetAsmPrinter())
    return false;
  LLVMContext C;
  auto M = read(C, IR);
  if (!M)
    return false;
  std::string Error;
  auto *Target = TargetRegistry::lookupTarget(M->getTargetTriple(), Error);
  if (!Target)
    return false;
  std::unique_ptr<TargetMachine> TM(Target->createTargetMachine(
      M->getTargetTriple(), "x86-64", "", TargetOptions(), Reloc::PIC_));
  if (!TM)
    return false;
  M->setDataLayout(TM->createDataLayout());
  raw_svector_ostream Stream(Object);
  legacy::PassManager PM;
  if (TM->addPassesToEmitFile(PM, Stream, nullptr, CodeGenFileType::ObjectFile))
    return false;
  PM.run(*M);
  return true;
}

static bool snapshot(StringRef IR, Bytes &Key, Bytes &Manifest) {
  LLVMContext Context;
  auto M = read(Context, IR);
  return M && ohm::writeTraitImportSignature(*M, Key, Manifest);
}
static StringRef bytes(ArrayRef<char> Data) {
  return StringRef(Data.data(), Data.size());
}
static bool rebind(ArrayRef<char> Input, StringLiteral Old,
                   StringLiteral Current, Bytes &Out) {
  Bytes Key, Manifest, NewKey, NewManifest;
  return snapshot(source(Old), Key, Manifest) &&
         snapshot(source(Current), NewKey, NewManifest) &&
         ohm::rebindTraitImports(bytes(Input), bytes(Manifest),
                                 bytes(NewManifest), Out);
}

TEST(OhmTraitImports, RebindsAfterInterveningLineEdits) {
  Bytes Before, Fresh, Rebound, Key, NewKey, Manifest, NewManifest;
  auto Old = withLines(Name, 80, 'a');
  auto New = withLines(Longer, 82, 'b');
  ASSERT_TRUE(emit(Old, Before));
  ASSERT_TRUE(emit(New, Fresh));
  ASSERT_TRUE(snapshot(Old, Key, Manifest));
  ASSERT_TRUE(snapshot(New, NewKey, NewManifest));
  ASSERT_EQ(Key, NewKey);
  ASSERT_TRUE(ohm::rebindTraitImports(bytes(Before), bytes(Manifest),
                                      bytes(NewManifest), Rebound));
  EXPECT_EQ(Fresh, Rebound);
  EXPECT_FALSE(ohm::rebindTraitImports(bytes(Before), bytes(Manifest),
                                       bytes(NewManifest).take_front(15),
                                       Rebound));
  Bytes SameKey, SameChecksum;
  ASSERT_TRUE(snapshot(withLines(Longer, 82, 'a'), SameKey, SameChecksum));
  EXPECT_FALSE(ohm::rebindTraitImports(bytes(Before), bytes(Manifest),
                                       bytes(SameChecksum), Rebound));
}
TEST(OhmTraitImports, MatchesFreshELFWhenProviderGrowsAndShrinks) {
  Bytes Before, After, Rebound;
  ASSERT_TRUE(emit(source(Name), Before));
  ASSERT_TRUE(emit(source(Longer), After));
  ASSERT_TRUE(rebind(Before, Name, Longer, Rebound));
  EXPECT_EQ(Rebound, After);
  ASSERT_TRUE(rebind(After, Longer, Name, Rebound));
  EXPECT_EQ(Rebound, Before);
}

TEST(OhmTraitImports, RejectsELFNameCollisions) {
  for (const auto &Extra :
       {("$" + Name +
         " = comdat any\n"
         "define linkonce_odr i32 @other() comdat($" +
         Name + ") { ret i32 0 }\n")
            .str(),
        ("@\"" + Name + "@V1\" = global i32 0\n").str(),
        ("@\"" + Longer + "@@V2\" = global i32 0\n").str(),
        ("@other = global i32 0, section \"" + Longer + "\"\n").str()}) {
    auto Before = source(Name) + Extra;
    auto After = source(Longer) + Extra;
    Bytes Donor, Key, Manifest, NextKey, NextManifest, Rebound;
    ASSERT_TRUE(emit(Before, Donor));
    ASSERT_TRUE(snapshot(Before, Key, Manifest));
    ASSERT_TRUE(snapshot(After, NextKey, NextManifest));
    ASSERT_EQ(Key, NextKey);
    EXPECT_FALSE(ohm::rebindTraitImports(bytes(Donor), bytes(Manifest),
                                         bytes(NextManifest), Rebound));
  }
}

TEST(OhmTraitImports, RejectsMismatchedObjectOrMethod) {
  Bytes Before, Out, Data, Collision, Defined;
  ASSERT_TRUE(emit(source(Name), Before));
  EXPECT_FALSE(rebind(Before, Name, Other, Out));
  EXPECT_FALSE(rebind(Before, Longer, Name, Out));
  Before.pop_back();
  EXPECT_FALSE(rebind(Before, Name, Longer, Out));
  ASSERT_TRUE(
      emit(source(Name) + ("@ptr = constant ptr @" + Name + "\n").str(), Data));
  EXPECT_FALSE(rebind(Data, Name, Longer, Out));
  ASSERT_TRUE(emit(source(Name) + ("@" + Longer + " = global i32 0\n").str(),
                   Collision));
  EXPECT_FALSE(rebind(Collision, Name, Longer, Out));
  ASSERT_TRUE(
      emit(source(Other) + ("define i32 @" + Name + "(){ret i32 0}").str(),
           Defined));
  EXPECT_FALSE(rebind(Defined, Name, Longer, Out));
}
#endif
} // namespace
