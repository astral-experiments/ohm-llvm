//===- OhmLeafObjectTest.cpp - Reuse across enum edits --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../../lib/CodeGen/OhmLeafObject.h"
#include "llvm/Bitcode/ClosedModuleHash.h"
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

TEST(OhmLeafInput, PreservesOtherRangesAndDeclinesAmbiguousTagSizes) {
  for (bool RustSymbol : {false, true}) {
    LLVMContext C;
    auto M = rules(C, 501);
    auto *Other = M->getFunction(Sibling);
    if (!RustSymbol)
      Other->setName("other");
    auto Range = Attribute::get(C, Attribute::Range,
                                ConstantRange(APInt(16, 0), APInt(16, 701)));
    Other->removeRetAttr(Attribute::Range);
    Other->addRetAttr(Range);
    EXPECT_EQ(ohm_leaf::prepare(*M, "session").empty(), RustSymbol);
    EXPECT_EQ(
        Other->getAttributes().getRetAttrs().getAttribute(Attribute::Range),
        Range);
  }
}

TEST(OhmLeafInput, RejectsStackProtection) {
  for (auto Attr :
       {Attribute::SafeStack, Attribute::StackProtect,
        Attribute::StackProtectStrong, Attribute::StackProtectReq}) {
    LLVMContext C;
    auto M = rules(C, 501);
    M->getFunction(Sibling)->addFnAttr(Attr);
    EXPECT_TRUE(ohm_leaf::prepare(*M, "session").empty());
  }
}

enum class Transfer { Direct, Compared, Returned, CallSiteRange };

std::string transferKey(unsigned Upper, Transfer Kind) {
  LLVMContext C;
  auto M = rules(C, 501, Upper);
  IRBuilder<> B(C);
  auto *Ty = B.getInt16Ty();
  auto *Source = Function::Create(FunctionType::get(Ty, false),
                                  GlobalValue::ExternalLinkage, "_Rsource", *M);
  auto *Sink = Function::Create(FunctionType::get(Ty, {Ty}, false),
                                GlobalValue::ExternalLinkage, "_Rsink", *M);
  Attribute Tag =
      M->getFunction(Name)->getAttributes().getRetAttr(Attribute::Range);
  Source->addRetAttr(Tag);
  Sink->addParamAttr(0, Tag);
  if (Kind == Transfer::Returned)
    Sink->addParamAttr(0, Attribute::Returned);
  auto *Caller = Function::Create(FunctionType::get(B.getInt1Ty(), false),
                                  GlobalValue::ExternalLinkage, "caller", *M);
  B.SetInsertPoint(BasicBlock::Create(C, "entry", Caller));
  auto *Value = B.CreateCall(Source);
  if (Kind == Transfer::CallSiteRange)
    Value->addRetAttr(Attribute::get(
        C, Attribute::Range, ConstantRange(APInt(16, 0), APInt(16, 974))));
  auto *Result = B.CreateCall(Sink, {Value});
  B.CreateRet(B.CreateICmpUGE(Kind == Transfer::Compared ? Value : Result,
                              B.getInt16(974)));
  return ohm_leaf::prepare(*M, "session");
}

TEST(OhmLeafInput, ProjectsOnlyOpaqueTagTransfers) {
  std::string Key = transferKey(974, Transfer::Direct);
  ASSERT_FALSE(Key.empty());
  EXPECT_EQ(Key, transferKey(975, Transfer::Direct));
  for (auto Kind :
       {Transfer::Compared, Transfer::Returned, Transfer::CallSiteRange})
    EXPECT_TRUE(transferKey(974, Kind).empty());
}

enum class LoadUse {
  MaskAndShift,
  Compared,
  WiderMask,
  SmallerShift,
  Collision
};

std::string loadKey(unsigned Upper, LoadUse Kind) {
  LLVMContext C;
  auto M = rules(C, 501, Upper);
  IRBuilder<> B(C);
  auto *F =
      Function::Create(FunctionType::get(B.getInt64Ty(), {B.getPtrTy()}, false),
                       GlobalValue::ExternalLinkage, "reader", *M);
  B.SetInsertPoint(BasicBlock::Create(C, "entry", F));
  auto Load = [&](IntegerType *Ty, unsigned Bound) {
    auto *V = B.CreateLoad(Ty, F->getArg(0));
    auto *Range =
        MDTuple::get(C, {ConstantAsMetadata::get(ConstantInt::get(Ty, 0)),
                         ConstantAsMetadata::get(ConstantInt::get(Ty, Bound))});
    V->setMetadata(LLVMContext::MD_range, Range);
    return V;
  };
  Value *V = B.CreateZExt(Load(B.getInt16Ty(), Upper), B.getInt64Ty());
  Value *Word = B.CreateLShr(V, Kind == LoadUse::SmallerShift ? 1 : 6);
  Value *Bit = B.CreateAnd(V, Kind == LoadUse::WiderMask ? 1023 : 63);
  Value *Result = B.CreateOr(Word, Bit);
  if (Kind == LoadUse::Compared)
    Result = B.CreateSelect(B.CreateICmpUGE(V, B.getInt64(974)), B.getInt64(0),
                            Result);
  if (Kind == LoadUse::Collision)
    Result = B.CreateOr(
        Result, B.CreateZExt(Load(B.getInt32Ty(), 976), B.getInt64Ty()));
  B.CreateRet(Result);
  return ohm_leaf::prepare(*M, "session");
}

