//===- OhmDebugObjectCacheTest.cpp - Incremental object admission tests ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../../lib/CodeGen/OhmDebugObjectCache.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Bitcode/DwarfLineCertificate.h"
#include "llvm/Bitcode/OhmDebugObjectSignature.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/OhmOptimizerState.h"
#include "llvm/IR/ValueHandle.h"
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

TEST(OhmDebugObjectCache, AdmissionCountsManyShortBlocks) {
  LLVMContext C;
  Module M("many-blocks", C);
  auto *Ty = FunctionType::get(Type::getVoidTy(C), false);
  auto AddBlock = [&] {
    auto *F = Function::Create(Ty, GlobalValue::ExternalLinkage, "f", M);
    IRBuilder<>(BasicBlock::Create(C, "entry", F)).CreateRetVoid();
  };
  for (unsigned I = 0; I < 2047; ++I)
    AddBlock();
  EXPECT_FALSE(ohm::isLargeDebugObjectInput(M));
  AddBlock();
  EXPECT_TRUE(ohm::isLargeDebugObjectInput(M));

  auto Callee = M.getOrInsertFunction("opaque", Ty);
  IRBuilder<> B(M.begin()->getEntryBlock().getTerminator());
  for (unsigned I = 0; I < 3 * 2048; ++I)
    B.CreateCall(Callee);
  EXPECT_TRUE(ohm::isLargeDebugObjectInput(M));
  B.CreateCall(Callee);
  EXPECT_FALSE(ohm::isLargeDebugObjectInput(M));
}

TEST(OhmDebugObjectCache, ExactObjectAdmissionHasLowerInstructionFloor) {
  LLVMContext C;
  Module M("small", C);
  auto *Ty = FunctionType::get(Type::getVoidTy(C), false);
  auto Callee = M.getOrInsertFunction("opaque", Ty);
  auto *F = Function::Create(Ty, GlobalValue::ExternalLinkage, "f", M);
  IRBuilder<> B(BasicBlock::Create(C, "entry", F));
  for (unsigned I = 0; I < 2046; ++I)
    B.CreateCall(Callee);
  auto *Ret = B.CreateRetVoid();
  EXPECT_FALSE(ohm::isLargeDebugObjectInput(M, true));
  B.SetInsertPoint(Ret);
  B.CreateCall(Callee);
  EXPECT_TRUE(ohm::isLargeDebugObjectInput(M, true));
  EXPECT_FALSE(ohm::isLargeDebugObjectInput(M));
}

TEST(OhmDebugObjectCache, ExactObjectInputKeepsUseListOrder) {
  LLVMContext C;
  Module M("order", C);
  auto *Ty = FunctionType::get(Type::getVoidTy(C), false);
  auto Callee = M.getOrInsertFunction("opaque", Ty);
  auto *F = Function::Create(Ty, GlobalValue::ExternalLinkage, "f", M);
  IRBuilder<> B(BasicBlock::Create(C, "entry", F));
  B.CreateCall(Callee);
  B.CreateCall(Callee);
  B.CreateRetVoid();
  auto Input = [&] {
    char *Bytes = nullptr;
    size_t Length = 0;
    EXPECT_EQ(1, ohm_bc_save_v1(wrap(&M), &Bytes, &Length));
    std::string S(Bytes, Length);
    ohm_bc_free_v1(Bytes);
    return S;
  };
  std::string Before = Input();
  Callee.getCallee()->reverseUseList();
  EXPECT_NE(Before, Input());
  Callee.getCallee()->reverseUseList();
  EXPECT_EQ(Before, Input());
}

TEST(OhmEarlyObject, ContextWrappersAreIncludedInInput) {
  LLVMContext C;
  Module M("early", C);
  auto *N = MDNode::get(C, MDString::get(C, "origin"));
  M.getOrInsertNamedMetadata("test")->addOperand(N);
  SmallVector<char> Before, After;
  ASSERT_TRUE(ohm::getOptimizerContextState(M, Before));
  MetadataAsValue::get(C, N);
  ASSERT_TRUE(ohm::getOptimizerContextState(M, After));
  EXPECT_NE(Before, After);
}

