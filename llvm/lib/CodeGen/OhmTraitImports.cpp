//===- OhmTraitImports.cpp - Relocatable Rust trait imports --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OhmTraitImports.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/StringTableBuilder.h"
#include "llvm/Object/ELFTypes.h"

#include "llvm/DebugInfo/DWARF/OhmDwarfRebase.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;
namespace {
// The closed DWARF line certificate starts with the compilation directory
// and a digest of the ordered IR, optimizer context and relative positions.
// The remaining fields record the exact checksums and source coordinates.
struct CertificateReader {
  StringRef Bytes;

  bool word(uint32_t &Value) {
    if (Bytes.size() < 4)
      return false;
    Value = support::endian::read32le(Bytes.data());
    Bytes = Bytes.drop_front(4);
    return true;
  }

  bool field(StringRef &Value) {
    uint32_t Size;
    if (!word(Size) || Size > Bytes.size())
      return false;
    Value = Bytes.take_front(Size);
    Bytes = Bytes.drop_front(Size);
    return true;
  }

  bool header(StringRef &Directory, StringRef &Digest) {
    if (!field(Directory) || Bytes.size() < 32)
      return false;
    Digest = Bytes.take_front(32);
    Bytes = Bytes.drop_front(32);
    return true;
  }
};

bool canonicalPath(StringRef Path) {
  if (!Path.starts_with("/") || Path.ends_with("/") || Path.contains("//"))
    return false;
  for (StringRef Part : split(Path, '/'))
    if (Part == "." || Part == "..")
      return false;
  return true;
}

// Mirror rustc's dwarf_certificate::compare. Require an unambiguous,
// monotone line mapping for each changed file. Unchanged files must preserve
// every coordinate. Rebase only after validating the complete certificate.
bool compareLines(StringRef Previous, StringRef Current, ohm::SourceMap &Map) {
  CertificateReader Old{Previous}, New{Current};
  StringRef OldDir, NewDir, OldDigest, NewDigest;
  if (!Old.header(OldDir, OldDigest) || !New.header(NewDir, NewDigest) ||
      OldDir != NewDir || OldDigest != NewDigest || NewDir.contains('\0'))
    return false;
  uint32_t Count, NewCount;
  if (!Old.word(Count) || !New.word(NewCount) || Count != NewCount ||
      Count > Old.Bytes.size() / 8 || Count > New.Bytes.size() / 8)
    return false;
  SmallVector<std::string> Paths;
  StringMap<std::pair<StringRef, StringRef>> Checksums;
  for (size_t I = 0; I < Count; ++I) {
    StringRef Path, NewPath, Before, After;
    if (!Old.field(Path) || !New.field(NewPath) || Path != NewPath ||
        Path.contains('\0') || !Old.field(Before) || !New.field(After))
      return false;
    Paths.push_back(Path.starts_with("/") ? Path.str()
                                          : (NewDir + "/" + Path).str());
    auto Pair = std::make_pair(Before, After);
    if (Checksums.try_emplace(Paths.back(), Pair).first->second != Pair)
      return false;
    if (Before != After) {
      if (Before.empty() || After.empty() || !canonicalPath(Paths.back()))
        return false;
      Map.try_emplace(Paths.back());
    }
  }
  if (!Old.word(Count) || !New.word(NewCount) || Count != NewCount ||
      Old.Bytes.size() != uint64_t(Count) * 8 ||
      Old.Bytes.size() != New.Bytes.size())
    return false;
  for (size_t I = 0; I < Count; ++I) {
    uint32_t File, NewFile, From, To;
    if (!Old.word(File) || !New.word(NewFile) || File != NewFile ||
        File >= Paths.size() || !Old.word(From) || !New.word(To) || !From ||
        !To)
      return false;
    auto It = Map.find(Paths[File]);
    if (It == Map.end()) {
      if (From != To)
        return false;
    } else if (It->second.try_emplace(From, To).first->second != To)
      return false;
  }
  for (const auto &File : Map) {
    uint32_t Last = 0;
    for (auto Line : File.second) {
      if (Line.second <= Last)
        return false;
      Last = Line.second;
    }
  }
  return true;
}
} // namespace

