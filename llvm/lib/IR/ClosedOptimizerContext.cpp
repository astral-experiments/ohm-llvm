//===- ClosedOptimizerContext.cpp - Account for live optimizer inputs -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/ClosedOptimizerContext.h"
#include "LLVMContextImpl.h"
#include "llvm/IR/DiagnosticHandler.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/OptBisect.h"
#include "llvm/Support/BLAKE3.h"
#include "llvm/Support/Endian.h"

using namespace llvm;

bool ReplaceableMetadataImpl::getRepresentedUserOrder(
    Metadata &MD, const DenseMap<Value *, unsigned> &Values,
    const DenseMap<Metadata *, unsigned> &Nodes, SmallVectorImpl<uint64_t> &Order) {
  Order.clear();
  const auto *Uses = getIfExists(MD);
  if (!Uses)
    return true;
  SmallVector<std::tuple<uint64_t, uint64_t, unsigned>, 4> Ordered;
  for (const auto &Entry : Uses->UseMap) {
    OwnerTy Owner = Entry.second.first;
    uint64_t ID;
    unsigned Slot = 0;
    if (auto *M = dyn_cast_if_present<Metadata *>(Owner)) {
      ID = Nodes.lookup(M);
      if (!ID)
        return false;
      const uintptr_t Ref = reinterpret_cast<uintptr_t>(Entry.first);
      if (auto *Args = dyn_cast<DIArgList>(M)) {
        auto A = Args->getArgs();
        const uintptr_t Begin = reinterpret_cast<uintptr_t>(A.data());
        if (Ref < Begin || (Ref - Begin) % sizeof(A[0]) ||
            (Ref - Begin) / sizeof(A[0]) >= A.size())
          return false;
        Slot = (Ref - Begin) / sizeof(A[0]);
        if (A[Slot] != &MD)
          return false;
      } else if (auto *N = dyn_cast<MDNode>(M)) {
        // This is the same operand that MDNode::handleChangedOperand updates.
        const uintptr_t Begin = reinterpret_cast<uintptr_t>(N->op_begin());
        if (Ref < Begin || (Ref - Begin) % sizeof(MDOperand) ||
            (Ref - Begin) / sizeof(MDOperand) >= N->getNumOperands())
          return false;
        Slot = (Ref - Begin) / sizeof(MDOperand);
        if (N->getOperand(Slot) != &MD)
          return false;
      } else {
        return false;
      }
    } else if (auto *V = dyn_cast_if_present<MetadataAsValue *>(Owner)) {
      ID = Values.lookup(V);
      if (!ID && (!V->use_empty() || V->isUsedByMetadata() || V->getMetadata() != &MD))
        return false;
      ID |= uint64_t(1) << 32;
    } else {
      return false;
    }
    Ordered.emplace_back(Entry.second.second, ID, Slot);
  }
  llvm::sort(Ordered);
  for (auto [Index, ID, Slot] : Ordered) {
    Order.push_back(ID);
    Order.push_back(Slot);
  }
  return true;
}

// rustc can leave an unreferenced slice constant that changes the use count and
// order of a represented address. Its entire remaining state is the fixed
// literal type and poison scalar. A function pointer or a nested user can
// affect optimization differently and must take the normal pipeline.
static bool isTerminalSlice(User *U, Value *Address) {
  const auto *S = dyn_cast<ConstantStruct>(U);
  if (!S || S->getNumOperands() != 2 || !S->getType()->isLiteral() ||
      S->getType()->isPacked() || S->getOperand(0) != Address ||
      !S->use_empty() || S->isUsedByMetadata() ||
      !Address->getType()->isPointerTy() ||
      Address->getType()->getPointerAddressSpace())
    return false;
  const auto *Length = dyn_cast<PoisonValue>(S->getOperand(1));
  if (!Length || !Length->getType()->isIntegerTy(64) || Length->hasUseList() ||
      Length->isUsedByMetadata())
    return false;
  if (auto *G = dyn_cast<GlobalVariable>(Address))
    return G->isConstant() && G->hasLocalLinkage();
  auto *E = dyn_cast<ConstantExpr>(Address);
  return E && E->getOpcode() == Instruction::IntToPtr &&
         isa<ConstantInt>(E->getOperand(0)) &&
         E->getOperand(0)->getType()->isIntegerTy(64);
}

namespace {
// Keep the original byte stream, but feed BLAKE3 whole chunks even when a
// length-prefixed string or use record straddles a block boundary.
class ContextHasher {
  static constexpr size_t BlockSize = 4096;
  // Room for a three-word use record starting at the last byte.
  std::array<uint8_t, BlockSize + 11> Buffer;
  size_t Used = 0;
  BLAKE3 Hash;

  LLVM_ATTRIBUTE_NOINLINE void flushBlock() {
    Hash.update(ArrayRef(Buffer.data(), BlockSize));
    Used -= BlockSize;
    memcpy(Buffer.data(), Buffer.data() + BlockSize, Used);
  }