TEST(OhmEarlyObject, UnrepresentedMetadataOwnerDeclines) {
  LLVMContext C;
  Module M("early", C);
  auto *V = ConstantInt::get(Type::getInt32Ty(C), 3);
  auto *MD = ConstantAsMetadata::get(V);
  auto *Root = MDNode::get(C, MD);
  M.getOrInsertNamedMetadata("test")->addOperand(Root);
  SmallVector<char> Bits;
  ASSERT_TRUE(ohm::getOptimizerContextState(M, Bits));
  MDNode::getDistinct(C, {MD, MDString::get(C, "unrepresented")});
  Bits.clear();
  EXPECT_FALSE(ohm::getOptimizerContextState(M, Bits));
}

TEST(OhmEarlyObject, ExternalContextStateDeclines) {
  LLVMContext C;
  Module M("early", C);
  auto *F = Function::Create(FunctionType::get(Type::getVoidTy(C), false),
                             GlobalValue::ExternalLinkage, "f", M);
  SmallVector<char> Bits;
  ASSERT_TRUE(ohm::getOptimizerContextState(M, Bits));
  {
    WeakTrackingVH Handle(F);
    EXPECT_FALSE(ohm::getOptimizerContextState(M, Bits));
  }
  ValueAsMetadata::get(F);
  EXPECT_FALSE(ohm::getOptimizerContextState(M, Bits));
  LLVMContext T;
  Module Other("early", T);
  StructType::create(T, "unreferenced");
  EXPECT_FALSE(ohm::getOptimizerContextState(Other, Bits));
}

#if defined(__linux__) && defined(__x86_64__)
namespace {
constexpr StringLiteral Prior = "_RNvXsh_C4demoNtB5_5TokenNtB5_2Eq2eq";
constexpr StringLiteral Edited = "_RNvXsf_C4demoNtB5_5TokenNtB5_2Eq2eq";
constexpr StringLiteral Stable = "_RNvXs0_C4demoNtB5_5TokenNtB5_2Eq2eq";
constexpr StringLiteral Other = "_RNvXs3_C4demoNtB5_5OtherNtB5_2Eq2eq";

std::unique_ptr<Module> createComparisons(
    LLVMContext &C, ArrayRef<std::pair<StringRef, unsigned>> Methods,
    DICompileUnit::DebugEmissionKind Kind = DICompileUnit::LineTablesOnly) {
  auto M = std::make_unique<Module>("demo", C);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  DIBuilder Debug(*M);
  auto *File = Debug.createFile("token.rs", "/src");
  Debug.createCompileUnit(dwarf::DW_LANG_Rust, File, "test", true, "", 0, "",
                          Kind);
  auto *Crate = Debug.createNameSpace(nullptr, "demo", false);
  auto *Sig = Debug.createSubroutineType(Debug.getOrCreateTypeArray({}));
  Type *Ptr = PointerType::getUnqual(C);
  auto *Ty = FunctionType::get(Type::getInt1Ty(C), {Ptr, Ptr}, false);
  for (auto [Name, Ordinal] : Methods) {
    auto *F = Function::Create(Ty, GlobalValue::InternalLinkage, Name, *M);
    auto *Scope = Debug.createNameSpace(
        Crate, ("{impl#" + Twine(Ordinal) + "}").str(), false);
    auto *SP =
        Debug.createFunction(Scope, "eq", Name, File, 10, Sig, 10,
                             DINode::FlagZero, DISubprogram::SPFlagDefinition);
    F->setSubprogram(SP);
    IRBuilder<> B(BasicBlock::Create(C, "entry", F));
    B.SetCurrentDebugLocation(DILocation::get(C, 10, 1, SP));
    auto *L = B.CreateLoad(B.getInt8Ty(), F->getArg(0));
    auto *R = B.CreateLoad(B.getInt8Ty(), F->getArg(1));
    B.CreateRet(B.CreateICmpEQ(L, R));
  }
  Debug.finalize();
  return M;
}

} // namespace

TEST(OhmLocalTraitComparisons, GivesRenumberedMethodsTheSameLineCertificate) {
  LLVMContext C, D;
  auto A = createComparisons(C, {{Prior, 19}, {Other, 5}});
  auto B = createComparisons(D, {{Other, 5}, {Edited, 17}});
  for (auto *M : {A.get(), B.get()}) {
    ohm::prepareLocalTraitComparisons(*M);
    auto *F = M->getFunction(Stable);
    ASSERT_TRUE(F);
    EXPECT_EQ(Stable, F->getSubprogram()->getLinkageName());
    auto *NS = cast<DINamespace>(F->getSubprogram()->getScope());
    EXPECT_EQ("{impl#2}", NS->getName());
    EXPECT_EQ("demo", NS->getScope()->getName());
  }
  SmallString<0> Old, New;
  raw_svector_ostream Before(Old), After(New);
  ASSERT_TRUE(writeClosedDwarfLineCertificateV1(*A, Before));
  ASSERT_TRUE(writeClosedDwarfLineCertificateV1(*B, After));
  EXPECT_EQ(Old, New);
}

