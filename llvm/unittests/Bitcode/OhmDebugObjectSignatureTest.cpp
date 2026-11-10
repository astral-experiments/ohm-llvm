// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "llvm/Bitcode/OhmDebugObjectSignature.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/OhmOptimizerState.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"
#include <array>
#include <set>

using namespace llvm;

namespace {
class DebugObjectSignatureTest : public testing::Test {
protected:
  const std::string OldSum = std::string(32, 'a');
  const std::string NewSum = std::string(32, 'b');
  std::string Mapping =
      R"({"version":1,"files":[{"directory":"/src","filename":"f.c","lines":[[3,4]]}]})";
  std::string Proof;
  char *Origin = nullptr;
  size_t OriginSize = 0;

  void setProof(unsigned Version, unsigned OldLines, unsigned NewLines) {
    auto Digest = toHex(SHA256::hash(arrayRefFromStringRef(Mapping)), true);
    auto File =
        formatv(
            R"("directory":"/src","filename":"f.c","{4}":{5},"new_lines":{6},"old":{{"md5":"{0}","sha1":"{2}","sha256":"{3}"},"new":{{"md5":"{1}","sha1":"{2}","sha256":"{3}"})",
            OldSum, NewSum, std::string(40, '1'), std::string(64, '2'),
            Version == 1 ? "ordered_byte_identical_old_lines" : "old_lines",
            OldLines, NewLines)
            .str();
    Proof = formatv(R"({{"version":{2},"map_sha256":"{0}","files":[{{{1}}]})",
                    Digest, File, Version)
                .str();
  }

  void SetUp() override { setProof(1, 3, 4); }

  void partialMap(StringRef Lines, unsigned OldLines, unsigned NewLines) {
    Mapping = formatv(
        R"({{"version":1,"files":[{{"directory":"/src","filename":"f.c","lines":{0}}]})",
        Lines).str();
    setProof(2, OldLines, NewLines);
  }

  ~DebugObjectSignatureTest() override { ohm_bc_free_v1(Origin); }