  void advance(size_t Bytes) {
    Used += Bytes;
    if (LLVM_UNLIKELY(Used >= BlockSize))
      flushBlock();
  }

public:
  void word(uint32_t W) {
    support::endian::write32le(Buffer.data() + Used, W);
    advance(4);
  }

  void text(StringRef S) {
    word(S.size());
    while (!S.empty()) {
      size_t Bytes = std::min(BlockSize - Used, S.size());
      memcpy(Buffer.data() + Used, S.data(), Bytes);
      advance(Bytes);
      S = S.drop_front(Bytes);
    }
  }

  void use(unsigned ID, unsigned OperandNo) {
    support::endian::write32le(Buffer.data() + Used, 1);
    support::endian::write32le(Buffer.data() + Used + 4, ID);
    support::endian::write32le(Buffer.data() + Used + 8, OperandNo);
    advance(12);
  }

  void owner(uint64_t Owner) {
    support::endian::write64le(Buffer.data() + Used, Owner);
    advance(8);
  }

  BLAKE3Result<> final() {
    Hash.update(ArrayRef(Buffer.data(), Used));
    return Hash.final();
  }
};
} // namespace

bool llvm::getClosedOptimizerContextV1(const Module &Input,
                                       std::array<uint8_t, 32> &Digest) {
  // LLVM's context metadata accessors take mutable pointers. This walk does
  // not change an instruction, a constant, a use list or a metadata owner.
  auto &M = const_cast<Module &>(Input);
  auto &C = M.getContext();
  auto &I = *C.pImpl;
  const auto *Diag = C.getDiagHandlerPtr();
  if (!M.isMaterialized() || I.OwnedModules.size() != 1 || !I.OwnedModules.contains(&M) ||
      !I.MachineFunctionNums.empty() || !I.ValueHandles.empty() ||
      I.YieldCallback || (I.OPG && I.OPG != &getGlobalPassGate()) ||
      getGlobalPassGate().isEnabled() || !I.NamedStructTypes.empty() ||
      I.NamedStructTypesUniqueID || I.DITypeMap || !Diag || Diag->HasErrors ||
      Diag->isAnyRemarkEnabled() || C.getMainRemarkStreamer() ||
      C.getLLVMRemarkStreamer() || C.getDiagnosticsHotnessRequested() ||
      I.MisExpectWarningRequested)
    return false;

  DenseMap<Value *, unsigned> ValueIDs;
  DenseMap<Metadata *, unsigned> Seen;
  SmallVector<Value *, 128> Values;
  SmallVector<Metadata *, 64> Nodes;
  auto AddValue = [&](Value *V) {
    if (V && ValueIDs.try_emplace(V, Values.size() + 1).second)
      Values.push_back(V);
  };
  auto AddNode = [&](Metadata *N) {
    if (N && Seen.try_emplace(N, Nodes.size() + 1).second)
      Nodes.push_back(N);
  };
  auto Attachments = [&](auto &V) {
    SmallVector<std::pair<unsigned, MDNode *>, 4> A;
    V.getAllMetadata(A);
    for (auto P : A)
      AddNode(P.second);
  };
  for (auto &N : M.named_metadata())
    for (auto *MD : N.operands())
      AddNode(MD);
  for (auto &G : M.global_values()) {
    Values.push_back(&G);
    if (auto *O = dyn_cast<GlobalObject>(&G); O && O->hasMetadata())
      Attachments(*O);
    if (auto *F = dyn_cast<Function>(&G)) {
      for (auto &A : F->args())
        Values.push_back(&A);
      for (auto &BB : *F) {
        Values.push_back(&BB);
        for (auto &Inst : BB) {
          if (Inst.hasDbgRecords())
            return false;
          Values.push_back(&Inst);
          if (Inst.hasMetadataOtherThanDebugLoc())
            Attachments(Inst);
          else if (Inst.getDebugLoc())
            AddNode(Inst.getDebugLoc().get());
        }
      }
    }
  }
  // The root walk visits each definition exactly once. Populate the map
  // now that its minimum size is known, before following operand edges.
  ValueIDs.reserve(Values.size() + Values.size() / 2);
  for (unsigned J = 0; J != Values.size(); ++J)
    ValueIDs.try_emplace(Values[J], J + 1);
  Seen.reserve(Nodes.size() * 2);
  size_t VI = 0, MI = 0;
  while (VI != Values.size() || MI != Nodes.size()) {
    while (MI != Nodes.size()) {
      auto *MD = Nodes[MI++];
      if (auto *Args = dyn_cast<DIArgList>(MD))
        for (auto *A : Args->getArgs())
          AddNode(A);
      if (auto *N = dyn_cast<MDNode>(MD)) {
        if (!N->isResolved() || N->isTemporary())
          return false;
        for (auto &Op : N->operands())
          AddNode(Op.get());
      } else if (auto *V = dyn_cast<ValueAsMetadata>(MD)) {
        AddValue(V->getValue());
      }
    }
    while (VI != Values.size()) {
      auto *V = Values[VI++];
      if (auto *U = dyn_cast<User>(V))
        for (auto &Op : U->operands())
          AddValue(Op.get());
      if (auto *S = dyn_cast<ShuffleVectorInst>(V))
        AddValue(S->getShuffleMaskForBitcode());
      if (auto *E = dyn_cast<ConstantExpr>(V); E && E->getOpcode() == Instruction::ShuffleVector)
        AddValue(E->getShuffleMaskForBitcode());
      if (auto *MD = dyn_cast<MetadataAsValue>(V))
        AddNode(MD->getMetadata());
    }
  }

  // A fold can rediscover an aggregate from the same context. An orphan is
  // inert only when no one observes it and its scalar operands keep no uses.
  auto AccountedFor = [&](User *U) {
    if (ValueIDs.contains(U))
      return true;
    bool HasOperandUse = false;
    for (auto *V : U->operand_values()) {
      if (!V->hasUseList())
        continue;
      if (ValueIDs.contains(V))
        return true; // Its complete user list is checked below.
      HasOperandUse = true;
    }
    return !HasOperandUse && U->use_empty() && !U->isUsedByMetadata();
  };
  if (!llvm::all_of(I.ExprConstants, AccountedFor) ||
      !llvm::all_of(I.StructConstants, AccountedFor) ||
      !llvm::all_of(I.ArrayConstants, AccountedFor) ||
      !llvm::all_of(I.VectorConstants, AccountedFor) ||
      !llvm::all_of(I.ConstantPtrAuths, AccountedFor))
    return false;

  // A later fold can also retrieve a uniqued metadata node or scalar wrapper
  // which existed before the module was constructed. An unseen tracking edge
  // there is just as observable as an unseen user of a global variable.
  if (llvm::any_of(I.ValuesAsMetadata, [&](auto &E) { return !ValueIDs.contains(E.first); }) ||
      llvm::any_of(I.MetadataAsValues, [&](auto &E) { return !Seen.contains(E.first); }) ||
      llvm::any_of(I.DIArgLists, [&](auto *N) { return !Seen.contains(N); }))
    return false;
  SmallVector<uint64_t, 4> Owners;
  auto InertOrSeen = [&](Metadata *MD) {
    return Seen.contains(MD) ||
           (ReplaceableMetadataImpl::getRepresentedUserOrder(*MD, ValueIDs, Seen, Owners) && Owners.empty());
  };
#define HANDLE_MDNODE_LEAF_UNIQUABLE(CLASS)                                    \
  if (!llvm::all_of(I.CLASS##s, InertOrSeen))                                  \
    return false;
#include "llvm/IR/Metadata.def"

  // Unlike ordinary bitcode, include the complete live use order, including
  // metadata operands with no bitcode value number and the side link that
  // makes a BasicBlock address-taken. Numbers follow the stable root walk.
  ContextHasher Hash;
  Hash.text("LLVM closed optimizer context v1");
  Hash.text(I.DefaultTargetCPU);
  Hash.text(I.DefaultTargetFeatures);
  Hash.word(C.shouldDiscardValueNames());
  Hash.word(static_cast<uint32_t>(I.NextAtomGroup));
  Hash.word(static_cast<uint32_t>(I.NextAtomGroup >> 32));
  SmallVector<StringRef> Names;
  auto NamesInOrder = [&] {
    Hash.word(Names.size());
    for (StringRef N : Names)
      Hash.text(N);
    Names.clear();
  };
  C.getMDKindNames(Names);
  NamesInOrder();
  C.getOperandBundleTags(Names);
  NamesInOrder();
  C.getSyncScopeNames(Names);
  NamesInOrder();
  Hash.word(Values.size());
  for (auto *V : Values) {
    if (V->isUsedByMetadata() && !Seen.contains(ValueAsMetadata::getIfExists(V)))
      return false;
    if (auto *BB = dyn_cast<BasicBlock>(V); BB && BB->hasAddressTaken() &&
        !ValueIDs.contains(BlockAddress::lookup(BB)))
      return false;
    if (V->hasUseList()) {
      for (const Use &U : V->uses()) {
        unsigned ID = ValueIDs.lookup(U.getUser());
        if (!ID && !isTerminalSlice(U.getUser(), V))
          return false;
        Hash.use(ID, U.getOperandNo());
      }
    }
    Hash.word(0);
  }
  Hash.word(Nodes.size());
  for (auto *MD : Nodes) {
    if (!ReplaceableMetadataImpl::getRepresentedUserOrder(*MD, ValueIDs, Seen, Owners))
      return false;
    Hash.word(Owners.size());
    for (uint64_t Owner : Owners)
      Hash.owner(Owner);
    auto *V = I.MetadataAsValues.empty() ? nullptr
                                         : MetadataAsValue::getIfExists(C, MD);
    if (V && !ValueIDs.contains(V) && (!V->use_empty() || V->isUsedByMetadata()))
      return false;
    Hash.word(V != nullptr);
  }
  Digest = Hash.final();
  return true;
}