static bool consumeIdentifier(StringRef &S) {
  uint64_t Size;
  if (S.starts_with("0") || S.consumeInteger(10, Size) || !Size ||
      Size > S.size() || !isAlpha(S.front()))
    return false;
  if (!all_of(S.take_front(Size),
              [](char C) { return isAlnum(C) || C == '_'; }))
    return false;
  S = S.drop_front(Size);
  return true;
}

static bool consumeCrate(StringRef &S) {
  if (!S.consume_front("Cs"))
    return false;
  S = S.drop_while(isAlnum);
  return S.consume_front("_") && consumeIdentifier(S);
}

static bool consumeTypePath(StringRef &S) {
  size_t Segments = 0;
  while (S.consume_front("Nt"))
    ++Segments;
  if (!Segments || !consumeCrate(S))
    return false;
  while (Segments--)
    if (!consumeIdentifier(S))
      return false;
  return true;
}

// Restrict to the v0 path of a provided trait method with a nominal receiver
// and a nominal trait: Nv Y <receiver> <trait> <method>. Accept a trailing
// instantiating crate only after parsing both complete type paths and the
// method. Generics, backreferences and special identifiers are out of scope.
static StringRef traitImportMethodPath(StringRef Name) {
  StringRef Rest = Name;
  if (!Rest.consume_front("_RNvY") || !consumeTypePath(Rest) ||
      !consumeTypePath(Rest) || !consumeIdentifier(Rest))
    return {};
  StringRef Path = Name.drop_back(Rest.size());
  return consumeCrate(Rest) && Rest.empty() ? Path : StringRef();
}

// All uses of this arbitrary external function must be direct callee
// operands. Its linkage, ABI, arguments, attributes and call sites remain
// part of the closed signature. No function bodies or debug names are masked.
static bool isDirectImport(const Function &F) {
  if (!F.isDeclaration() || !F.hasExternalLinkage() || F.hasComdat() ||
      F.hasSection() || F.isVarArg() || F.isUsedByMetadata() ||
      F.hasMetadata() || F.use_empty())
    return false;
  return all_of(F.uses(), [](const Use &U) {
    auto *CB = dyn_cast<CallBase>(U.getUser());
    return CB && CB->isCallee(&U);
  });
}

bool ohm::writeTraitImportSignature(const Module &M, SmallVectorImpl<char> &Key,
                                    SmallVectorImpl<char> &Manifest,
                                    function_ref<bool(StringRef)> ShouldProbe) {
  SmallVector<char, 0> Imports;
  if (M.getDwarfVersion() != 4 || !M.getModuleInlineAsm().empty() ||
      !M.alias_empty() || !M.ifunc_empty())
    return false;

  DenseMap<const Function *, StringRef> Paths;
  StringSet<> Seen;
  for (const Function &F : M) {
    StringRef Path = traitImportMethodPath(F.getName());
    if (Path.empty() || !isDirectImport(F))
      continue;
    // The LLVM assembler-name escape is stripped before ELF emission. Its
    // references would share the import's symbol index and could not be
    // distinguished from the calls we intend to rebind.
    if (M.getNamedValue(("\1" + F.getName()).str()) || M.getNamedValue(Path) ||
        !Seen.insert(Path).second)
      return false;
    Paths[&F] = Path;
    Imports.append(F.getName().begin(), F.getName().end());
    Imports.push_back('\0');
  }
  if (Paths.empty() ||
      (ShouldProbe && !ShouldProbe(StringRef(Imports.data(), Imports.size()))))
    return false;

  // Assembly can reference spelling-dependent symbols outside IR use lists.
  for (const Function &F : M)
    for (const BasicBlock &BB : F)
      for (const Instruction &I : BB)
        if (auto *CB = dyn_cast<CallBase>(&I); CB && CB->isInlineAsm())
          return false;
  auto GetName = [&](const Function &F) {
    auto It = Paths.find(&F);
    return It != Paths.end() ? It->second : F.getName();
  };
  SmallVector<char, 0> Certificate;
  raw_svector_ostream Stream(Certificate);
  if (!writeProjectedDwarfLineCertificate(M, Stream, GetName))
    return false;
  CertificateReader Reader{StringRef(Certificate.data(), Certificate.size())};
  StringRef Directory, Digest;
  if (!Reader.header(Directory, Digest) || Certificate.size() >= (128u << 20))
    return false;
  Key.append(Certificate.begin(), Certificate.end() - Reader.Bytes.size());
  char Size[4];
  support::endian::write32le(Size, Certificate.size());
  Manifest.append(Size, Size + 4);
  Manifest.append(Certificate.begin(), Certificate.end());
  Manifest.append(Imports.begin(), Imports.end());
  return true;
}

