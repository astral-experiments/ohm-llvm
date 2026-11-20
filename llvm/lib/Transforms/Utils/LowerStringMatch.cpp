//===- LowerStringMatch.cpp - Lower Rust string-to-enum matches
//------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// A string-to-enum parser can have hundreds of calls to the same slice
// equality helper, each with a different constant. Inlining all the calls
// grows the CFG before the scalar optimizers and instruction selection.
// Build a static hash table while the calls still expose the pattern.

#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/RustSymbolChecks.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/LowerSwitch.h"

using namespace llvm;
using namespace llvm::PatternMatch;

static bool canCombine(const CallInst &Call) {
  return !Call.hasOperandBundles() && !Call.isConvergent() &&
         !Call.cannotMerge() && !Call.cannotDuplicate() &&
         !Call.doesNotReturn() && !Call.hasFnAttr(Attribute::ReturnsTwice) &&
         !Call.hasFnAttr(Attribute::NoIPA) &&
         !Call.hasMetadataOtherThanDebugLoc();
}

static bool isByteComparison(const CallInst &Call,
                             const TargetLibraryInfo &TLI) {
  LibFunc LF;
  return TLI.getLibFunc(Call, LF) && TLI.has(LF) &&
         (LF == LibFunc_bcmp || LF == LibFunc_memcmp) &&
         Call.getCalledFunction()->isDeclaration() &&
         Call.getCallingConv() == CallingConv::C && canCombine(Call);
}

static PHINode *returnedPhi(BasicBlock *BB) {
  auto *Ret = dyn_cast<ReturnInst>(BB->getTerminator());
  if (!Ret || BB->size() != 2 || Ret->getReturnValue() != &BB->front())
    return nullptr;
  return dyn_cast<PHINode>(&BB->front());
}

// Match len == key.len && bcmp(input, key, len) == 0.
static bool isSliceEquality(Function &F, const TargetLibraryInfo &TLI) {
  if (!F.hasLocalLinkage() || F.size() != 3 || F.arg_size() != 4 ||
      F.isVarArg() || F.hasOptNone())
    return false;
  BasicBlock &Entry = F.getEntryBlock();
  auto *Br = dyn_cast<CondBrInst>(Entry.getTerminator());
  if (!Br || Entry.size() != 2 ||
      !match(Br->getCondition(),
             m_SpecificICmp(ICmpInst::ICMP_EQ, m_Specific(F.getArg(1)),
                            m_Specific(F.getArg(3)))))
    return false;
  BasicBlock *Compare = Br->getSuccessor(0), *End = Br->getSuccessor(1);
  auto *Call = dyn_cast<CallInst>(&Compare->front());
  if (Compare->size() != 3 || !isa<UncondBrInst>(Compare->getTerminator()) ||
      Compare->getSingleSuccessor() != End || !Call || Call->arg_size() != 3 ||
      Call->getArgOperand(0) != F.getArg(0) ||
      Call->getArgOperand(1) != F.getArg(2) ||
      Call->getArgOperand(2) != F.getArg(1) || !isByteComparison(*Call, TLI))
    return false;
  auto *Phi = returnedPhi(End);
  return Phi && match(Phi->getIncomingValueForBlock(&Entry), m_Zero()) &&
         match(Phi->getIncomingValueForBlock(Compare),
               m_SpecificICmp(ICmpInst::ICMP_EQ, m_Specific(Call), m_Zero()));
}

