//===- OhmLeafObject.cpp - Reuse objects across enum edits ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Reassigning enum variants can renumber functions returning a discriminant.
// Compare the complete optimizer input with just the returned literals masked.
// Each changed return must occupy its own mov/ret section, with no aliases
// or incoming code/data relocations.
//
//===----------------------------------------------------------------------===//

#include "OhmLeafObject.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Bitcode/ClosedModuleHash.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/BLAKE3.h"
#include "llvm/Support/Endian.h"

using namespace llvm;

namespace {

using Digest = std::array<uint8_t, 32>;

StringRef bytes(const Digest &D) {
  return {reinterpret_cast<const char *>(D.data()), D.size()};
}

Digest digest(StringRef S) {
  return BLAKE3::hash<32>(arrayRefFromStringRef(S));
}

Digest binding(StringRef Proof, StringRef Object) {
  BLAKE3 Hash;
  Hash.update("ohm-enum-object-v1");
  Hash.update(digest(Proof));
  Hash.update(Object);
  return Hash.final();
}

bool isLargeTag(const APInt &Lower, const APInt &Upper) {
  return Upper.getBitWidth() == 16 && Lower.isZero() && Upper.ugt(256) &&
         Upper.isNonNegative();
}

Attribute tagRange(const Function &F) {
  if (!F.getReturnType()->isIntegerTy(16) || !F.getName().starts_with("_R"))
    return {};
  auto Attr = F.getAttributes().getRetAttrs().getAttribute(Attribute::Range);
  if (!Attr.isValid())
    return {};
  const ConstantRange &R = Attr.getValueAsConstantRange();
  return isLargeTag(R.getLower(), R.getUpper()) ? Attr : Attribute();
}

struct Leaf {
  ReturnInst *Ret;
  ConstantInt *Value;
};

// The literal cannot flow through a call or address in the IR. Reject
// special entry points and metadata (e.g. type IDs for devirtualization).
std::optional<Leaf> isolatedLeaf(Function &F, const ConstantRange &Range) {
  if (F.getName().contains(char(0)) || !F.hasExternalLinkage() ||
      !F.hasDefaultVisibility() ||
      F.getDLLStorageClass() != GlobalValue::DefaultStorageClass ||
      F.getCallingConv() != CallingConv::C || !F.arg_empty() || F.isVarArg() ||
      !F.use_empty() || F.isUsedByMetadata() || F.hasComdat() ||
      F.hasPersonalityFn() || F.hasPrefixData() || F.hasPrologueData() ||
      F.hasSection() || F.hasGC() || F.hasMetadataOtherThanDebugLocAndGuid() ||
      !hasSingleElement(F))
    return std::nullopt;

  auto &Entry = F.getEntryBlock();
  if (Entry.hasAddressTaken() || !hasSingleElement(Entry))
    return std::nullopt;
  auto *Ret = dyn_cast<ReturnInst>(&Entry.front());
  if (!Ret || Ret->hasMetadataOtherThanDebugLoc())
    return std::nullopt;
  auto *V = dyn_cast_or_null<ConstantInt>(Ret->getReturnValue());
  if (!V || !Range.contains(V->getValue()))
    return std::nullopt;
  return Leaf{Ret, V};
}

struct Change {
  uint16_t Before;
  uint16_t After;
  bool Found = false;
};

using Elf = object::ELF64LE;
using namespace ELF;

bool matchesInstruction(ArrayRef<uint8_t> Code, uint16_t Expected) {
  if (Code.size() == 5 && Code[0] == 0x66 && Code[1] == 0xb8 && Code[4] == 0xc3)
    return support::endian::read16le(Code.data() + 2) == Expected;
  if (Code.size() == 6 && Code[0] == 0xb8 && Code[5] == 0xc3)
    return support::endian::read32le(Code.data() + 1) == Expected;
  return false;
}

bool ordinarySection(uint32_t Type) {
  switch (Type) {
  case SHT_NULL:
  case SHT_PROGBITS:
  case SHT_SYMTAB:
  case SHT_STRTAB:
  case SHT_RELA:
  case SHT_NOBITS:
  case SHT_NOTE:
  case SHT_GROUP:
  case SHT_INIT_ARRAY:
  case SHT_FINI_ARRAY:
  case SHT_PREINIT_ARRAY:
  case SHT_X86_64_UNWIND:
  case SHT_LLVM_ADDRSIG:
    return true;
  default:
    return false;
  }
}

// Cross-section calls and data references require relocations. In
// particular an equivalent function can have become a jump to this leaf.
// Updating it would silently change that other function's return value.
bool isolatedSections(const object::ELF64LEFile &Obj, Elf::ShdrRange Sections,
                      unsigned Symtab, Elf::SymRange Symbols,
                      ArrayRef<Change *> Changes) {
  auto Names = expectedToOptional(Obj.getSectionStringTable(Sections));
  if (!Names)
    return false;
  for (auto &Rel : Sections) {
    if (Rel.sh_type != SHT_RELA)
      continue;
    if (Rel.sh_link != Symtab || !Rel.sh_info ||
        Rel.sh_info >= Sections.size() || Changes[Rel.sh_info])
      return false;
    auto Entries = expectedToOptional(Obj.relas(Rel));
    auto &From = Sections[Rel.sh_info];
    auto Name = expectedToOptional(Obj.getSectionName(From, *Names));
    if (!Entries || !Name)
      return false;
    for (const auto &R : *Entries) {
      uint32_t SI = R.getSymbol(false);
      if (SI >= Symbols.size() || R.r_offset >= From.sh_size)
        return false;
      // Limit allocated relocations to forms with explicit symbol indices.
      // A different symbol can then be ruled out by its section index.
      if ((From.sh_flags & SHF_ALLOC) &&
          !is_contained({R_X86_64_NONE, R_X86_64_64, R_X86_64_PC32,
                         R_X86_64_PLT32, R_X86_64_GOTPCREL, R_X86_64_32,
                         R_X86_64_32S, R_X86_64_TLSGD, R_X86_64_GOTPCRELX,
                         R_X86_64_REX_GOTPCRELX},
                        R.getType(false)))
        return false;
      const auto &S = Symbols[SI];
      if (S.st_shndx >= Sections.size() || !Changes[S.st_shndx])
        continue;
      if (!(From.sh_flags & SHF_ALLOC) && Name->starts_with(".debug_"))
        continue;
      // The length, address and stack effects of the leaf stay fixed.
      if (*Name != ".eh_frame" || From.sh_flags != SHF_ALLOC ||
          (From.sh_type != SHT_PROGBITS && From.sh_type != SHT_X86_64_UNWIND) ||
          R.getType(false) != R_X86_64_PC32 || R.r_addend || S.st_value ||
          From.sh_size - R.r_offset < 4)
        return false;
    }
  }
  return true;
}

bool patchLiterals(std::string &Bytes, StringMap<Change> &Delta) {
  auto Obj = expectedToOptional(object::ELF64LEFile::create(Bytes));
  if (!Obj)
    return false;
  const auto &E = Obj->getHeader();
  if (!E.checkMagic() || E.getFileClass() != ELFCLASS64 ||
      E.getDataEncoding() != ELFDATA2LSB || E.e_version != EV_CURRENT ||
      E.e_machine != EM_X86_64 || E.e_type != ET_REL ||
      E.e_ehsize != sizeof(E) || E.e_phnum || !E.e_shnum ||
      E.e_shnum >= SHN_LORESERVE || !E.e_shstrndx || E.e_shstrndx >= E.e_shnum)
    return false;
  auto Sections = expectedToOptional(Obj->sections());
  if (!Sections || Sections->size() != E.e_shnum ||
      (*Sections)[E.e_shstrndx].sh_type != SHT_STRTAB)
    return false;
  unsigned Symtab = 0;
  for (auto [Index, S] : enumerate(*Sections)) {
    if (!ordinarySection(S.sh_type) || (S.sh_flags & SHF_COMPRESSED) ||
        (S.sh_type != SHT_NOBITS &&
         !expectedToOptional(Obj->getSectionContents(S))))
      return false;
    if (S.sh_type == SHT_SYMTAB) {
      if (Symtab || !Index)
        return false;
      Symtab = Index;
    }
  }
  if (!Symtab)
    return false;
  auto &Table = (*Sections)[Symtab];
  auto Names = expectedToOptional(Obj->getStringTableForSymtab(Table));
  auto Symbols = expectedToOptional(Obj->symbols(&Table));
  if (!Names || !Symbols)
    return false;

  SmallVector<Change *> Changes(Sections->size(), nullptr);
  SmallVector<unsigned> Members(Sections->size(), 0);
  size_t Found = 0;
  for (const auto &S : *Symbols) {
    auto Name = expectedToOptional(S.getName(*Names));
    if (!Name)
      return false;
    auto It = Delta.find(*Name);
    if (S.st_shndx == SHN_UNDEF || S.st_shndx == SHN_ABS ||
        S.st_shndx == SHN_COMMON) {
      if (It != Delta.end())
        return false;
      continue;
    }
    if (S.st_shndx >= Sections->size())
      return false;
    if (S.getType() != STT_SECTION || S.getBinding() != STB_LOCAL ||
        !Name->empty() || S.st_value || S.st_size)
      ++Members[S.st_shndx];
    if (It == Delta.end())
      continue;
    auto &Text = (*Sections)[S.st_shndx];
    if (It->second.Found || S.getType() != STT_FUNC ||
        S.getBinding() != STB_GLOBAL || S.st_other || S.st_value ||
        S.st_size != Text.sh_size || Text.sh_type != SHT_PROGBITS ||
        Text.sh_flags != (SHF_ALLOC | SHF_EXECINSTR) || Text.sh_addr)
      return false;
    ++Found;
    Changes[S.st_shndx] = &It->second;
    It->second.Found = true;
  }
  if (Found != Delta.size() ||
      !isolatedSections(*Obj, *Sections, Symtab, *Symbols, Changes))
    return false;
  for (auto [Index, S] : enumerate(*Sections)) {
    const auto *C = Changes[Index];
    if (!C)
      continue;
    auto Code = expectedToOptional(Obj->getSectionContents(S));
    if (!C->Before || !C->After || Members[Index] != 1 || !Code ||
        !matchesInstruction(*Code, C->Before))
      return false;
    uint64_t Start = S.sh_offset, End = Start + S.sh_size;
    if (Start < sizeof(E) ||
        (Start < E.e_shoff + Sections->size() * sizeof(S) && E.e_shoff < End))
      return false;
    for (const auto &Other : *Sections)
      if (&Other != &S && Other.sh_size && Other.sh_type != SHT_NOBITS &&
          Other.sh_offset < End && Start < Other.sh_offset + Other.sh_size)
        return false;
    size_t Offset = Start + (Code->size() == 5 ? 2 : 1);
    support::endian::write16le(Bytes.data() + Offset, C->After);
  }
  return true;
}
} // namespace