  std::unique_ptr<Module> module(LLVMContext &Context, StringRef Checksum,
                                 unsigned Line, StringRef Operand) {
    auto IR = formatv(R"(
      target triple = "x86_64-unknown-linux-gnu"
      define i32 @f(i32 %x) !dbg !4 {{
        %y = add i32 %x, 1, !dbg !5, !guard !6
        ret i32 %y, !dbg !5
      }
      !llvm.dbg.cu = !{{!0}
      !llvm.module.flags = !{{!3}
      !0 = distinct !DICompileUnit(language: DW_LANG_C, file: !1, producer: "test", isOptimized: true, runtimeVersion: 0, emissionKind: LineTablesOnly)
      !1 = !DIFile(filename: "f.c", directory: "/src", checksumkind: CSK_MD5, checksum: "{0}")
      !2 = !DISubroutineType(types: !{{})
      !3 = !{{i32 2, !"Debug Info Version", i32 3}
      !4 = distinct !DISubprogram(name: "f", scope: !1, file: !1, line: 1, type: !2, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)
      !5 = !DILocation(line: {1}, column: 2, scope: !4)
      !6 = !{{!"{2}"}
    )",
                      Checksum, Line, Operand)
                  .str();
    SMDiagnostic Error;
    auto M = parseAssemblyString(IR, Error, Context);
    if (!M)
      ADD_FAILURE() << Error.getMessage().str() << " at " << Error.getLineNo()
                    << ": " << Error.getLineContents().str();
    return M;
  }

  void train(Module &M) {
    ASSERT_EQ(ohm_bc_save_v1(wrap(&M), &Origin, &OriginSize), 1);
  }

  void location(Module &M, unsigned Line, bool SharedTail = false) {
    LLVMContext &C = M.getContext();
    auto Global = [&](StringRef Name, Constant *Initializer, Align Alignment) {
      auto *G = new GlobalVariable(M, Initializer->getType(), true,
                                   GlobalValue::PrivateLinkage, Initializer, Name);
      G->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
      G->setAlignment(Alignment);
      return G;
    };
    auto *File = Global("file", ConstantDataArray::getString(C, "f.c"), Align(1));
    std::array<uint8_t, 16> Data{};
    support::endian::write64le(Data.data(), 3);
    support::endian::write32le(Data.data() + 8, Line);
    support::endian::write32le(Data.data() + 12, 1);
    auto *Tail = ConstantDataArray::get(C, ArrayRef<uint8_t>(Data));
    auto *Rec = Global("location", ConstantStruct::getAnon({File, Tail}, true), Align(8));
    if (SharedTail)
      Global("unrelated", Tail, Align(1));
    IRBuilder<> B(&*M.getFunction("f")->getEntryBlock().getFirstInsertionPt());
    auto Callee = M.getOrInsertFunction("_Ropaque", B.getVoidTy(), B.getPtrTy());
    B.CreateCall(Callee, {Rec});
  }

  static int accept(void *Context, const char *Records, size_t Size) {
    *static_cast<std::string *>(Context) = StringRef(Records, Size).str();
    return 1;
  }

  static int decline(void *Context, const char *Records, size_t Size) {
    return 0;
  }

  int relocatable(Module &M, OhmCheckRuntimeLocations Check, void *Data) {
    return ohm_bc_match_relocatable_v1(
        wrap(&M), Origin, OriginSize, Mapping.data(), Mapping.size(),
        Proof.data(), Proof.size(), Check, Data);
  }

  bool matches(Module &M) {
    return ohm_bc_match_v1(wrap(&M), Origin, OriginSize, Mapping.data(),
                           Mapping.size(), Proof.data(), Proof.size());
  }
};

static std::string machineKey(Module &M) {
  char *Data = nullptr;
  size_t Size = 0;
  bool Saved = ohm_bc_save_v1(wrap(&M), &Data, &Size);
  std::string Key;
  if (Saved)
    Key.assign(Data, Size);
  ohm_bc_free_v1(Data);
  return Key;
}

static bool canSaveMachineInput(Module &M) { return !machineKey(M).empty(); }

static std::string optimizerKey(Module &M) {
  SmallVector<char, 0> Bytes;
  if (!writeClosedOptimizerInput(M, Bytes))
    return {};
  return std::string(Bytes.begin(), Bytes.end());
}

static bool canSaveOptimizerInput(Module &M) {
  return !optimizerKey(M).empty();
}

TEST(DebugObjectUsesTest, OptimizerSliceLeafRecordsStorageAndUseOrder) {
  LLVMContext C;
  SMDiagnostic Error;
  auto M = parseAssemblyString(R"(
    target triple = "x86_64-unknown-linux-gnu"
    @a = private constant i8 1
    @b = private constant i8 2
    @keep = constant ptr @a
  )",
                               Error, C);
  ASSERT_TRUE(M);
  auto *A = M->getNamedGlobal("a");
  auto *B = M->getNamedGlobal("b");
  auto *Length = PoisonValue::get(Type::getInt64Ty(C));
  std::string Original = optimizerKey(*M);
  ASSERT_FALSE(Original.empty());
  auto *Slice = ConstantStruct::getAnon({A, Length});
  std::string WithSlice = optimizerKey(*M);
  ASSERT_FALSE(WithSlice.empty());
  EXPECT_NE(Original, WithSlice);
  A->reverseUseList();
  std::string Reordered = optimizerKey(*M);
  EXPECT_FALSE(Reordered.empty());
  EXPECT_NE(WithSlice, Reordered);
  A->reverseUseList();
  EXPECT_EQ(WithSlice, optimizerKey(*M));
  Slice->destroyConstant();
  auto *Other = ConstantStruct::getAnon({B, Length});
  std::string OtherKey = optimizerKey(*M);
  EXPECT_FALSE(OtherKey.empty());
  EXPECT_NE(WithSlice, OtherKey);
  Other->destroyConstant();
  EXPECT_EQ(Original, optimizerKey(*M));
}

TEST(DebugObjectUsesTest, OptimizerSliceLeafRecordsNumberedIntegerAddress) {
  LLVMContext C;
  SMDiagnostic Error;
  auto M = parseAssemblyString(R"(
    target triple = "x86_64-unknown-linux-gnu"
    @one = constant ptr inttoptr (i64 1 to ptr)
    @eight = constant ptr inttoptr (i64 8 to ptr)
  )",
                               Error, C);
  ASSERT_TRUE(M);
  auto *One = M->getNamedGlobal("one")->getInitializer();
  auto *Eight = M->getNamedGlobal("eight")->getInitializer();
  auto *Length = PoisonValue::get(Type::getInt64Ty(C));
  const std::string Original = optimizerKey(*M);
  ASSERT_FALSE(Original.empty());
  auto *First = ConstantStruct::getAnon({One, Length});
  const std::string OneKey = optimizerKey(*M);
  ASSERT_FALSE(OneKey.empty());
  EXPECT_NE(OneKey, Original);
  First->destroyConstant();
  auto *Second = ConstantStruct::getAnon({Eight, Length});
  const std::string EightKey = optimizerKey(*M);
  ASSERT_FALSE(EightKey.empty());
  EXPECT_NE(EightKey, OneKey);
  Eight->reverseUseList();
  const std::string Reordered = optimizerKey(*M);
  EXPECT_FALSE(Reordered.empty());
  EXPECT_NE(Reordered, EightKey);
  Second->destroyConstant();
  EXPECT_EQ(Original, optimizerKey(*M));
}

TEST(DebugObjectUsesTest, OptimizerDeclinesDetachedIntegerAddressObserver) {
  LLVMContext C;
  Module M("context", C);
  auto ContextIsAccountedFor = [&] {
    SmallVector<char, 0> State;
    return ohm::getOptimizerContextState(M, State);
  };
  auto *Ptr = PointerType::getUnqual(C);
  auto *I64 = Type::getInt64Ty(C);
  auto *Length = PoisonValue::get(I64);
  auto *Null = ConstantPointerNull::get(Ptr);
  auto *Inert = ConstantStruct::getAnon({Null, Length});
  ASSERT_FALSE(Null->hasUseList());
  ASSERT_FALSE(Length->hasUseList());
  ASSERT_TRUE(ContextIsAccountedFor());
  const std::string Original = optimizerKey(M);
  ASSERT_FALSE(Original.empty());
  auto Address = [&] {
    return ConstantExpr::getIntToPtr(ConstantInt::get(I64, 8), Ptr);
  };
  auto *Detached = ConstantStruct::getAnon({Address(), Length});
  EXPECT_EQ(Original, optimizerKey(M));
  EXPECT_TRUE(Address()->hasOneUse());
  EXPECT_FALSE(ContextIsAccountedFor());
  Detached->destroyConstant();
  EXPECT_TRUE(ContextIsAccountedFor());
  Inert->destroyConstant();
}

TEST(DebugObjectUsesTest, OptimizerSliceLeafDeclinesOtherObserversAndForms) {
  LLVMContext C;
  SMDiagnostic Error;
  auto M = parseAssemblyString(R"(
    target triple = "x86_64-unknown-linux-gnu"
    @data = private constant i8 0
    @writable = private global i8 0
    @ext = external constant i8
  )",
                               Error, C);
  ASSERT_TRUE(M);
  auto *Storage = M->getNamedGlobal("data");
  auto *I64 = Type::getInt64Ty(C);
  auto *Length = PoisonValue::get(I64);
  auto Declined = [&](Constant *Leaf) {
    EXPECT_FALSE(canSaveOptimizerInput(*M));
    Leaf->destroyConstant();
  };
  Declined(ConstantStruct::getAnon({M->getNamedGlobal("writable"), Length}));
  Declined(ConstantStruct::getAnon({M->getNamedGlobal("ext"), Length}));
  Declined(ConstantStruct::getAnon({Storage, Length}, /*Packed=*/true));
  Declined(ConstantStruct::getAnon({Storage, UndefValue::get(I64)}));
  Declined(ConstantStruct::getAnon({Storage, ConstantInt::get(I64, 4)}));
  auto *Slice = ConstantStruct::getAnon({Storage, Length});
  ASSERT_TRUE(canSaveOptimizerInput(*M));
  auto *Observer = ConstantStruct::getAnon({Slice});
  EXPECT_FALSE(canSaveOptimizerInput(*M));
  Observer->destroyConstant();
  ASSERT_TRUE(canSaveOptimizerInput(*M));
  (void)ConstantAsMetadata::get(Length);
  EXPECT_FALSE(canSaveOptimizerInput(*M));
}

TEST(DebugObjectUsesTest, AuthenticatesAnUnreferencedInitializerUser) {
  LLVMContext Context;
  SMDiagnostic Error;
  auto M = parseAssemblyString(R"(
    target triple = "x86_64-unknown-linux-gnu"
    @target = external global i32
    @ptr = private unnamed_addr constant ptr @target
    @field = constant i32 trunc (i64 sub (i64 ptrtoint (ptr @ptr to i64),
                                         i64 ptrtoint (ptr @field to i64)) to i32)
  )",
                               Error, Context);
  ASSERT_TRUE(M);
  auto Serialize = [&] {
    SmallVector<char, 0> Bytes;
    raw_svector_ostream Stream(Bytes);
    WriteBitcodeToFile(*M, Stream, /*ShouldPreserveUseListOrder=*/true);
    return Bytes;
  };
  auto Before = Serialize();
  std::string OriginalKey = machineKey(*M);
  ASSERT_FALSE(OriginalKey.empty());
  Constant *Init = M->getNamedGlobal("field")->getInitializer();
  ASSERT_TRUE(Init->hasOneUse());
  auto *Unreferenced = ConstantStruct::getAnon({Init});
  EXPECT_EQ(Before, Serialize());
  EXPECT_FALSE(Init->hasOneUse());
  std::string WithLeaf = machineKey(*M);
  ASSERT_FALSE(WithLeaf.empty());
  EXPECT_FALSE(canSaveOptimizerInput(*M));
  EXPECT_NE(OriginalKey, WithLeaf);
  // The private evidence block remains valid, readable LLVM bitcode.
  LLVMContext OtherContext;
  auto Read =
      parseBitcodeFile(MemoryBufferRef(WithLeaf, "checked"), OtherContext);
  ASSERT_TRUE(bool(Read)) << toString(Read.takeError());
  EXPECT_NE(nullptr, (*Read)->getNamedGlobal("field"));
  // Named metadata serializes the user without giving it a value ID in the
  // writer's use-list order map.
  auto *Named = M->getOrInsertNamedMetadata("additional");
  Named->addOperand(
      MDNode::get(Context, ConstantAsMetadata::get(Unreferenced)));
  EXPECT_FALSE(canSaveMachineInput(*M));
  M->eraseNamedMetadata(Named);
  Unreferenced->destroyConstant();
  EXPECT_TRUE(canSaveMachineInput(*M));
  EXPECT_TRUE(canSaveOptimizerInput(*M));
}

TEST(DebugObjectUsesTest, ChecksInstructionsBlocksAndMetadataOnlyOperands) {
  LLVMContext Context;
  SMDiagnostic Error;
  auto M = parseAssemblyString(R"(
    target triple = "x86_64-unknown-linux-gnu"
    define i32 @f(i32 %arg) {
      br label %exit
    exit:
      %v = add i32 %arg, 1
      ret i32 %v
    }
    !named = !{!0}
    !0 = !{ptr inttoptr (i64 123456789 to ptr)}
  )",
                               Error, Context);
  ASSERT_TRUE(M);
  ASSERT_TRUE(canSaveMachineInput(*M));
  Function *F = M->getFunction("f");
  BasicBlock &Exit = F->back();
  auto *Outside = BinaryOperator::CreateAdd(F->getArg(0), &Exit.front());
  EXPECT_FALSE(canSaveMachineInput(*M));
  Outside->deleteValue();
  auto *Address = BlockAddress::get(&Exit);
  EXPECT_FALSE(canSaveMachineInput(*M));
  auto *AddressOwner = new GlobalVariable(*M, Address->getType(), true,
                                          GlobalValue::PrivateLinkage, Address);
  EXPECT_TRUE(canSaveMachineInput(*M));
  AddressOwner->eraseFromParent();
  Address->destroyConstant();
  Constant *Number = ConstantExpr::getIntToPtr(
      ConstantInt::get(Type::getInt64Ty(Context), 123456789),
      PointerType::getUnqual(Context));
  auto *User = ConstantStruct::getAnon({Number});
  EXPECT_FALSE(canSaveMachineInput(*M));
  User->destroyConstant();
  EXPECT_TRUE(canSaveMachineInput(*M));
}

TEST(DebugObjectUsesTest, AuthenticatesLeafKindsOperandsAndUseOrder) {
  LLVMContext C;
  SMDiagnostic Error;
  auto M = parseAssemblyString(R"(
    target triple = "x86_64-unknown-linux-gnu"
    @a = external global i8
    @b = external global i8
    @keep = global ptr @a
  )",
                               Error, C);
  ASSERT_TRUE(M);
  GlobalVariable *A = M->getNamedGlobal("a"), *B = M->getNamedGlobal("b");
  auto *Ptr = A->getType();
  auto *I64 = Type::getInt64Ty(C);
  auto *I128 = Type::getInt128Ty(C);
  std::set<std::string> Keys;
  auto Distinct = [&](Constant *Leaf) {
    std::string Key = machineKey(*M);
    EXPECT_FALSE(Key.empty());
    EXPECT_TRUE(Keys.insert(Key).second);
    Leaf->destroyConstant();
  };
  Distinct(ConstantStruct::getAnon({A, B}));
  Distinct(ConstantStruct::getAnon({B, A}));
  Distinct(ConstantStruct::getAnon({A, B}, true));
  Distinct(ConstantArray::get(ArrayType::get(Ptr, 2), {A, B}));
  Distinct(ConstantStruct::getAnon({A, B, A}));
  Distinct(ConstantStruct::getAnon({A, PoisonValue::get(I64)}));
  Distinct(ConstantStruct::getAnon({A, UndefValue::get(I64)}));
  Distinct(ConstantStruct::getAnon({A, UndefValue::get(I128)}));
  Distinct(ConstantStruct::getAnon({A, ConstantInt::get(I128, 0)}));
  Distinct(ConstantStruct::getAnon(
      {A, ConstantInt::get(C, APInt::getOneBitSet(128, 120))}));