// The loop can inhibit propagation of a constant input through a caller.
// Avoid keeping references to undefined symbols that specialization of the
// original match could have eliminated. Check each calling root once.
static bool hasClosedReferences(Function &Root, FunctionAnalysisManager &FAM) {
  SmallVector<Constant *> Work{&Root};
  SmallPtrSet<Constant *, 32> Seen;
  while (!Work.empty()) {
    Constant *C = Work.pop_back_val();
    if (isa<ConstantData>(C) || !Seen.insert(C).second)
      continue;
    if (auto *GV = dyn_cast<GlobalValue>(C); GV && GV->isDeclarationForLinker())
      return false;
    if (auto *F = dyn_cast<Function>(C)) {
      auto &TLI = FAM.getResult<TargetLibraryAnalysis>(*F);
      for (Instruction &I : instructions(F)) {
        auto *Call = dyn_cast<CallInst>(&I);
        for (Use &U : I.operands()) {
          if (Call && Call->isCallee(&U) && isByteComparison(*Call, TLI))
            continue;
          if (auto *Op = dyn_cast<Constant>(U))
            Work.push_back(Op);
        }
      }
    }
    for (Value *Op : C->operands())
      if (auto *K = dyn_cast<Constant>(Op))
        Work.push_back(K);
  }
  return true;
}

static bool hasClosedCallers(Function &F, FunctionAnalysisManager &FAM,
                             DenseMap<Function *, bool> &Closed) {
  for (Use &U : F.uses()) {
    auto *Call = dyn_cast<CallInst>(U.getUser());
    if (!Call || !Call->isCallee(&U) || !Call->getFunction()->use_empty())
      return false;
    auto [It, New] = Closed.try_emplace(Call->getFunction());
    if (New)
      It->second = hasClosedReferences(*Call->getFunction(), FAM);
    if (!It->second)
      return false;
  }
  return true;
}

// Collect an ordered chain of slice comparisons returning 0..N-1 or -1.
// The last comparison selects between the final ordinal and the miss value.
static bool collectKeys(Function &F, CallInst &First,
                        SmallVectorImpl<Constant *> &Keys) {
  BasicBlock *BB = &F.getEntryBlock();
  auto *Br = dyn_cast<CondBrInst>(BB->getTerminator());
  if (!Br)
    return false;
  BasicBlock *End = Br->getSuccessor(0);
  PHINode *Phi = returnedPhi(End);
  if (!Phi || End->hasAddressTaken() ||
      Phi->getNumIncomingValues() + 1 != F.size())
    return false;
  DenseMap<BasicBlock *, Value *> Values;
  for (const Use &U : Phi->incoming_values())
    Values[Phi->getIncomingBlock(U)] = U.get();
  while (Keys.size() < Phi->getNumIncomingValues()) {
    auto *Test = dyn_cast<CallInst>(&BB->front());
    if (!Test || BB->hasAddressTaken() ||
        Test->getCalledFunction() != First.getCalledFunction() ||
        Test->getAttributes() != First.getAttributes() ||
        Test->getCallingConv() != First.getCallingConv() ||
        Test->hasOperandBundles() || Test->hasMetadataOtherThanDebugLoc() ||
        Test->getArgOperand(0) != F.getArg(0) ||
        Test->getArgOperand(1) != F.getArg(1))
      return false;
    auto *Key = dyn_cast<GlobalVariable>(Test->getArgOperand(2));
    if (!Key || !Key->isConstant() || !Key->hasPrivateLinkage() ||
        !Key->hasDefinitiveInitializer() || Key->isThreadLocal() ||
        Key->getAddressSpace())
      return false;
    auto *Data = dyn_cast<ConstantDataArray>(Key->getInitializer());
    if (!Data || !Data->isString() || Data->getNumElements() == 0 ||
        Data->getNumElements() > 63 ||
        !match(Test->getArgOperand(3), m_SpecificInt(Data->getNumElements())))
      return false;
    Value *V = Values.lookup(BB);
    if (!V)
      return false;
    unsigned Index = Keys.size();
    Keys.push_back(Key);
    if (auto *Br = dyn_cast<CondBrInst>(BB->getTerminator())) {
      if (BB->size() != 2 || Br->getCondition() != Test ||
          Br->getSuccessor(0) != End || !match(V, m_SpecificInt(Index)))
        return false;
      BB = Br->getSuccessor(1);
      continue;
    }
    return Keys.size() == Phi->getNumIncomingValues() && BB->size() == 3 &&
           isa<UncondBrInst>(BB->getTerminator()) &&
           BB->getSingleSuccessor() == End && V == Test->getNextNode() &&
           match(V,
                 m_Select(m_Specific(Test), m_SpecificInt(Index), m_AllOnes()));
  }
  return false;
}