// The integrated assembler emits the symbol and section names into a
// single suffix-compressed string table immediately before the section
// headers. Rebuild it with the same builder, preserving section and symbol
// indices: relocations and all index-based metadata keep their targets.
static bool renameObjectImports(StringRef Object,
                                const StringMap<StringRef> &Renames,
                                SmallVectorImpl<char> &Output) {
  using Header = object::ELF64LE::Ehdr;
  using Section = object::ELF64LE::Shdr;
  using Symbol = object::ELF64LE::Sym;
  using Relocation = object::ELF64LE::Rela;
  if (Object.size() < sizeof(Header) || Object.size() > (128u << 20))
    return false;
  Header H;
  memcpy(&H, Object.data(), sizeof(H));
  if (!H.checkMagic() || H.getFileClass() != ELF::ELFCLASS64 ||
      H.getDataEncoding() != ELF::ELFDATA2LSB || H.e_type != ELF::ET_REL ||
      H.e_machine != ELF::EM_X86_64 || H.e_ehsize != sizeof(Header) ||
      H.e_flags || H.e_version != ELF::EV_CURRENT || H.e_entry || H.e_phnum ||
      H.e_phoff || H.e_shentsize != sizeof(Section) ||
      H.e_shoff >= Object.size() ||
      Object.size() - H.e_shoff != size_t(H.e_shnum) * sizeof(Section) ||
      !H.e_shstrndx || H.e_shstrndx >= H.e_shnum)
    return false;
  SmallVector<Section> Sections(H.e_shnum);
  memcpy(Sections.data(), Object.data() + H.e_shoff,
         H.e_shnum * sizeof(Section));
  auto &StrSec = Sections[H.e_shstrndx];
  if (StrSec.sh_type != ELF::SHT_STRTAB || StrSec.sh_flags ||
      StrSec.sh_offset > H.e_shoff || StrSec.sh_offset < sizeof(Header) ||
      StrSec.sh_size > H.e_shoff - StrSec.sh_offset ||
      alignTo(StrSec.sh_offset + StrSec.sh_size, 8) != H.e_shoff)
    return false;
  StringRef Table = Object.substr(StrSec.sh_offset, StrSec.sh_size);
  if (Table.empty() || Table.front() || Table.back())
    return false;
  Section *SymSec = nullptr;
  for (auto &S : Sections) {
    if (S.sh_type == ELF::SHT_REL || S.sh_type == ELF::SHT_CREL ||
        S.sh_type == ELF::SHT_DYNAMIC || S.sh_type == ELF::SHT_DYNSYM)
      return false;
    if (&S != &StrSec && S.sh_type != ELF::SHT_NOBITS &&
        (S.sh_offset > StrSec.sh_offset ||
         S.sh_size > StrSec.sh_offset - S.sh_offset))
      return false;
    if (S.sh_type == ELF::SHT_SYMTAB) {
      if (SymSec || S.sh_link != H.e_shstrndx ||
          S.sh_entsize != sizeof(Symbol) || !S.sh_size ||
          S.sh_size % sizeof(Symbol) || S.sh_offset < sizeof(Header))
        return false;
      SymSec = &S;
    } else if (S.sh_link == H.e_shstrndx)
      return false;
    if (S.sh_name >= Table.size())
      return false;
  }
  if (!SymSec)
    return false;
  for (auto &S : Sections)
    if (&S != SymSec && S.sh_type != ELF::SHT_NOBITS && S.sh_size &&
        S.sh_offset < SymSec->sh_offset + SymSec->sh_size &&
        S.sh_offset + S.sh_size > SymSec->sh_offset)
      return false;

  // Refuse producer metadata that embeds a changed name, even when it
  // carries no relocation. Allocated literal strings are part of the exact
  // IR and keep their original bytes.
  for (const Section &S : Sections) {
    if (S.sh_flags & ELF::SHF_COMPRESSED)
      return false;
    if (S.sh_type != ELF::SHT_PROGBITS || (S.sh_flags & ELF::SHF_ALLOC))
      continue;
    StringRef Data = Object.substr(S.sh_offset, S.sh_size);
    for (const auto &Pair : Renames)
      if (Data.contains(Pair.first()))
        return false;
  }

  SmallVector<Symbol> Symbols(SymSec->sh_size / sizeof(Symbol));
  memcpy(Symbols.data(), Object.data() + SymSec->sh_offset, SymSec->sh_size);
  StringSet<> Destinations, Seen;
  for (auto &Pair : Renames)
    if (!Destinations.insert(Pair.second).second)
      return false;
  StringTableBuilder Builder(StringTableBuilder::ELF);
  for (const Section &S : Sections) {
    if (!S.sh_name)
      continue;
    // A named section can supply its own implicit symbol at assembly time.
    StringRef Name(Table.data() + S.sh_name);
    if (Renames.contains(Name) || Destinations.contains(Name))
      return false;
    Builder.add(Name);
  }
  SmallVector<StringRef> Names;
  SmallDenseSet<size_t> Changed;
  for (auto &S : Symbols) {
    if (S.st_name >= Table.size())
      return false;
    StringRef Name(Table.data() + S.st_name);
    // Keep any use of ELF version syntax for either name out of replay.
    StringRef Base = Name.split('@').first;
    if (Destinations.contains(Base) || (Base != Name && Renames.contains(Base)))
      return false;
    if (auto It = Renames.find(Name); It != Renames.end()) {
      if (S.st_shndx != ELF::SHN_UNDEF || S.st_value || S.st_size ||
          S.getBinding() != ELF::STB_GLOBAL ||
          (S.getType() != ELF::STT_FUNC && S.getType() != ELF::STT_NOTYPE) ||
          (S.st_other != ELF::STV_DEFAULT && S.st_other != ELF::STV_HIDDEN) ||
          !Seen.insert(Name).second)
        return false;
      Name = It->second;
      Changed.insert(Names.size());
    }
    Names.push_back(Name);
    if (!Name.empty())
      Builder.add(Name);
  }
  if (Seen.size() != Renames.size())
    return false;
  // Every surviving reference must be an ordinary code relocation. In
  // particular refuse imports that occur as addresses in data or DWARF.
  SmallDenseSet<size_t> Referenced;
  for (const Section &Rel : Sections) {
    // A COMDAT signature can share the imported function's symbol index.
    // Rebinding it would also change how the linker selects the group.
    if (Rel.sh_type == ELF::SHT_GROUP && Changed.contains(Rel.sh_info))
      return false;
    if (Rel.sh_type != ELF::SHT_RELA)
      continue;
    if (Rel.sh_link != size_t(SymSec - Sections.data()) || !Rel.sh_info ||
        Rel.sh_info >= Sections.size() ||
        Rel.sh_entsize != sizeof(Relocation) ||
        Rel.sh_size % sizeof(Relocation))
      return false;
    const auto &Target = Sections[Rel.sh_info];
    for (size_t Off = 0; Off < Rel.sh_size; Off += sizeof(Relocation)) {
      Relocation R;
      memcpy(&R, Object.data() + Rel.sh_offset + Off, sizeof(R));
      auto Id = R.getSymbol(false);
      if (Id >= Symbols.size())
        return false;
      if (!Changed.contains(Id))
        continue;
      if (Target.sh_type != ELF::SHT_PROGBITS ||
          !(Target.sh_flags & ELF::SHF_EXECINSTR) || Target.sh_size < 4 ||
          R.r_offset > Target.sh_size - 4 || R.r_addend != -4)
        return false;
      switch (R.getType(false)) {
      case ELF::R_X86_64_PLT32:
      case ELF::R_X86_64_PC32:
      case ELF::R_X86_64_GOTPCREL:
      case ELF::R_X86_64_GOTPCRELX:
      case ELF::R_X86_64_REX_GOTPCRELX:
        Referenced.insert(Id);
        break;
      default:
        return false;
      }
    }
  }
  if (Referenced.size() != Changed.size())
    return false;
  Builder.finalize();
  if (Builder.getSize() > (128u << 20))
    return false;
  for (auto &S : Sections)
    if (S.sh_name)
      S.sh_name = Builder.getOffset(Table.data() + S.sh_name);
  for (auto [S, Name] : zip_equal(Symbols, Names))
    if (S.st_name)
      S.st_name = Builder.getOffset(Name);
  StrSec.sh_size = Builder.getSize();
  H.e_shoff = alignTo(StrSec.sh_offset + StrSec.sh_size, 8);
  Output.assign(Object.begin(), Object.begin() + StrSec.sh_offset);
  Output.resize(H.e_shoff + H.e_shnum * sizeof(Section), 0);
  memcpy(Output.data(), &H, sizeof(Header));
  memcpy(Output.data() + SymSec->sh_offset, Symbols.data(), SymSec->sh_size);
  Builder.write(reinterpret_cast<uint8_t *>(Output.data() + StrSec.sh_offset));
  memcpy(Output.data() + H.e_shoff, Sections.data(),
         H.e_shnum * sizeof(Section));
  return true;
}

