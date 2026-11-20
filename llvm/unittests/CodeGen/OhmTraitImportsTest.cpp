//===- OhmTraitImportsTest.cpp - Rust trait import cache tests
//--------------===//
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

static std::string source(StringRef Import, StringRef Literal = Name) {
  // The plain symbol @old and the section "ld" can share their spelling
  // with the provider suffix in the combined symbol/section string table.
  return ("target triple = \"x86_64-unknown-linux-gnu\"\n"
          "@old = constant i32 1\n"
          "@literal = constant [" +
          Twine(Literal.size()) + " x i8] c\"" + Literal +
          "\", section \"ld\"\n"
          "declare i32 @" +
          Import +
          "()\n"
          "define i32 @caller() { %v = call i32 @" +
          Import + "() ret i32 %v }\n")
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

TEST(OhmTraitImports, OnlyProviderIsProjected) {
  LLVMContext C;
  auto M = read(C, source(Name));
  ASSERT_TRUE(M);
  Bytes Base, Names, Current, ChangedLiteral, ChangedMethod;
  ASSERT_TRUE(ohm::writeTraitImportSignature(*M, Base, Names));
  EXPECT_TRUE(M->getFunction(Name));
  EXPECT_EQ(StringRef(Names.data(), Names.size()),
            StringRef(Name.data(), Name.size() + 1));
  ASSERT_TRUE(signature(source(Longer), Current));
  EXPECT_EQ(Base, Current);
  ASSERT_TRUE(signature(source(Longer, Longer), ChangedLiteral));
  EXPECT_NE(Base, ChangedLiteral);
  ASSERT_TRUE(signature(source(Other), ChangedMethod));
  EXPECT_NE(Base, ChangedMethod);
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

static bool rebind(ArrayRef<char> Input, StringLiteral Old,
                   StringLiteral Current, Bytes &Out) {
  return ohm::rebindTraitImports(StringRef(Input.data(), Input.size()),
                                 StringRef(Old.data(), Old.size() + 1),
                                 StringRef(Current.data(), Current.size() + 1),
                                 Out);
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