static bool buildLookup(Function &F, CallInst &Call,
                        ArrayRef<Constant *> Keys) {
  // Use the length and three bytes to reject most candidates before
  // running the original comparison. Zero marks an empty bucket.
  unsigned Shift = 32 - Log2_64_Ceil(2 * Keys.size());
  uint32_t Mask = ~0u >> Shift;
  constexpr uint32_t Multiplier = 2654435761u;
  SmallVector<uint64_t> Buckets(Mask + 1, 0);
  uint64_t Lengths = 0;
  for (auto [Index, Key] : enumerate(Keys)) {
    auto *G = cast<GlobalVariable>(Key);
    StringRef S = cast<ConstantDataArray>(G->getInitializer())->getAsString();
    Lengths |= 1ull << S.size();
    uint32_t Tag =
        (uint32_t(S.size()) << 24) | (uint32_t(uint8_t(S.front())) << 16) |
        (uint32_t(uint8_t(S[S.size() / 2])) << 8) | uint8_t(S.back());
    unsigned Slot = (Tag * Multiplier) >> Shift;
    // Linear probing preserves the first match when keys repeat.
    unsigned Probes = 0;
    while (Buckets[Slot]) {
      if (++Probes == 64)
        return false;
      Slot = (Slot + 1) & Mask;
    }
    Buckets[Slot] = (uint64_t(Index) << 32) | Tag;
  }

  LLVMContext &Ctx = F.getContext();
  IRBuilder<> B(Ctx);
  DebugLoc Loc = Call.getDebugLoc();
  for (BasicBlock &BB : F)
    if (auto *Test = dyn_cast<CallInst>(&BB.front()))
      Loc = DebugLoc::getMergedLocation(Loc, Test->getDebugLoc());
  B.SetCurrentDebugLocation(Loc);
  auto Table = [&](Constant *Data) {
    auto *G = new GlobalVariable(*F.getParent(), Data->getType(), true,
                                 GlobalValue::PrivateLinkage, Data,
                                 "match.table." + F.getName());
    G->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    return G;
  };
  auto *Records =
      Table(ConstantDataArray::get(Ctx, ArrayRef<uint64_t>(Buckets)));
  auto *Entry = BasicBlock::Create(Ctx, "entry", &F, &F.getEntryBlock());
  auto Block = [&](StringRef Name) {
    return BasicBlock::Create(Ctx, Name, &F);
  };
  auto *Hash = Block("hash"), *Loop = Block("probe"), *Test = Block("test");
  auto *Found = Block("found"), *Missing = Block("missing"),
       *Next = Block("next");

  // The equality helper reads bytes only at lengths present in the chain.
  // Test that set before the first load, including for lengths above 63.
  B.SetInsertPoint(Entry);
  Value *Length = F.getArg(1);
  Value *Allowed = B.CreateLShr(B.getInt64(Lengths), B.CreateAnd(Length, 63));
  Value *Valid = B.CreateAnd(B.CreateICmpULT(Length, B.getInt64(64)),
                             B.CreateTrunc(Allowed, B.getInt1Ty()));
  B.CreateCondBr(Valid, Hash, Missing);

  B.SetInsertPoint(Hash);
  auto Byte = [&](Value *I) {
    Value *P = B.CreateGEP(B.getInt8Ty(), F.getArg(0), I);
    Value *V = B.CreateFreeze(B.CreateLoad(B.getInt8Ty(), P));
    return B.CreateZExt(V, B.getInt32Ty());
  };
  Value *Tag = B.CreateShl(B.CreateTrunc(Length, B.getInt32Ty()), 24);
  Tag = B.CreateOr(Tag, B.CreateShl(Byte(B.getInt64(0)), 16));
  Tag = B.CreateOr(Tag, B.CreateShl(Byte(B.CreateLShr(Length, 1)), 8));
  Tag = B.CreateOr(Tag, Byte(B.CreateSub(Length, B.getInt64(1))));
  Value *Start = B.CreateLShr(B.CreateMul(Tag, B.getInt32(Multiplier)), Shift);
  B.CreateBr(Loop);

  B.SetInsertPoint(Loop);
  auto *Slot = B.CreatePHI(B.getInt32Ty(), 2);
  Slot->addIncoming(Start, Hash);
  Value *RecordPtr = B.CreateInBoundsGEP(B.getInt64Ty(), Records, Slot);
  Value *Record = B.CreateLoad(B.getInt64Ty(), RecordPtr);
  B.CreateCondBr(B.CreateIsNull(Record), Missing, Test);

  // At lengths up to three the tag includes every byte of the input.
  B.SetInsertPoint(Test);
  BasicBlock *Compare = (Lengths >> 4) ? Block("compare") : Found;
  Value *Index = B.CreateLShr(Record, 32);
  Value *Stored = B.CreateTrunc(Record, B.getInt32Ty());
  B.CreateCondBr(B.CreateICmpEQ(Stored, Tag), Compare, Next);
  if (Compare != Found) {
    auto *Pointers = Table(
        ConstantArray::get(ArrayType::get(B.getPtrTy(), Keys.size()), Keys));
    B.SetInsertPoint(Compare);
    Value *Ptr = B.CreateInBoundsGEP(B.getPtrTy(), Pointers, Index);
    auto *Eq = cast<CallInst>(Call.clone());
    Eq->setDebugLoc(Loc);
    Eq->setArgOperand(2, B.CreateLoad(B.getPtrTy(), Ptr));
    Eq->setArgOperand(3, Length);
    B.Insert(Eq);
    B.CreateCondBr(Eq, Found, Next);
  }
  B.SetInsertPoint(Next);
  Slot->addIncoming(B.CreateAnd(B.CreateAdd(Slot, B.getInt32(1)), Mask), Next);
  B.CreateBr(Loop);

  B.SetInsertPoint(Found);
  B.CreateRet(B.CreateTrunc(Index, F.getReturnType()));
  B.SetInsertPoint(Missing);
  B.CreateRet(ConstantInt::getAllOnesValue(F.getReturnType()));
  removeUnreachableBlocks(F);
  return true;
}