TEST(OhmLocalTraitComparisons, KeepsBothCollidingMethods) {
  LLVMContext C;
  auto M = createComparisons(C, {{Prior, 19}, {Edited, 17}});
  ohm::prepareLocalTraitComparisons(*M);
  EXPECT_TRUE(M->getFunction(Prior));
  EXPECT_TRUE(M->getFunction(Edited));
  EXPECT_FALSE(M->getNamedValue(Stable));
}

TEST(OhmLocalTraitComparisons, RespectsExistingSymbol) {
  for (const auto &Name : {Stable.str(), (Twine("\1") + Stable).str()}) {
    LLVMContext C;
    auto M = createComparisons(C, {{Prior, 19}});
    auto *G = M->getOrInsertGlobal(Name, Type::getInt8Ty(C));
    ohm::prepareLocalTraitComparisons(*M);
    EXPECT_TRUE(M->getFunction(Prior));
    EXPECT_EQ(G, M->getNamedGlobal(Name));
  }
}

TEST(OhmLocalTraitComparisons, RequiresMatchingNamespaceAndLineTables) {
  LLVMContext C, D;
  auto Namespace = createComparisons(C, {{Prior, 17}});
  auto Full = createComparisons(D, {{Prior, 19}}, DICompileUnit::FullDebug);
  for (auto *M : {Namespace.get(), Full.get()}) {
    ohm::prepareLocalTraitComparisons(*M);
    EXPECT_TRUE(M->getFunction(Prior));
  }
}

TEST(OhmLocalTraitComparisons, PreservesPrivateAssemblyPrefix) {
  LLVMContext C;
  auto M = createComparisons(C, {{Prior, 19}});
  M->getFunction(Prior)->setLinkage(GlobalValue::PrivateLinkage);
  ohm::prepareLocalTraitComparisons(*M);
  EXPECT_TRUE(M->getFunction(Prior));
}

TEST(OhmLocalTraitComparisons, PreservesPublicAndAddressTakenMethods) {
  LLVMContext C;
  auto M = createComparisons(C, {{Prior, 19}, {Other, 5}});
  M->getFunction(Prior)->setLinkage(GlobalValue::ExternalLinkage);
  auto *F = M->getFunction(Other);
  new GlobalVariable(*M, F->getType(), true, GlobalValue::ExternalLinkage, F);
  ohm::prepareLocalTraitComparisons(*M);
  EXPECT_TRUE(M->getFunction(Prior));
  EXPECT_TRUE(M->getFunction(Other));
}

TEST(OhmLocalTraitComparisons, RequiresSimpleLoadsAndEquality) {
  LLVMContext C;
  auto M = createComparisons(C, {{Prior, 19}, {Other, 5}});
  cast<LoadInst>(M->getFunction(Prior)->front().front()).setVolatile(true);
  auto *I = M->getFunction(Other)->front().getTerminator()->getPrevNode();
  cast<ICmpInst>(I)->setPredicate(CmpInst::ICMP_ULT);
  ohm::prepareLocalTraitComparisons(*M);
  EXPECT_TRUE(M->getFunction(Prior));
  EXPECT_TRUE(M->getFunction(Other));
}

TEST(OhmLocalTraitComparisons, PreservesSymbolsVisibleToAssembly) {
  LLVMContext C, D;
  auto Global = createComparisons(C, {{Prior, 19}});
  auto Local = createComparisons(D, {{Prior, 19}});
  std::string Assembly = (".quad " + Twine(Prior)).str();
  Global->setModuleInlineAsm(Assembly);
  auto *Ty = FunctionType::get(Type::getVoidTy(D), false);
  auto *F = Function::Create(Ty, GlobalValue::ExternalLinkage, "other", *Local);
  IRBuilder<> B(BasicBlock::Create(D, "entry", F));
  B.CreateCall(InlineAsm::get(Ty, Assembly, "", true));
  B.CreateRetVoid();
  for (auto *M : {Global.get(), Local.get()}) {
    ohm::prepareLocalTraitComparisons(*M);
    EXPECT_TRUE(M->getFunction(Prior));
  }
}
#endif
