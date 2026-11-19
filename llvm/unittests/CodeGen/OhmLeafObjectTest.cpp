//===- OhmLeafObjectTest.cpp - Reuse across enum edits --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../../lib/CodeGen/OhmLeafObject.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

constexpr StringLiteral X86("x86_64-unknown-linux-gnu");
constexpr StringLiteral Name = "_RNvC4test4leaf";
constexpr StringLiteral Sibling = "_RNvC4test5other";
enum class Other { SameValue, Forward, Alias };

std::unique_ptr<Module> rules(LLVMContext &C, unsigned Literal,
                              unsigned Upper = 974,
                              Other Kind = Other::SameValue) {
  auto M = std::make_unique<Module>("example", C);
  M->setTargetTriple(Triple(X86));
  IRBuilder<> B(C);
  auto make = [&](StringRef Symbol) {
    auto *Ty = FunctionType::get(B.getInt16Ty(), false);
    auto *F = Function::Create(Ty, GlobalValue::ExternalLinkage, Symbol, *M);
    ConstantRange Range(APInt(16, 0), APInt(16, Upper));
    F->addRetAttr(Attribute::get(C, Attribute::Range, Range));
    B.SetInsertPoint(BasicBlock::Create(C, "entry", F));
    return F;
  };
  auto *Leaf = make(Name);
  B.CreateRet(B.getInt16(Literal));
  if (Kind == Other::Alias)
    GlobalAlias::create(Leaf->getValueType(), 0, GlobalValue::ExternalLinkage,
                        Sibling, Leaf, M.get());
  else {
    make(Sibling);
    if (Kind == Other::Forward)
      B.CreateRet(B.CreateCall(Leaf));
    else
      B.CreateRet(B.getInt16(std::min(501u, Upper - 1)));
  }
  return M;
}

TEST(OhmLeafInput, RoundsRangesOnTheActualDonorAndRestoresTheLiteral) {
  LLVMContext C;
  auto M = rules(C, 501);
  EXPECT_TRUE(ohm_leaf::candidate(*M));
  EXPECT_FALSE(ohm_leaf::prepare(*M, "session").empty());
  auto *F = M->getFunction(Name);
  auto Attr = F->getAttributes().getRetAttrs().getAttribute(Attribute::Range);
  EXPECT_EQ(Attr.getValueAsConstantRange().getUpper(), APInt(16, 976));
  auto *Ret = cast<ReturnInst>(&F->getEntryBlock().front());
  EXPECT_EQ(cast<ConstantInt>(Ret->getReturnValue())->getZExtValue(), 501u);
}

TEST(OhmLeafInput, RefusesAnUnclosedContextBeforeWidening) {
  LLVMContext C;
  auto M = rules(C, 501);
  Module Unrelated("another", C);
  EXPECT_TRUE(ohm_leaf::prepare(*M, "session").empty());
  auto *F = M->getFunction(Name);
  auto Attr = F->getAttributes().getRetAttrs().getAttribute(Attribute::Range);
  EXPECT_EQ(Attr.getValueAsConstantRange().getUpper(), APInt(16, 974));
  auto *Ret = cast<ReturnInst>(&F->getEntryBlock().front());
  EXPECT_EQ(cast<ConstantInt>(Ret->getReturnValue())->getZExtValue(), 501u);
}

TEST(OhmLeafInput, RefusesOtherTargetsAndSmallTags) {
  LLVMContext C;
  auto M = rules(C, 501);
  M->setTargetTriple(Triple("aarch64-unknown-linux-gnu"));
  EXPECT_FALSE(ohm_leaf::candidate(*M));
  M->setTargetTriple(Triple("x86_64-apple-darwin"));
  EXPECT_FALSE(ohm_leaf::candidate(*M));
  LLVMContext Small;
  EXPECT_FALSE(ohm_leaf::candidate(*rules(Small, 12, 256)));
}