bool ohm_leaf::candidate(const Module &M) {
  const Triple &T = M.getTargetTriple();
  return T.getArch() == Triple::x86_64 && T.isOSBinFormatELF() &&
         M.isMaterialized() && M.getModuleInlineAsm().empty() &&
         M.ifunc_empty() &&
         any_of(M, [](const Function &F) { return tagRange(F).isValid(); });
}

std::string ohm_leaf::prepare(Module &M, StringRef Configuration,
                              StringRef Previous) {
  if (Configuration.empty() || !candidate(M))
    return {};
  SmallVector<Leaf> Leaves;
  std::string Literals;
  for (auto &F : M) {
    auto Range = tagRange(F);
    if (!Range.isValid())
      continue;
    auto Slot = isolatedLeaf(F, Range.getValueAsConstantRange());
    if (!Slot)
      continue;
    Leaves.push_back(*Slot);
    Literals += F.getName();
    Literals.push_back(0);
    char Value[2];
    support::endian::write16le(Value, Slot->Value->getZExtValue());
    Literals.append(Value, 2);
  }
  // The previous witness starts with the object binding and input digest.
  // This is a performance hint only. Reuse checks the complete witness.
  if (Leaves.empty() ||
      (Previous.size() >= 64 && Previous.drop_front(64) == Literals))
    return {};

  // Integer constants have no use list. Restoring a literal cannot change
  // the use order of another value in the module.
  for (auto &L : Leaves)
    L.Ret->setOperand(0, ConstantInt::get(L.Value->getType(), 0));
  auto Restore = scope_exit([&] {
    for (auto &L : Leaves)
      L.Ret->setOperand(0, L.Value);
  });

  Digest Input;
  if (!getClosedModuleHashV1(M, Input))
    return {};
  BLAKE3 Hash;
  Hash.update("ohm-literal-input-v1");
  Hash.update(digest(Configuration));
  Hash.update(Input);
  return bytes(Hash.final()).str() + Literals;
}