TEST(OhmLeafInput, ProjectsOnlyLoadsWithEquivalentMasksAndShifts) {
  std::string Key = loadKey(974, LoadUse::MaskAndShift);
  ASSERT_FALSE(Key.empty());
  EXPECT_EQ(Key, loadKey(975, LoadUse::MaskAndShift));
  for (auto Kind :
       {LoadUse::Compared, LoadUse::WiderMask, LoadUse::SmallerShift})
    EXPECT_TRUE(loadKey(974, Kind).empty());
}

TEST(OhmLeafInput, KeepsRangesDistinguishableWhenNarrowingLoads) {
  // SelectionDAG can truncate [0, 976) on the i32 load to i16. Preserve
  // the distinct range of the first load when memory nodes are merged.
  EXPECT_TRUE(loadKey(974, LoadUse::Collision).empty());
}

enum class EarlyUse {
  Split,
  OtherComputation,
  Compare,
  Signed,
  Narrow,
  CallSiteRange,
  NonLocal,
  ExtraCaller,
  Occupied,
  OpenContext
};

std::string earlyKey(unsigned Upper, EarlyUse Use = EarlyUse::Split) {
  LLVMContext C;
  auto M = rules(C, 501, Upper);
  IRBuilder<> B(C);
  auto *I16 = B.getInt16Ty();
  auto *I64 = B.getInt64Ty();
  auto *Leaf = M->getFunction(Name);
  Attribute Tag = Leaf->getAttributes().getRetAttr(Attribute::Range);
  auto Helper = [&](StringRef Name) {
    auto *F = Function::Create(FunctionType::get(I64, {I16}, false),
                               GlobalValue::InternalLinkage, Name, *M);
    F->addParamAttr(0, Tag);
    B.SetInsertPoint(BasicBlock::Create(C, "entry", F));
    return F;
  };
  auto *Split = Helper("split");
  Value *Arg = Split->getArg(0);
  unsigned K = Use == EarlyUse::Narrow ? 2 : 64;
  auto *Extended = B.CreateZExt(Arg, I64);
  Value *Index = Use == EarlyUse::Signed
                     ? B.CreateSDiv(Extended, B.getInt64(K))
                     : B.CreateUDiv(Extended, B.getInt64(K));
  Value *Bit = B.CreateZExt(B.CreateURem(Arg, B.getInt16(K)), I64);
  Value *Result = B.CreateAdd(Index, Bit);
  if (Use == EarlyUse::Compare)
    Result = B.CreateSelect(B.CreateICmpULT(Arg, B.getInt16(974)), Result, Bit);
  if (Use == EarlyUse::OtherComputation)
    Result = B.CreateAdd(Result, B.getInt64(1));
  B.CreateRet(Result);

  auto *Forward = Helper("forward");
  if (Use == EarlyUse::NonLocal)
    Forward->setLinkage(GlobalValue::ExternalLinkage);
  auto *Call = B.CreateCall(Split, {Forward->getArg(0)});
  if (Use == EarlyUse::CallSiteRange)
    Call->addParamAttr(0, Tag);
  B.CreateRet(Call);
  auto *Root = Function::Create(FunctionType::get(I64, {B.getPtrTy()}, false),
                                GlobalValue::ExternalLinkage, "reader", *M);
  B.SetInsertPoint(BasicBlock::Create(C, "entry", Root));
  auto *Load = B.CreateLoad(I16, Root->getArg(0));
  auto *MD = MDTuple::get(C, {ConstantAsMetadata::get(B.getInt16(0)),
                              ConstantAsMetadata::get(B.getInt16(Upper))});
  Load->setMetadata(LLVMContext::MD_range, MD);
  Load->setMetadata(LLVMContext::MD_noundef, MDTuple::get(C, {}));
  if (Use == EarlyUse::ExtraCaller)
    B.CreateCall(Forward, {B.getInt16(0)});
  B.CreateRet(B.CreateCall(Forward, {Load}));

  constexpr StringLiteral Anchor = "ohm.leaf.ranges";
  if (Use == EarlyUse::Occupied)
    M->getOrInsertNamedMetadata(Anchor)->addOperand(MD);
  std::unique_ptr<Module> Other;
  if (Use == EarlyUse::OpenContext)
    Other = std::make_unique<Module>("other", C);

  std::string Proof = ohm_leaf::prepareEarly(*M, "session");
  unsigned Actual = Proof.empty() ? Upper : 976;
  EXPECT_EQ(Actual, getConstantRangeFromMetadata(
                        *Load->getMetadata(LLVMContext::MD_range))
                        .getUpper()
                        .getZExtValue());
  EXPECT_EQ(Actual, Leaf->getAttributes()
                        .getRetAttr(Attribute::Range)
                        .getValueAsConstantRange()
                        .getUpper()
                        .getZExtValue());
  for (auto *F : {Forward, Split})
    EXPECT_EQ(Actual, F->getAttributes()
                          .getParamAttr(0, Attribute::Range)
                          .getValueAsConstantRange()
                          .getUpper()
                          .getZExtValue());
  EXPECT_EQ(B.getInt16(501), Leaf->front().front().getOperand(0));
  if (Proof.empty()) {
    EXPECT_EQ(Use == EarlyUse::Occupied,
              M->getNamedMetadata(Anchor) != nullptr);
    if (Use == EarlyUse::Occupied && M->getNamedMetadata(Anchor))
      EXPECT_EQ(MD, M->getNamedMetadata(Anchor)->getOperand(0));
  } else {
    auto *Ranges = M->getNamedMetadata(Anchor);
    EXPECT_NE(nullptr, Ranges);
    if (Ranges)
      EXPECT_EQ(16u, Ranges->getNumOperands());
    // A subsequent proof still sees all rooted metadata in this context.
    std::array<uint8_t, 32> Closed;
    EXPECT_TRUE(getClosedModuleHashV1(*M, Closed));
  }
  return Proof;
}