class OhmLeafObject : public ::testing::Test {
protected:
  std::unique_ptr<TargetMachine> TM;
  void SetUp() override {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    std::string Err;
    auto *Target = TargetRegistry::lookupTarget(Triple(X86), Err);
    if (!Target)
      GTEST_SKIP() << "Requires the x86 backend";
    TargetOptions Options;
    Options.FunctionSections = true;
    TM.reset(Target->createTargetMachine(Triple(X86), "x86-64", "", Options,
                                         Reloc::PIC_));
    ASSERT_TRUE(TM);
  }

  std::string emit(Module &M) {
    M.setDataLayout(TM->createDataLayout());
    legacy::PassManager PM;
    SmallVector<char, 0> Bytes;
    raw_svector_ostream Out(Bytes);
    if (TM->addPassesToEmitFile(PM, Out, nullptr,
                                CodeGenFileType::ObjectFile)) {
      ADD_FAILURE() << "Cannot emit an ELF object";
      return {};
    }
    PM.run(M);
    return Out.str().str();
  }
};

TEST_F(OhmLeafObject, MatchesAFreshObjectAfterOneOfTwoEqualReturnsChanges) {
  LLVMContext Old, Current;
  auto Before = rules(Old, 501);
  auto First = ohm_leaf::prepare(*Before, "session");
  auto Donor = emit(*Before);
  auto Witness = ohm_leaf::enroll(First, Donor);
  ASSERT_FALSE(Witness.empty());

  auto M = rules(Current, 502, 975);
  auto Proof = ohm_leaf::prepare(*M, "session");
  auto Actual = ohm_leaf::tryReuse(Proof, Witness, Donor);
  ASSERT_FALSE(Actual.empty());
  EXPECT_NE(Actual, Donor);
  EXPECT_EQ(Actual, emit(*M));
}

TEST_F(OhmLeafObject, RejectsMergedFunctions) {
  // These are two valid ways for LLVM to emit two identical literal
  // functions. With either representation, changing the common mov's
  // immediate would affect both functions.
  LLVMContext Old, Current;
  auto Before = rules(Old, 501);
  auto After = rules(Current, 502, 975);
  auto First = ohm_leaf::prepare(*Before, "session");
  auto Proof = ohm_leaf::prepare(*After, "session");
  for (auto Kind : {Other::Forward, Other::Alias}) {
    LLVMContext Folded;
    auto Equivalent = rules(Folded, 501, 976, Kind);
    auto Donor = emit(*Equivalent);
    auto Witness = ohm_leaf::enroll(First, Donor);
    ASSERT_FALSE(Witness.empty());
    EXPECT_TRUE(ohm_leaf::tryReuse(Proof, Witness, Donor).empty());
  }
}

TEST_F(OhmLeafObject, RejectsOtherInputsAndCorruptObjects) {
  LLVMContext Old, Current;
  auto Before = rules(Old, 501);
  auto First = ohm_leaf::prepare(*Before, "session");
  auto Donor = emit(*Before);
  auto Witness = ohm_leaf::enroll(First, Donor);
  ASSERT_FALSE(Witness.empty());
  auto M = rules(Current, 502, 975);
  auto OtherSession = ohm_leaf::prepare(*M, "other");
  EXPECT_TRUE(ohm_leaf::tryReuse(OtherSession, Witness, Donor).empty());

  LLVMContext Wider;
  auto Wide = rules(Wider, 502, 977);
  auto OtherBucket = ohm_leaf::prepare(*Wide, "session");
  EXPECT_TRUE(ohm_leaf::tryReuse(OtherBucket, Witness, Donor).empty());

  auto Proof = ohm_leaf::prepare(*M, "session");
  ASSERT_FALSE(ohm_leaf::tryReuse(Proof, Witness, Donor).empty());
  auto Corrupt = Donor;
  Corrupt.back() ^= 1;
  EXPECT_TRUE(ohm_leaf::tryReuse(Proof, Witness, Corrupt).empty());
  Witness.back() ^= 1;
  EXPECT_TRUE(ohm_leaf::tryReuse(Proof, Witness, Donor).empty());
  // Reseal the truncated object to exercise the ELF bounds checks.
  Donor.resize(Donor.size() / 2);
  Witness = ohm_leaf::enroll(First, Donor);
  EXPECT_TRUE(ohm_leaf::tryReuse(Proof, Witness, Donor).empty());
}
} // namespace