  auto Ordinary = [&] {
    SmallVector<char, 0> Data;
    raw_svector_ostream OS(Data);
    WriteBitcodeToFile(*M, OS, /*ShouldPreserveUseListOrder=*/true);
    return Data;
  };
  auto Public = Ordinary();
  auto *Repeated = ConstantArray::get(ArrayType::get(Ptr, 2), {A, A});
  std::string Before = machineKey(*M);
  A->reverseUseList();
  std::string After = machineKey(*M);
  EXPECT_EQ(Public, Ordinary());
  EXPECT_FALSE(Before.empty());
  EXPECT_FALSE(After.empty());
  EXPECT_NE(Before, After);
  A->reverseUseList();
  EXPECT_EQ(Before, machineKey(*M));
  auto *Nested = ConstantStruct::getAnon({Repeated});
  EXPECT_FALSE(canSaveMachineInput(*M));
  Nested->destroyConstant();
  Repeated->destroyConstant();
  auto *Named = StructType::create(C, {Ptr}, "named");
  auto *Identified = ConstantStruct::get(Named, A);
  EXPECT_FALSE(canSaveMachineInput(*M));
  Identified->destroyConstant();
  SmallVector<Constant *, 0> Many(257, A);
  auto *Wide = ConstantArray::get(ArrayType::get(Ptr, Many.size()), Many);
  EXPECT_FALSE(canSaveMachineInput(*M));
  Wide->destroyConstant();
}