std::string ohm_leaf::enroll(StringRef Proof, StringRef Object) {
  if (Proof.size() < 32 || Object.empty())
    return {};
  return (bytes(binding(Proof, Object)) + Proof).str();
}

std::string ohm_leaf::tryReuse(StringRef Proof, StringRef Witness,
                               StringRef Prior) {
  if (Proof.size() < 32 || Witness.size() < 64 || Prior.empty())
    return {};
  StringRef Prev = Witness.drop_front(32);
  if (Prev.take_front(32) != Proof.take_front(32) ||
      bytes(binding(Prev, Prior)) != Witness.take_front(32))
    return {};

  Prev = Prev.drop_front(32);
  Proof = Proof.drop_front(32);
  StringMap<Change> Delta;
  while (!Proof.empty()) {
    auto [Name, Value] = Proof.split(char(0));
    if (Name.empty() || Value.size() < 2 ||
        !Prev.consume_front(Proof.take_front(Name.size() + 1)) ||
        Prev.size() < 2)
      return {};
    uint16_t Before = support::endian::read16le(Prev.data());
    uint16_t After = support::endian::read16le(Value.data());
    if (Before != After)
      Delta.try_emplace(Name, Change{Before, After});
    Prev = Prev.drop_front(2);
    Proof = Value.drop_front(2);
  }
  if (!Prev.empty())
    return {};

  std::string Bytes = Prior.str();
  if (!Delta.empty() && !patchLiterals(Bytes, Delta))
    return {};
  return Bytes;
}