TEST(OhmLeafInput, EarlyTransfersRetainBothProjectionIntervals) {
  auto Key = earlyKey(974);
  ASSERT_FALSE(Key.empty());
  EXPECT_EQ(Key, earlyKey(975));
  EXPECT_EQ(Key, earlyKey(976));
  auto Other = earlyKey(975, EarlyUse::OtherComputation);
  ASSERT_FALSE(Other.empty());
  EXPECT_NE(Key, Other);
}

TEST(OhmLeafInput, EarlyMaskDoesNotHideMetadataObservers) {
  LLVMContext C;
  auto M = rules(C, 502);
  auto *Literal =
      cast<ConstantInt>(M->getFunction(Name)->front().front().getOperand(0));
  // Only the isolated return references this integer in the module.
  // Its detached metadata holder must remain visible to the context check.
  MDTuple::get(C, {ConstantAsMetadata::get(Literal)});
  ASSERT_TRUE(Literal->isUsedByMetadata());
  EXPECT_TRUE(ohm_leaf::prepareEarly(*M, "session").empty());
}

TEST(OhmLeafInput, EarlyRejectionRestoresInput) {
  for (auto Use :
       {EarlyUse::Compare, EarlyUse::Signed, EarlyUse::Narrow,
        EarlyUse::CallSiteRange, EarlyUse::NonLocal, EarlyUse::ExtraCaller,
        EarlyUse::Occupied, EarlyUse::OpenContext})
    EXPECT_TRUE(earlyKey(974, Use).empty());
  // Even when the upper bound is already aligned, validate the observation.
  EXPECT_TRUE(earlyKey(976, EarlyUse::Compare).empty());
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

TEST_F(OhmLeafObject, EarlyReusePreservesWhichReturnsAreEqual) {
  // The sibling returns 501 throughout. LLVM can merge the two functions
  // when the edited return becomes 501, but not when it becomes 503.
  enum class Peer { Mangled, Exported, WiderExport };
  for (auto Kind : {Peer::Mangled, Peer::Exported, Peer::WiderExport}) {
    auto make = [Kind](LLVMContext &C, unsigned Value, unsigned Upper = 974) {
      auto M = rules(C, Value, Upper);
      auto *F = M->getFunction(Sibling);
      if (Kind != Peer::Mangled)
        F->setName("exported");
      if (Kind == Peer::WiderExport) {
        // The return ranges match only after projection.
        F->removeRetAttr(Attribute::Range);
        F->addRetAttr(Attribute::get(
            C, Attribute::Range, ConstantRange(APInt(16, 0), APInt(16, 976))));
      }
      return M;
    };

    LLVMContext Old, Equal, Distinct;
    auto Before = make(Old, 502);
    auto First = ohm_leaf::prepareEarly(*Before, "session");
    auto Donor = emit(*Before);
    auto Witness = ohm_leaf::enroll(First, Donor);
    ASSERT_FALSE(Witness.empty());

    auto Same = make(Equal, 501);
    auto SameProof = ohm_leaf::prepareEarly(*Same, "session");
    ASSERT_FALSE(SameProof.empty());
    EXPECT_TRUE(ohm_leaf::tryReuse(SameProof, Witness, Donor).empty());

    auto Renumbered = make(Distinct, 503, 975);
    auto Proof = ohm_leaf::prepareEarly(*Renumbered, "session");
    auto Actual = ohm_leaf::tryReuse(Proof, Witness, Donor);
    ASSERT_FALSE(Actual.empty());
    EXPECT_EQ(Actual, emit(*Renumbered));
  }
}

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