TEST(DebugObjectUsesTest, AuthenticatesUnnumberedMetadataOperandOrder) {
  LLVMContext C;
  SMDiagnostic Error;
  auto M = parseAssemblyString(R"(
    target triple = "x86_64-unknown-linux-gnu"
    declare void @llvm.experimental.noalias.scope.decl(metadata)
    define void @f() {
      call void @llvm.experimental.noalias.scope.decl(metadata !2)
      call void @llvm.experimental.noalias.scope.decl(metadata !2)
      ret void
    }
    !0 = distinct !{!0, !"domain"}
    !1 = distinct !{!1, !0, !"scope"}
    !2 = !{!1}
  )",
                               Error, C);
  ASSERT_TRUE(M);
  auto *Call = cast<CallInst>(&M->getFunction("f")->front().front());
  auto *Scope = cast<MetadataAsValue>(Call->getArgOperand(0));
  ASSERT_TRUE(Scope->hasNUses(2));
  EXPECT_FALSE(canSaveOptimizerInput(*M));
  auto Ordinary = [&] {
    SmallVector<char, 0> Data;
    raw_svector_ostream OS(Data);
    WriteBitcodeToFile(*M, OS, /*ShouldPreserveUseListOrder=*/true);
    return Data;
  };
  auto Public = Ordinary();
  std::string Key = machineKey(*M);
  ASSERT_FALSE(Key.empty());
  Scope->reverseUseList();
  EXPECT_EQ(Public, Ordinary());
  EXPECT_NE(Key, machineKey(*M));
  Scope->reverseUseList();
  EXPECT_EQ(Key, machineKey(*M));
  auto *Outside = CallInst::Create(Call->getFunctionType(),
                                   Call->getCalledOperand(), {Scope});
  EXPECT_FALSE(canSaveMachineInput(*M));
  Outside->deleteValue();
  EXPECT_EQ(Key, machineKey(*M));
}