bool ohm::rebindTraitImports(StringRef Object, StringRef Old, StringRef Current,
                             SmallVectorImpl<char> &Output) {
  StringRef BeforeLines, AfterLines;
  CertificateReader Previous{Old}, Next{Current};
  if (!Previous.field(BeforeLines) || !Next.field(AfterLines) ||
      BeforeLines.empty() || AfterLines.empty())
    return false;
  Old = Previous.Bytes;
  Current = Next.Bytes;
  if (Old.empty() || Current.empty() || Old.back() || Current.back())
    return false;
  StringMap<StringRef> Renames;
  while (!Old.empty() && !Current.empty()) {
    auto [Before, OldTail] = Old.split('\0');
    auto [After, NewTail] = Current.split('\0');
    if (Before.empty() || After.empty())
      return false;
    if (Before != After &&
        (traitImportMethodPath(Before).empty() ||
         traitImportMethodPath(Before) != traitImportMethodPath(After) ||
         !Renames.try_emplace(Before, After).second))
      return false;
    Old = OldTail;
    Current = NewTail;
  }
  if (!Old.empty() || !Current.empty() || Renames.empty())
    return false;
  if (BeforeLines == AfterLines)
    return renameObjectImports(Object, Renames, Output);
  SourceMap Map;
  if (!compareLines(BeforeLines, AfterLines, Map))
    return false;
  auto Rebased =
      rebaseDwarf4(MemoryBufferRef(Object, "cached trait donor"), Map);
  if (!Rebased) {
    consumeError(Rebased.takeError());
    return false;
  }
  return renameObjectImports(
      StringRef(Rebased->Bytes.data(), Rebased->Bytes.size()), Renames, Output);
}
