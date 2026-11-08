//===- OhmRuntimeLocations.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/DWARF/OhmRuntimeLocations.h"
#include "OhmDebugObjectJSON.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include <map>
#include <set>
#include <tuple>

using namespace llvm;
using namespace llvm::object;

namespace {
using Shdr = ELF64LE::Shdr;
using Sym = ELF64LE::Sym;
using Tuple = std::tuple<StringRef, uint32_t, uint32_t>;
using Address = std::pair<unsigned, uint64_t>;
Error reject(const Twine &Message) {
  return createStringError(inconvertibleErrorCode(), Message);
}
struct Update {
  std::string Global, FileGlobal, File;
  bool Terminated;
  uint32_t Before, After;
  std::optional<uint32_t> Column;
};
bool privateName(StringRef S) {
  if (!S.consume_front("anon."))
    return false;
  const auto [Hash, Index] = S.split('.');
  return Hash.size() == 32 &&
         all_of(Hash,
                [](char C) { return isDigit(C) || (C >= 'a' && C <= 'f'); }) &&
         !Index.empty() && (Index == "0" || !Index.starts_with("0")) &&
         all_of(Index, [](char C) { return isDigit(C); });
}
Expected<std::vector<Update>> parse(StringRef Witness,
                                    const ohm::SourceMap &Map) {
  auto Json = json::parse(Witness);
  if (!Json)
    return Json.takeError();
  const auto *Rows = Json->getAsArray();
  if (!Rows || !ohm::uniqueObjectKeys(Witness))
    return reject("invalid runtime location array");
  std::set<std::string> Seen;
  std::vector<Update> Result;
  for (const auto &Element : *Rows) {
    const auto *O = Element.getAsObject();
    if (!O || O->size() != 8)
      return reject("invalid runtime location record");
    auto G = O->getString("global"), FG = O->getString("file_global"),
         F = O->getString("file"), D = O->getString("directory");
    auto Nul = O->getBoolean("file_nul");
    auto B = O->getInteger("old_line"), A = O->getInteger("new_line");
    const auto *C = O->get("column");
    if (!G || !FG || !privateName(*G) || !privateName(*FG) || !F ||
        F->empty() || F->contains('\0') || !D || !D->starts_with("/") || !Nul ||
        !B || !A || !C || *A <= 0 || *B <= 0 || *A > UINT32_MAX ||
        *B > UINT32_MAX || *A == *B || !Seen.insert(G->str()).second)
      return reject("invalid or duplicate runtime location identity");
    std::optional<uint32_t> Column;
    if (C->kind() != json::Value::Null) {
      auto V = C->getAsInteger();
      if (!V || *V < 0 || *V > UINT32_MAX)
        return reject("invalid runtime column");
      Column = *V;
    }
    std::string Path =
        F->starts_with("/")
            ? F->str()
            : (*D == "/" ? Twine("/") + *F : Twine(*D) + "/" + *F).str();
    auto File = Map.find(Path);
    if (File == Map.end())
      return reject("runtime filename not in the checked source map");
    auto Line = File->second.find(*B);
    if (Line == File->second.end() || Line->second != *A)
      return reject("runtime line change differs from the checked source map");
    Result.push_back({G->str(), FG->str(), F->str(), *Nul, uint32_t(*B),
                      uint32_t(*A), Column});
  }
  return Result;
}

struct Relocation {
  unsigned Owner, Type, Symbol;
  uint64_t Offset;
  int64_t Addend;
};
bool plainSectionSymbol(const Sym &S, size_t I) {
  return S.st_name == 0 && S.st_info == ELF::STT_SECTION && S.st_other == 0 &&
         S.st_shndx == I && S.st_value == 0 && S.st_size == 0;
}
std::optional<uint64_t> addOffset(uint64_t Base, int64_t Addend) {
  if (Addend < 0) {
    uint64_t Amount = -uint64_t(Addend);
    if (Amount > Base)
      return {};
    return Base - Amount;
  }
  if (uint64_t(Addend) > UINT64_MAX - Base)
    return {};
  return Base + Addend;
}

class Binding {
  const ELFFile<ELF64LE> &EF;
  StringRef Raw;
  ArrayRef<Shdr> Sections;
  ArrayRef<Sym> Symbols;
  std::vector<StringRef> Names;
  StringMap<int> ByName;
  std::vector<unsigned> Defined;
  std::vector<Relocation> Relocs;
  std::vector<SmallVector<unsigned, 1>> Owned, Incoming;
  std::map<Tuple, std::vector<Address>> Occupied;
  std::set<Tuple> Proposed;
  std::map<std::pair<unsigned, StringRef>, uint64_t> Pooled;
  StringRef bytes(size_t I) const {
    return Raw.substr(Sections[I].sh_offset, Sections[I].sh_size);
  }
  int unique(StringRef Name) const {
    auto It = ByName.find(Name);
    return It == ByName.end() ? -1 : It->second;
  }
  Error readSections() {
    auto All = EF.sections();
    if (!All)
      return All.takeError();
    Sections = *All;
    Defined.resize(Sections.size());
    Owned.resize(Sections.size());
    Incoming.resize(Sections.size());
    const auto &H = EF.getHeader();
    if (H.e_type != ELF::ET_REL || H.e_machine != ELF::EM_X86_64 || H.e_phnum ||
        H.e_shentsize != sizeof(Shdr))
      return reject(
          "runtime locations require ordinary x86-64 ELF64 RELA objects");
    using Span = std::pair<uint64_t, uint64_t>;
    std::vector<Span> Spans{
        {0, sizeof(ELF64LE::Ehdr)},
        {H.e_shoff, H.e_shoff + uint64_t(Sections.size()) * sizeof(Shdr)}};
    for (const auto &[I, S] : enumerate(Sections)) {
      auto N = EF.getSectionName(S);
      if (!N)
        return N.takeError();
      if (S.sh_addr)
        return reject("unexpected section load address");
      Names.push_back(*N);
      auto It = ByName.try_emplace(*N, int(I));
      if (!It.second)
        It.first->second = -1;
      if (S.sh_type == ELF::SHT_REL || S.sh_type == ELF::SHT_RELR ||
          S.sh_type == ELF::SHT_SYMTAB_SHNDX || S.sh_type == ELF::SHT_DYNSYM)
        return reject("unsupported runtime relocation or symbol-table format");
      if (S.sh_type != ELF::SHT_NOBITS && S.sh_size) {
        if (S.sh_offset > Raw.size() || S.sh_size > Raw.size() - S.sh_offset)
          return reject("invalid ELF section bounds");
        Spans.emplace_back(S.sh_offset, S.sh_offset + S.sh_size);
      }
      if (S.sh_type != ELF::SHT_SYMTAB)
        continue;
      if (!Symbols.empty() || S.sh_link >= Sections.size() ||
          Sections[S.sh_link].sh_type != ELF::SHT_STRTAB)
        return reject(
            "runtime locations require one conventional symbol table");
      auto Syms = EF.symbols(&S);
      if (!Syms)
        return Syms.takeError();
      Symbols = *Syms;
    }
    llvm::sort(Spans);
    for (size_t I = 1; I < Spans.size(); ++I)
      if (Spans[I].first < Spans[I - 1].second)
        return reject("overlapping ELF regions");
    if (Symbols.empty())
      return reject("missing ELF symbols");
    for (const auto &S : Symbols)
      if (S.st_shndx < Sections.size())
        ++Defined[S.st_shndx];
    for (const auto &S : Sections) {
      if (S.sh_type != ELF::SHT_RELA)
        continue;
      if (S.sh_info >= Sections.size() || S.sh_link >= Sections.size() ||
          Sections[S.sh_link].sh_type != ELF::SHT_SYMTAB)
        return reject("invalid runtime relocation-table links");
      auto RelName = EF.getSectionName(S);
      if (!RelName)
        return RelName.takeError();
      if (*RelName != (Twine(".rela") + Names[S.sh_info]).str())
        return reject("relocation section names a different target");
      auto Rs = EF.relas(S);
      if (!Rs)
        return Rs.takeError();
      for (const auto &R : *Rs) {
        unsigned K = R.getSymbol(false);
        if (K >= Symbols.size() || R.r_offset >= Sections[S.sh_info].sh_size)
          return reject("invalid runtime relocation index or offset");
        unsigned Index = Relocs.size();
        Relocs.push_back(
            {S.sh_info, R.getType(false), K, R.r_offset, R.r_addend});
        Owned[S.sh_info].push_back(Index);
        if (Symbols[K].st_shndx < Sections.size())
          Incoming[Symbols[K].st_shndx].push_back(Index);
      }
    }
    return Error::success();
  }
  void indexTuples(ArrayRef<Update> Changes) {
    DenseSet<uint64_t> Lengths;
    std::set<StringRef> Files;
    for (const auto &C : Changes) {
      Files.insert(C.File);
      Lengths.insert(C.File.size());
    }
    for (const auto &R : Relocs) {
      const auto &Owner = Sections[R.Owner];
      const auto &S = Symbols[R.Symbol];
      if (R.Type != ELF::R_X86_64_64 || !(Owner.sh_flags & ELF::SHF_ALLOC) ||
          Owner.sh_type != ELF::SHT_PROGBITS || Owner.sh_size - R.Offset < 24 ||
          S.st_shndx == 0 || S.st_shndx >= Sections.size())
        continue;
      const char *P = bytes(R.Owner).data() + R.Offset;
      uint64_t N = support::endian::read64le(P + 8);
      if (support::endian::read64le(P) || !Lengths.contains(N))
        continue;
      const auto &T = Sections[S.st_shndx];
      auto At = addOffset(S.st_value, R.Addend);
      if (T.sh_type != ELF::SHT_PROGBITS || !At || *At > T.sh_size ||
          N > T.sh_size - *At)
        continue;
      StringRef File = bytes(S.st_shndx).substr(*At, N);
      if (Files.count(File))
        Occupied[{File, support::endian::read32le(P + 16),
                  support::endian::read32le(P + 20)}]
            .emplace_back(R.Owner, R.Offset);
    }
  }
  Error filename(const Update &C, const Relocation &R) {
    const auto &Sym = Symbols[R.Symbol];
    if (R.Offset || R.Type != ELF::R_X86_64_64 || !Sym.st_shndx ||
        Sym.st_shndx >= Sections.size() ||
        !plainSectionSymbol(Sym, Sym.st_shndx) || R.Addend < 0)
      return reject(
          "runtime filename is not a direct 64-bit local section relocation");
    unsigned F = Sym.st_shndx;
    const auto &S = Sections[F];
    if (!Owned[F].empty() || S.sh_type != ELF::SHT_PROGBITS ||
        S.sh_addralign != 1 || unique(Names[F]) != int(F))
      return reject("ambiguous or relocated runtime filename storage");
    auto Bytes = bytes(F);
    if (!C.Terminated) {
      if (Names[F] != (Twine(".rodata..L") + C.FileGlobal).str() ||
          S.sh_flags != ELF::SHF_ALLOC || S.sh_entsize || R.Addend ||
          Bytes != C.File)
        return reject("dedicated filename global does not match its bytes");
      return Error::success();
    }
    if (Names[F] != ".rodata.str1.1" ||
        S.sh_flags != (ELF::SHF_ALLOC | ELF::SHF_STRINGS | ELF::SHF_MERGE) ||
        S.sh_entsize != 1)
      return reject("unsupported pooled runtime filename");
    auto [It, Inserted] = Pooled.emplace(std::make_pair(F, StringRef(C.File)),
                                         uint64_t(R.Addend));
    if (!Inserted) {
      if (It->second != uint64_t(R.Addend))
        return reject("ambiguous filename pool offset");
      return Error::success();
    }
    size_t Count = 0;
    size_t At = 0;
    while (!Bytes.empty()) {
      size_t End = Bytes.find('\0');
      if (End == StringRef::npos)
        return reject("unterminated ELF string pool");
      if (Bytes.take_front(End) == C.File) {
        if (At != uint64_t(R.Addend))
          return reject("ambiguous or interior filename relocation");
        ++Count;
      }
      At += End + 1;
      Bytes = Bytes.drop_front(End + 1);
    }
    if (Count != 1)
      return reject("no unique terminated runtime filename");
    return Error::success();
  }
  Expected<size_t> bind(const Update &C) {
    int Index = unique((Twine(".data.rel.ro..L") + C.Global).str());
    if (Index < 0)
      return reject("runtime global has no unique dedicated ELF section");
    unsigned I = Index;
    const auto &S = Sections[I];
    if (S.sh_type != ELF::SHT_PROGBITS ||
        S.sh_flags != (ELF::SHF_WRITE | ELF::SHF_ALLOC) || S.sh_size != 24 ||
        S.sh_link || S.sh_info || S.sh_addralign != 8 || S.sh_entsize ||
        Defined[I] != 1 || Owned[I].size() != 1 || Incoming[I].empty())
      return reject("unbound, aliased or unexpected runtime global layout");
    const auto &R = Relocs[Owned[I][0]];
    if (Error E = filename(C, R))
      return std::move(E);
    const char *P = bytes(I).data();
    uint32_t Col = C.Column.value_or(0);
    if (support::endian::read64le(P) ||
        support::endian::read64le(P + 8) != C.File.size() ||
        support::endian::read32le(P + 16) != C.Before ||
        support::endian::read32le(P + 20) != Col)
      return reject(
          "old filename length, runtime coordinate or undef padding differs");
    for (unsigned N : Incoming[I]) {
      const auto &In = Relocs[N];
      const auto &Source = Sections[In.Owner];
      if (!plainSectionSymbol(Symbols[In.Symbol], I) ||
          In.Type != ELF::R_X86_64_PC32 || In.Addend != -4 ||
          Source.sh_type != ELF::SHT_PROGBITS ||
          !(Source.sh_flags & ELF::SHF_EXECINSTR) || In.Offset < 3 ||
          Source.sh_size - In.Offset < 4)
        return reject("runtime global has a non-address or interior reference");
      const auto *Op =
          reinterpret_cast<const uint8_t *>(bytes(In.Owner).data() + In.Offset);
      if ((Op[-3] & 0xf8) != 0x48 || Op[-2] != 0x8d ||
          (Op[-1] & 0xc7) != 0x05 || support::endian::read32le(Op))
        return reject(
            "runtime address is not a complete RIP-relative 64-bit LEA");
    }
    Tuple Old{C.File, C.Before, Col}, New{C.File, C.After, Col};
    auto Found = Occupied.find(Old);
    if (Found == Occupied.end() || Found->second.size() != 1 ||
        Found->second.front() != Address(I, 0) || Occupied.count(New) ||
        !Proposed.insert(New).second)
      return reject("runtime global could merge with another emitted record");
    return S.sh_offset + 16;
  }

public:
  Binding(const ELF64LEObjectFile &Obj, StringRef B)
      : EF(Obj.getELFFile()), Raw(B) {}
  Error apply(ArrayRef<Update> Changes, MutableArrayRef<char> Out) {
    if (Out.size() != Raw.size())
      return reject("runtime patch requires the same ELF layout");
    if (Error E = readSections())
      return E;
    indexTuples(Changes);
    std::vector<size_t> Offsets;
    for (const auto &C : Changes) {
      auto At = bind(C);
      if (!At)
        return At.takeError();
      if (StringRef(Out.data() + *At - 16, 24) != Raw.substr(*At - 16, 24))
        return reject("runtime record intersects another rewrite");
      Offsets.push_back(*At);
    }
    for (const auto &[I, C] : enumerate(Changes))
      support::endian::write32le(Out.data() + Offsets[I], C.After);
    return Error::success();
  }
};
} // namespace
Error llvm::ohm::rebaseRuntimeLocations(MemoryBufferRef Old,
                                        const SourceMap &Map, StringRef Witness,
                                        MutableArrayRef<char> Result) {
  auto Updates = parse(Witness, Map);
  if (!Updates)
    return Updates.takeError();
  if (Updates->empty())
    return Error::success();
  auto Obj = ObjectFile::createObjectFile(Old);
  if (!Obj)
    return Obj.takeError();
  auto *ELF = dyn_cast<ELF64LEObjectFile>(Obj->get());
  if (!ELF)
    return reject("requires ELF64 little endian");
  return Binding(*ELF, Old.getBuffer()).apply(*Updates, Result);
}
extern "C" int ohm_debug_rebase_locations_v1(const void *M, size_t MS,
                                             const void *L, size_t LS,
                                             const void *O, size_t OS,
                                             LLVMMemoryBufferRef *Output,
                                             const char **Reason) {
  if (Output)
    *Output = nullptr;
  if (Reason)
    *Reason = nullptr;
  auto Miss = [&](Error E) {
    static thread_local std::string Last;
    Last = toString(std::move(E));
    if (Reason)
      *Reason = Last.c_str();
    return 2;
  };
  if (!M || !L || !O || !Output)
    return Miss(
        reject("missing source map, location witness or cached object"));
  auto Map = ohm::parseSourceMap(StringRef(static_cast<const char *>(M), MS));
  if (!Map)
    return Miss(Map.takeError());
  MemoryBufferRef Old(StringRef(static_cast<const char *>(O), OS),
                      "cached runtime locations");
  auto Result = ohm::rebaseDwarf4(Old, *Map);
  if (!Result)
    return Miss(Result.takeError());
  if (Error E = ohm::rebaseRuntimeLocations(
          Old, *Map, StringRef(static_cast<const char *>(L), LS),
          Result->Bytes))
    return Miss(std::move(E));
  auto Bytes = StringRef(Result->Bytes.data(), Result->Bytes.size());
  *Output = reinterpret_cast<LLVMMemoryBufferRef>(
      MemoryBuffer::getMemBufferCopy(Bytes).release());
  return 0;
}