TEST(DebugObjectUsesTest, IncludesTheWritersImplicitShuffleMask) {
  LLVMContext Context;
  SMDiagnostic Error;
  auto M = parseAssemblyString(R"(
    target triple = "x86_64-unknown-linux-gnu"
    define <2 x i32> @f(<2 x i32> %v) {
      %s = shufflevector <2 x i32> %v, <2 x i32> poison,
                         <2 x i32> <i32 1, i32 poison>
      ret <2 x i32> %s
    }
  )",
                               Error, Context);
  ASSERT_TRUE(M);
  ASSERT_TRUE(canSaveMachineInput(*M));
  auto &Shuffle = cast<ShuffleVectorInst>(M->getFunction("f")->front().front());
  Constant *Mask = Shuffle.getShuffleMaskForBitcode();
  ASSERT_TRUE(Mask->hasUseList());
  std::string Before = machineKey(*M);
  auto *User = ConstantStruct::getAnon({Mask});
  ASSERT_TRUE(canSaveMachineInput(*M));
  EXPECT_NE(Before, machineKey(*M));
  User->destroyConstant();
  EXPECT_TRUE(canSaveMachineInput(*M));
}

TEST_F(DebugObjectSignatureTest, ChecksUsesOnBothSidesOfALineTableLookup) {
  LLVMContext Before, After;
  auto Old = module(Before, OldSum, 3, "same");
  auto New = module(After, NewSum, 4, "same");
  ASSERT_TRUE(Old && New);
  train(*Old);
  ASSERT_TRUE(matches(*New));
  auto *User = ConstantStruct::getAnon({New->getFunction("f")});
  EXPECT_TRUE(canSaveMachineInput(*New));
  EXPECT_FALSE(matches(*New));
  auto *OldUser = ConstantStruct::getAnon({Old->getFunction("f")});
  train(*Old);
  EXPECT_TRUE(matches(*New));
  User->destroyConstant();
  EXPECT_FALSE(matches(*New));
  OldUser->destroyConstant();
}