PreservedAnalyses LowerStringMatchPass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  DenseMap<Function *, bool> Closed;
  bool Changed = false;
  for (Function &F : M) {
    if (F.empty() || !F.hasExternalLinkage() || F.hasOptNone() ||
        F.isVarArg() || !F.getName().starts_with("_R") || F.arg_size() != 2 ||
        F.getArg(0)->getType() != PointerType::getUnqual(M.getContext()) ||
        !F.getArg(1)->getType()->isIntegerTy(64) ||
        !F.hasParamAttribute(0, Attribute::NoUndef) ||
        !F.hasParamAttribute(1, Attribute::NoUndef) ||
        !F.getReturnType()->isIntegerTy() ||
        F.getReturnType()->getIntegerBitWidth() > 32 || F.size() <= 32 ||
        F.size() > 65536)
      continue;
    auto *Call = dyn_cast<CallInst>(&F.getEntryBlock().front());
    if (!Call || !canCombine(*Call))
      continue;
    auto *Helper = Call->getCalledFunction();
    if (!Helper || !Helper->hasLocalLinkage() ||
        !isSliceEquality(*Helper,
                         FAM.getResult<TargetLibraryAnalysis>(*Helper)))
      continue;
    SmallVector<Constant *> Keys;
    if (!collectKeys(F, *Call, Keys) || !hasClosedCallers(F, FAM, Closed))
      continue;
    if (!Changed && !canIntroduceRustSymbols(M))
      return PreservedAnalyses::all();
    Changed |= buildLookup(F, *Call, Keys);
  }
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
