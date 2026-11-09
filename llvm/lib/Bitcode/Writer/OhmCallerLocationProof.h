// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#ifndef LLVM_LIB_BITCODE_WRITER_OHMCALLERLOCATIONPROOF_H
#define LLVM_LIB_BITCODE_WRITER_OHMCALLERLOCATIONPROOF_H

#include "DebugObjectSourceMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/JSON.h"
#include <set>

namespace llvm {
namespace ohm_location {

struct Record {
  const GlobalVariable *File;
  const ConstantDataArray *Tail;
  StringRef Path;
  bool HasColumn;
};

inline bool immutable(const GlobalVariable &G) {
  return G.hasPrivateLinkage() && G.hasGlobalUnnamedAddr() && G.isConstant() &&
         G.hasInitializer() && !G.isThreadLocal() &&
         !G.isExternallyInitialized() && !G.hasSection() && !G.hasComdat() &&
         !G.getAddressSpace() && !G.hasMetadataOtherThanDebugLocAndGuid() &&
         !G.getMetadata(LLVMContext::MD_dbg) && !G.isUsedByMetadata();
}

inline std::optional<Record> readRecord(const GlobalVariable &G) {
  if (!immutable(G) || G.getAlign().valueOrOne() != Align(8))
    return std::nullopt;
  auto *S = dyn_cast<ConstantStruct>(G.getInitializer());
  if (!S || !S->getType()->isPacked() ||
      (S->getNumOperands() != 2 && S->getNumOperands() != 3))
    return std::nullopt;
  bool HasColumn = S->getNumOperands() == 2;
  if (!HasColumn) {
    auto *Padding = S->getOperand(2);
    auto *Ty = dyn_cast<ArrayType>(Padding->getType());
    if (!isa<UndefValue>(Padding) || isa<PoisonValue>(Padding) || !Ty ||
        Ty->getNumElements() != 4 || !Ty->getElementType()->isIntegerTy(8))
      return std::nullopt;
  }
  auto *File = dyn_cast<GlobalVariable>(S->getOperand(0));
  auto *Tail = dyn_cast<ConstantDataArray>(S->getOperand(1));
  if (!File || !immutable(*File) || File->getAlign().valueOrOne() != Align(1) ||
      !Tail || !Tail->isString() ||
      Tail->getNumElements() != (HasColumn ? 16 : 12))
    return std::nullopt;
  auto *Name = dyn_cast<ConstantDataArray>(File->getInitializer());
  if (!Name || !Name->isString() || Name->isCString() != HasColumn)
    return std::nullopt;
  StringRef Path = Name->getAsString().drop_back(HasColumn ? 1 : 0);
  if (Path.contains(char(0)))
    return std::nullopt;
  if (Path.empty() || Path.size() !=
                          support::endian::read64le(Tail->getAsString().data()))
    return std::nullopt;
  return Record{File, Tail, Path, HasColumn};
}

// For a normal Rust declaration, the x86 C ABI passes this operand as the
// address of the global. A memcpy, a known C library routine, an aggregate ABI
// argument, or arbitrary IR may inspect the initializer during lowering.
inline bool pointerAttributes(AttributeSet Attrs) {
  for (const Attribute &A : Attrs) {
    if (A.isStringAttribute())
      return false;
    switch (A.getKindAsEnum()) {
    case Attribute::Alignment:
    case Attribute::Captures:
    case Attribute::Dereferenceable:
    case Attribute::DereferenceableOrNull:
    case Attribute::NoAlias:
    case Attribute::NoFree:
    case Attribute::NoUndef:
    case Attribute::NonNull:
    case Attribute::ReadNone:
    case Attribute::ReadOnly:
    case Attribute::WriteOnly:
      break;
    default:
      return false;
    }
  }
  return true;
}

inline bool addressOnly(const GlobalVariable &G) {
  if (G.use_empty())
    return false;
  for (const Use &U : G.uses()) {
    auto *C = dyn_cast<CallBase>(U.getUser());
    if (!C || (!isa<CallInst>(C) && !isa<InvokeInst>(C)) ||
        C->hasOperandBundles() || !C->isArgOperand(&U) ||
        C->getCallingConv() != CallingConv::C)
      return false;
    auto *F = C->getCalledFunction();
    if (!F || !F->isDeclaration() || F->isIntrinsic() ||
        !F->getName().starts_with("_R") || F->isVarArg() ||
        F->getCallingConv() != CallingConv::C)
      return false;
    unsigned I = C->getArgOperandNo(&U);
    if (I >= F->arg_size() || !F->getFunctionType()->getParamType(I)->isPointerTy() ||
        !pointerAttributes(C->getAttributes().getParamAttrs(I)) ||
        !pointerAttributes(F->getAttributes().getParamAttrs(I)))
      return false;
  }
  return true;
}

// A local one-instruction function returning the address through a constant
// function table has no direct IR caller and never loads the record. The ELF
// checker independently requires a single native LEA relocation to the record.
inline bool returnedAddress(const GlobalVariable &G) {
  if (!G.hasOneUse())
    return false;
  auto *Ret = dyn_cast<ReturnInst>(*G.user_begin());
  if (!Ret || Ret->getReturnValue() != &G)
    return false;
  auto *F = Ret->getFunction();
  if (!F->hasLocalLinkage() || !F->hasGlobalUnnamedAddr() || F->isVarArg() ||
      F->getCallingConv() != CallingConv::C || F->size() != 1 ||
      F->getEntryBlock().size() != 1 || !F->hasOneUse() || F->isUsedByMetadata())
    return false;
  auto *Table = dyn_cast<ConstantStruct>(*F->user_begin());
  if (!Table || !Table->hasOneUse() || Table->isUsedByMetadata())
    return false;
  auto *TableGlobal = dyn_cast<GlobalVariable>(*Table->user_begin());
  return TableGlobal && immutable(*TableGlobal) &&
         TableGlobal->getInitializer() == Table;
}
inline bool prepare(const Module &M, DebugObjectSourceMap &Map,
                    json::Array &Changes) {
  if (!M.getTargetTriple().isX86() || !M.getTargetTriple().isOSLinux() ||
      !M.getDataLayout().isLittleEndian() ||
      M.getDataLayout().getPointerSizeInBits() != 64)
    return false;
  auto *CUs = M.getNamedMetadata("llvm.dbg.cu");
  if (!CUs || CUs->getNumOperands() != 1)
    return false;
  auto *CU = dyn_cast<DICompileUnit>(CUs->getOperand(0));
  if (!CU || CU->getEmissionKind() != DICompileUnit::LineTablesOnly)
    return false;
  std::set<std::pair<std::string, std::string>> OldKeys, NewKeys;
  for (const GlobalVariable &G : M.globals()) {
    auto R = readRecord(G);
    if (!R)
      continue;
    auto First = Map.Files.find({CU->getDirectory().str(), R->Path.str()});
    if (First == Map.Files.end())
      continue;
    StringRef Bytes = R->Tail->getAsString();
    uint32_t Current = support::endian::read32le(Bytes.data() + 8);
    std::string Prior = Bytes.str();
    const auto &Lines = First->second.Lines;
    uint32_t Previous = Current < Lines.size() ? Lines[Current] : 0;
    if (Previous && Previous != Current) {
      if (!(R->HasColumn ? addressOnly(G) : returnedAddress(G)) ||
          !G.getInitializer()->hasOneUse())
        return false;
      support::endian::write32le(Prior.data() + 8, Previous);
      if (G.getInitializer()->isUsedByMetadata())
        return false;
      Map.CallerInitializers.insert(G.getInitializer());
      auto It = Map.CallerTails.try_emplace(R->Tail, Prior);
      if (!It.second && It.first->second != Prior)
        return false;
      Changes.push_back(json::Object{
          {"global", G.getName()}, {"file_global", R->File->getName()},
          {"directory", CU->getDirectory()}, {"file", R->Path}, {"file_nul", R->HasColumn},
          {"old_line", Previous}, {"new_line", Current},
          {"column", R->HasColumn
                         ? json::Value(support::endian::read32le(Bytes.data() + 12))
                         : json::Value(nullptr)}});
    }
    if (!OldKeys.emplace(R->File->getName().str(), Prior).second ||
        !NewKeys.emplace(R->File->getName().str(), Bytes.str()).second)
      return false;
  }
  // ConstantData has no LLVM use-list. ValueEnumerator checks every serialized
  // ordinary value reference and permits the selected bytes only in operand 1
  // of these exact global initializers, including references from metadata.
  return !Changes.empty();
}
} // namespace ohm_location
} // namespace llvm
#endif