TEST_F(DebugObjectSignatureTest, MatchesOnlyExistingSourceCoordinates) {
  LLVMContext Before, After, Inserted;
  auto Old = module(Before, OldSum, 3, "payload");
  auto Moved = module(After, NewSum, 4, "payload");
  auto Insertion = module(Inserted, NewSum, 3, "payload");
  ASSERT_TRUE(Old && Moved && Insertion);
  ASSERT_TRUE(ohm_bc_save_v1(wrap(Old.get()), &Origin, &OriginSize));
  EXPECT_TRUE(matches(*Moved));
  EXPECT_FALSE(matches(*Insertion));
  Proof.clear();
  EXPECT_FALSE(matches(*Moved));
}

TEST_F(DebugObjectSignatureTest, IgnoresAnUnreferencedReplacement) {
  LLVMContext Before, After;
  auto Old = module(Before, OldSum, 3, "same");
  auto New = module(After, NewSum, 3, "same");
  ASSERT_TRUE(Old && New);
  train(*Old);
  partialMap("[[1,1],[3,3]]", 3, 3);
  EXPECT_TRUE(matches(*New));
}

TEST_F(DebugObjectSignatureTest, RejectsSameOrdinalInReplacedSource) {
  LLVMContext Before, After;
  auto Old = module(Before, OldSum, 2, "same");
  auto New = module(After, NewSum, 2, "same");
  ASSERT_TRUE(Old && New);
  train(*Old);
  partialMap("[[1,1],[3,3]]", 3, 3);
  EXPECT_FALSE(matches(*New));
}

TEST_F(DebugObjectSignatureTest, MovesLinesBackAfterADeletion) {
  LLVMContext Before, After;
  auto Old = module(Before, OldSum, 4, "same");
  auto New = module(After, NewSum, 2, "same");
  ASSERT_TRUE(Old && New);
  train(*Old);
  partialMap("[[1,1],[4,2]]", 4, 2);
  EXPECT_TRUE(matches(*New));
}

TEST_F(DebugObjectSignatureTest, RejectsInvalidPartialMappings) {
  LLVMContext Before, After;
  auto Old = module(Before, OldSum, 3, "same");
  auto New = module(After, NewSum, 4, "same");
  ASSERT_TRUE(Old && New);
  train(*Old);
  for (StringRef Lines : {"[[1,1],[2,3],[3,4],[4,2]]",
                          "[[1,1],[2,3],[3,4],[4,3]]",
                          "[[1,1],[2,2],[2,3],[3,4]]",
                          "[[1,1],[3,4],[5,5]]",
                          "[[0,0],[1,1],[3,4]]"}) {
    SCOPED_TRACE(Lines.str());
    partialMap(Lines, 4, 4);
    EXPECT_FALSE(matches(*New));
  }
}

TEST_F(DebugObjectSignatureTest, RejectsChecksumAlsoUsedAsOrdinaryMetadata) {
  LLVMContext Before, After;
  auto Old = module(Before, OldSum, 3, OldSum);
  auto Moved = module(After, NewSum, 4, NewSum);
  ASSERT_TRUE(Old && Moved);
  ASSERT_TRUE(ohm_bc_save_v1(wrap(Old.get()), &Origin, &OriginSize));
  EXPECT_FALSE(matches(*Moved));
}
TEST_F(DebugObjectSignatureTest, ChecksRuntimeLocationAgainstOriginalObject) {
  LLVMContext C1, C2;
  auto A = module(C1, OldSum, 3, "payload");
  auto B = module(C2, NewSum, 4, "payload");
  ASSERT_TRUE(A && B);
  location(*A, 3);
  location(*B, 4);
  train(*A);
  EXPECT_FALSE(matches(*B));
  std::string Records;
  EXPECT_EQ(relocatable(*B, accept, &Records), 2);
  EXPECT_NE(Records.find("\"old_line\":3"), std::string::npos);
  EXPECT_NE(Records.find("\"new_line\":4"), std::string::npos);
  EXPECT_EQ(relocatable(*B, decline, nullptr), 0);
}

TEST_F(DebugObjectSignatureTest, PreservesAnUnchangedRuntimeWord) {
  LLVMContext C1, C2;
  auto A = module(C1, OldSum, 3, "payload");
  auto B = module(C2, NewSum, 4, "payload");
  ASSERT_TRUE(A && B);
  // The literal already has value four in the origin and in the current IR.
  // Its old ELF cannot confirm the proposed 3 -> 4 update.
  location(*A, 4);
  location(*B, 4);
  train(*A);
  EXPECT_EQ(relocatable(*B, decline, nullptr), 1);
}

TEST_F(DebugObjectSignatureTest, RejectsAnUnrelatedSharedRuntimeWord) {
  LLVMContext C1, C2;
  auto A = module(C1, OldSum, 3, "payload");
  auto B = module(C2, NewSum, 4, "payload");
  ASSERT_TRUE(A && B);
  location(*A, 3, true);
  location(*B, 4, true);
  train(*A);
  std::string Records;
  EXPECT_EQ(relocatable(*B, accept, &Records), 0);
}

} // namespace
