//===- OhmDwarfRebase.cpp - Checked DWARF4 source coordinates -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/DWARF/OhmDwarfRebase.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDie.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <optional>
#include <set>
#include <utility>

using namespace llvm;
using namespace llvm::object;
using namespace llvm::ohm;

namespace {
Error unsupported(const Twine &Reason) {
  return createStringError(inconvertibleErrorCode(), Reason);
}

bool canonicalMapPath(StringRef Path, bool Absolute) {
  if (Path.empty() || Path.contains('\0') || Path.starts_with("/") != Absolute)
    return false;
  if (Absolute && Path == "/")
    return true;
  if (Path.ends_with("/") || Path.contains("//"))
    return false;
  SmallVector<StringRef, 16> Parts;
  Path.split(Parts, '/', -1, false);
  return llvm::none_of(
      Parts, [](StringRef Part) { return Part == "." || Part == ".."; });
}

std::string joinPath(StringRef Directory, StringRef File) {
  if (File.starts_with("/"))
    return File.str();
  SmallString<256> Path(Directory);
  sys::path::append(Path, sys::path::Style::posix, File);
  return Path.str().str();
}

class Cursor {
  StringRef Data;
  std::string &Problem;

public:
  size_t At;
  const size_t End;
  Cursor(StringRef Data, size_t Start, size_t Limit, std::string &Problem)
      : Data(Data), Problem(Problem), At(Start), End(Limit) {}
  void fail(StringRef Message) {
    if (Problem.empty())
      Problem = Message.str();
  }
  bool good() const { return Problem.empty(); }
  bool have(size_t N) {
    if (At > End || N > End - At) {
      fail("truncated DWARF line unit");
      return false;
    }
    return true;
  }
  uint64_t fixed(unsigned N) {
    if (!have(N))
      return 0;
    uint64_t V = 0;
    for (unsigned I = 0; I < N; ++I)
      V |= uint64_t(static_cast<uint8_t>(Data[At++])) << (I * 8);
    return V;
  }
  uint64_t uleb() {
    if (!have(1))
      return 0;
    unsigned N = 0;
    const char *Why = nullptr;
    auto *B = reinterpret_cast<const uint8_t *>(Data.data());
    uint64_t V = decodeULEB128(B + At, &N, B + End, &Why);
    if (Why || !N || !have(N) || N != getULEB128Size(V)) {
      fail("invalid or noncanonical ULEB128");
      return 0;
    }
    At += N;
    return V;
  }
  int64_t sleb() {
    if (!have(1))
      return 0;
    unsigned N = 0;
    const char *Why = nullptr;
    auto *B = reinterpret_cast<const uint8_t *>(Data.data());
    int64_t V = decodeSLEB128(B + At, &N, B + End, &Why);
    if (Why || !N || !have(N) || N != getSLEB128Size(V)) {
      fail("invalid or noncanonical SLEB128");
      return 0;
    }
    At += N;
    return V;
  }
  StringRef string() {
    if (!have(1))
      return {};
    size_t N = Data.substr(At, End - At).find('\0');
    if (N == StringRef::npos) {
      fail("unterminated DWARF line string");
      return {};
    }
    auto S = Data.substr(At, N);
    At += N + 1;
    return S;
  }
};

struct Section {
  std::string Name;
  uint64_t Offset = 0, Size = 0, Flags = 0;
  uint32_t Type = 0, Link = 0, Info = 0;
};
struct Relocation {
  uint64_t Offset;
  unsigned Width;
  unsigned Type;
  unsigned SymbolSection;
};
struct UnitLines {
  std::string CompDir;
  uint64_t End = 0;
  std::vector<const LineMap *> Files{nullptr};
};
struct Advance {
  size_t Begin, End;
  int64_t Value;
};

class Rewriter {
  MemoryBufferRef Old;
  const SourceMap &Map;
  StringRef Raw;
  RebasedObject Out;
  std::string Problem;
  std::vector<Section> Sections;
  std::map<std::string, unsigned, std::less<>> Debug;
  std::vector<Relocation> LineRelocs, InfoRelocs;
  std::map<size_t, UnitLines> LineUnits;
  std::set<size_t> AddressOperands;
  const Section *Info = nullptr, *Line = nullptr;
  bool fail(const Twine &Why) {
    if (Problem.empty())
      Problem = Why.str();
    return false;
  }

  static bool allowedSection(StringRef N) {
    if (N.empty())
      return true;
    for (StringRef Prefix :
         {".text", ".data", ".rodata", ".bss", ".tdata", ".tbss",
          ".gcc_except_table", ".eh_frame", ".init_array", ".fini_array",
          ".preinit_array", ".ctors", ".dtors", ".init", ".fini"}) {
      if (N == Prefix || (N.starts_with(Prefix) && N.size() > Prefix.size() &&
                          N[Prefix.size()] == '.'))
        return true;
    }
    return N == ".symtab" || N == ".strtab" || N == ".shstrtab" ||
           N == ".group" || N == ".symtab_shndx" || N == ".comment" ||
           N == ".note.GNU-stack" || N == ".note.gnu.property" ||
           N == ".llvm_addrsig";
  }

  bool inspectELF(const ELF64LEObjectFile &Obj) {
    const auto &EF = Obj.getELFFile();
    const auto &H = EF.getHeader();
    if (H.e_type != ELF::ET_REL || H.e_machine != ELF::EM_X86_64 || H.e_phnum ||
        H.e_ehsize != 64 || H.e_shentsize != 64)
      return fail("requires a little-endian ELF64 x86-64 relocatable object");
    auto All = EF.sections();
    if (!All)
      return fail(toString(All.takeError()));
    std::vector<std::pair<uint64_t, uint64_t>> Spans{{0, 64}};
    if (H.e_shoff > Raw.size() || All->size() > (Raw.size() - H.e_shoff) / 64)
      return fail("invalid ELF section table bounds");
    Spans.emplace_back(H.e_shoff, H.e_shoff + All->size() * 64);
    for (const auto &S : *All) {
      auto N = EF.getSectionName(S);
      if (!N)
        return fail(toString(N.takeError()));
      Section D{N->str(),  S.sh_offset, S.sh_size, S.sh_flags,
                S.sh_type, S.sh_link,   S.sh_info};
      if (S.sh_type != ELF::SHT_NOBITS && S.sh_size) {
        if (S.sh_offset > Raw.size() || S.sh_size > Raw.size() - S.sh_offset)
          return fail("invalid ELF section bounds");
        Spans.emplace_back(S.sh_offset, S.sh_offset + S.sh_size);
      }
      if (N->starts_with(".debug")) {
        if (*N != ".debug_info" && *N != ".debug_line" &&
            *N != ".debug_abbrev" && *N != ".debug_str" &&
            *N != ".debug_ranges" && *N != ".debug_aranges")
          return fail("unsupported debug section " + *N);
        if (S.sh_type != ELF::SHT_PROGBITS ||
            (S.sh_flags &
             (ELF::SHF_COMPRESSED | ELF::SHF_ALLOC | ELF::SHF_GROUP)) ||
            !Debug.emplace(N->str(), Sections.size()).second)
          return fail("unsupported or duplicate debug section " + *N);
      } else if (S.sh_type != ELF::SHT_RELA && S.sh_type != ELF::SHT_REL &&
                 !allowedSection(*N))
        return fail("unsupported ELF section " + *N);
      Sections.push_back(std::move(D));
    }
    std::sort(Spans.begin(), Spans.end());
    for (size_t I = 1; I < Spans.size(); ++I)
      if (Spans[I].first < Spans[I - 1].second)
        return fail("overlapping ELF sections or headers");
    for (StringRef N : {".debug_info", ".debug_abbrev", ".debug_line"})
      if (!Debug.count(N.str()))
        return fail("missing required section " + N);
    Info = &Sections[Debug.at(".debug_info")];
    Line = &Sections[Debug.at(".debug_line")];
    for (size_t I = 0; I < Sections.size(); ++I) {
      const Section &S = Sections[I];
      if (S.Type != ELF::SHT_RELA && S.Type != ELF::SHT_REL)
        continue;
      if (S.Info >= Sections.size())
        return fail("invalid relocation target");
      const Section &Target = Sections[S.Info];
      if (!StringRef(Target.Name).starts_with(".debug")) {
        if (StringRef(S.Name).starts_with(".rel.debug") ||
            StringRef(S.Name).starts_with(".rela.debug"))
          return fail("DWARF relocation section names the wrong target");
        continue;
      }
      if (S.Type != ELF::SHT_RELA || S.Name != ".rela" + Target.Name ||
          S.Link >= Sections.size() || Sections[S.Link].Type != ELF::SHT_SYMTAB)
        return fail("unsupported DWARF relocation section");
      if (&Target != Line && &Target != Info &&
          Target.Name != ".debug_ranges" && Target.Name != ".debug_aranges")
        return fail("relocations in unsupported DWARF data");
      auto Relocs = EF.relas((*All)[I]);
      if (!Relocs)
        return fail(toString(Relocs.takeError()));
      auto Symbols = EF.symbols(&(*All)[S.Link]);
      if (!Symbols)
        return fail(toString(Symbols.takeError()));
      for (const auto &R : *Relocs) {
        unsigned T = R.getType(false);
        unsigned Width = T == ELF::R_X86_64_64   ? 8
                         : T == ELF::R_X86_64_32 ? 4
                                                 : 0;
        if (!Width || R.r_offset > Target.Size ||
            Width > Target.Size - R.r_offset)
          return fail("unsupported or out-of-range DWARF relocation");
        if (&Target != Line && &Target != Info)
          continue;
        unsigned Index = R.getSymbol(false);
        if (Index >= Symbols->size())
          return fail("invalid DWARF relocation symbol");
        unsigned SymbolSection = (*Symbols)[Index].st_shndx;
        if (!SymbolSection || SymbolSection >= Sections.size())
          return fail("unresolved or extended DWARF relocation symbol");
        auto &List = &Target == Line ? LineRelocs : InfoRelocs;
        List.push_back({R.r_offset, Width, T, SymbolSection});
      }
    }
    for (auto *R : {&InfoRelocs, &LineRelocs}) {
      llvm::sort(
          *R, [](const auto &A, const auto &B) { return A.Offset < B.Offset; });
      for (size_t I = 1; I < R->size(); ++I)
        if ((*R)[I - 1].Offset + (*R)[I - 1].Width > (*R)[I].Offset)
          return fail("overlapping DWARF relocations");
    }
    return true;
  }

  bool addFile(UnitLines &U, ArrayRef<std::string> Dirs, StringRef File,
               uint64_t Dir, uint64_t Time, uint64_t Length,
               std::set<std::string> &Matched) {
    if (File.empty() || Dir >= Dirs.size())
      return fail("invalid line table file index");
    std::string Joined = joinPath(Dirs[Dir], File);
    if (!StringRef(Joined).starts_with("/"))
      return fail("ambiguous relative DWARF file");
    SmallString<256> Normal(Joined);
    sys::path::remove_dots(Normal, true, sys::path::Style::posix);
    auto It = Map.find(Normal.str().str());
    if (It == Map.end()) {
      U.Files.push_back(nullptr);
      return true;
    }
    // Avoid silently collapsing a potentially symlink-dependent '..' or two
    // file-table identities which resolve to the same requested source.
    if (Joined != Normal.str() || !Matched.insert(Joined).second)
      return fail("ambiguous mapped DWARF file " + Twine(Joined));
    if (Time || Length)
      return fail("mapped file carries a recorded time or length");
    U.Files.push_back(&It->second);
    return true;
  }

  bool parseLines(size_t Offset, StringRef CompDir) {
    auto Known = LineUnits.find(Offset);
    if (Known != LineUnits.end())
      return Known->second.CompDir == CompDir ||
             fail("shared line table has ambiguous compilation directories");
    StringRef Data = Raw.substr(Line->Offset, Line->Size);
    Cursor H(Data, Offset, Data.size(), Problem);
    uint64_t Size = H.fixed(4);
    if (!H.good() || Size == UINT32_MAX || !H.have(Size))
      return fail("invalid or DWARF64 line unit");
    size_t End = H.At + Size;
    Cursor P(Data, H.At, End, Problem);
    if (P.fixed(2) != 4)
      return fail("requires DWARF version 4 line programs");
    const auto HeaderSize = P.fixed(4);
    if (!P.have(HeaderSize))
      return false;
    size_t ProgramStart = P.At + HeaderSize;
    Cursor C(Data, P.At, ProgramStart, Problem);
    uint64_t Min = C.fixed(1), Max = C.fixed(1), Stmt = C.fixed(1);
    int8_t LineBase = static_cast<int8_t>(C.fixed(1));
    unsigned Range = C.fixed(1), OpBase = C.fixed(1);
    static constexpr uint8_t StdLengths[] = {0, 1, 1, 1, 1, 0,
                                             0, 0, 1, 0, 0, 1};
    if (!Min || Max != 1 || Stmt > 1 || !Range || OpBase != 13)
      return fail("unsupported line prologue");
    for (uint8_t S : StdLengths)
      if (C.fixed(1) != S)
        return fail("unsupported standard opcode table");
    std::vector<std::string> Dirs{CompDir.str()};
    while (C.good()) {
      StringRef S = C.string();
      if (S.empty())
        break;
      Dirs.push_back(joinPath(CompDir, S));
    }
    UnitLines U;
    U.CompDir = CompDir.str();
    U.End = End;
    std::set<std::string> Matched;
    while (C.good()) {
      StringRef F = C.string();
      if (F.empty())
        break;
      auto D = C.uleb(), T = C.uleb(), L = C.uleb();
      if (!C.good() || !addFile(U, Dirs, F, D, T, L, Matched))
        return false;
    }
    if (!C.good() || C.At != ProgramStart || U.Files.size() < 2)
      return fail("invalid line header extent");
    Cursor Ops(Data, ProgramStart, End, Problem);
    uint64_t File = 1;
    int64_t Before = 1, After = 1;
    std::optional<Advance> Last;
    bool Ended = false;
    auto move = [&](int64_t Delta) {
      if (AddOverflow(Before, Delta, Before) ||
          AddOverflow(After, Delta, After))
        return fail("DWARF line arithmetic overflow");
      return true;
    };
    while (Ops.At < End && Ops.good()) {
      size_t Start = Ops.At;
      unsigned Op = Ops.fixed(1);
      bool Row = false, EndSequence = false;
      Ended = false;
      if (Op == 0) {
        uint64_t Len = Ops.uleb();
        if (!Len || !Ops.have(Len))
          return fail("invalid extended line opcode");
        size_t SubEnd = Ops.At + Len;
        unsigned Ext = Ops.fixed(1);
        if (Ext == dwarf::DW_LNE_end_sequence) {
          if (Len != 1)
            return fail("invalid end_sequence");
          Row = EndSequence = true;
        } else if (Ext == dwarf::DW_LNE_set_address) {
          if (Len != 9)
            return fail("unsupported set_address width");
          AddressOperands.insert(Ops.At);
          Ops.fixed(8);
        } else if (Ext == dwarf::DW_LNE_set_discriminator) {
          Ops.uleb();
        } else
          return fail("unsupported extended line opcode");
        if (Ops.At != SubEnd)
          return fail("invalid extended opcode length");
      } else if (Op >= OpBase) {
        if (!move(LineBase + int((Op - OpBase) % Range)))
          return false;
        Row = true;
      } else
        switch (Op) {
        case dwarf::DW_LNS_copy:
          Row = true;
          break;
        case dwarf::DW_LNS_advance_pc:
        case dwarf::DW_LNS_set_column:
        case dwarf::DW_LNS_set_isa:
          Ops.uleb();
          break;
        case dwarf::DW_LNS_advance_line: {
          size_t Pos = Ops.At;
          auto V = Ops.sleb();
          if (!move(V))
            return false;
          Last = Advance{Pos, Ops.At, V};
          break;
        }
        case dwarf::DW_LNS_set_file:
          File = Ops.uleb();
          if (!File || File >= U.Files.size())
            return fail("invalid line-program file");
          break;
        case dwarf::DW_LNS_fixed_advance_pc:
          Ops.fixed(2);
          break;
        case dwarf::DW_LNS_negate_stmt:
        case dwarf::DW_LNS_set_basic_block:
        case dwarf::DW_LNS_const_add_pc:
        case dwarf::DW_LNS_set_prologue_end:
        case dwarf::DW_LNS_set_epilogue_begin:
          break;
        default:
          return fail("unsupported standard line opcode");
        }
      if (!Ops.good())
        return false;
      if (!Row)
        continue;
      if (Before < 0 || uint64_t(Before) > UINT32_MAX || After < 0 ||
          uint64_t(After) > UINT32_MAX)
        return fail("invalid emitted source line");
      int64_t Want = Before;
      if (const auto *M = U.Files[File])
        if (auto I = M->find(uint32_t(Before)); I != M->end())
          Want = I->second;
      int64_t Change = Want - After;
      if (Change) {
        if (Last) {
          int64_t V;
          if (AddOverflow(Last->Value, Change, V) ||
              getSLEB128Size(V) != Last->End - Last->Begin)
            return fail("advance_line would change encoded width");
          encodeSLEB128(V, reinterpret_cast<uint8_t *>(
                               Out.Bytes.data() + Line->Offset + Last->Begin));
        } else if (Op >= OpBase) {
          int64_t Index = (Op - OpBase) % Range + Change;
          if (Index < 0 || Index >= Range || int64_t(Op) + Change > 255)
            return fail("unrepresentable special line opcode");
          Out.Bytes[Line->Offset + Start] = static_cast<char>(Op + Change);
        } else if (Op == dwarf::DW_LNS_copy && Change - LineBase >= 0 &&
                   Change - LineBase < Range &&
                   int64_t(OpBase) + Change - LineBase <= 255) {
          Out.Bytes[Line->Offset + Start] =
              static_cast<char>(OpBase + Change - LineBase);
        } else
          return fail("line row has no same-width representation");
        ++Out.LineRows;
        After = Want;
      }
      Last.reset();
      if (EndSequence) {
        Before = After = 1;
        File = 1;
        Ended = true;
      }
    }
    if (!Ops.good() || !Ended)
      return fail("unterminated line sequence");
    LineUnits.emplace(Offset, std::move(U));
    return true;
  }

  bool fixCoordinate(const std::optional<DWARFAttribute> &File,
                     const std::optional<DWARFAttribute> &LineAttr,
                     const UnitLines &Lines, bool Decl) {
    if (!File && !LineAttr)
      return true;
    if (!File)
      return fail("source line without a resolvable file attribute");
    auto Index = File->Value.getAsUnsignedConstant();
    if (!Index || *Index >= Lines.Files.size())
      return fail("invalid DIE source file index");
    // Consumers can resolve a file and its line independently through
    // DW_AT_abstract_origin, DW_AT_specification or DW_AT_signature. Requiring
    // direct pairs guarantees that inherited coordinates come from one DIE.
    if (!LineAttr)
      return fail("source file is missing its paired line attribute");
    auto V = LineAttr->Value.getAsUnsignedConstant();
    if (!V || *V > UINT32_MAX || (!*Index && *V))
      return fail("invalid or ambiguous DIE source line");
    unsigned Width = 0;
    auto Form = LineAttr->Value.getForm();
    switch (Form) {
    case dwarf::DW_FORM_data1:
      Width = 1;
      break;
    case dwarf::DW_FORM_data2:
      Width = 2;
      break;
    case dwarf::DW_FORM_data4:
      Width = 4;
      break;
    case dwarf::DW_FORM_data8:
      Width = 8;
      break;
    case dwarf::DW_FORM_udata:
      break;
    default:
      return fail("unsupported DIE source-line form");
    }
    size_t Off = LineAttr->Offset, Length = LineAttr->ByteSize;
    if (Off > Info->Size || !Length || Length > Info->Size - Off ||
        (Width && Width != Length))
      return fail("invalid or indirect DIE source-line encoding");
    Cursor C(Raw.substr(Info->Offset, Info->Size), Off, Off + Length, Problem);
    uint64_t OldValue = Width ? C.fixed(Width) : C.uleb();
    if (!C.good() || C.At != C.End || OldValue != *V)
      return fail("relocated or malformed DIE source line");
    const LineMap *M = Lines.Files[*Index];
    if (!M)
      return true;
    auto It = M->find(static_cast<uint32_t>(*V));
    if (It == M->end() || It->second == *V)
      return true;
    uint64_t New = It->second;
    if ((Width < 8 && Width && New >= (uint64_t(1) << (8 * Width))) ||
        (!Width && getULEB128Size(New) != Length))
      return fail("DIE source line would change encoded width");
    auto *Dst =
        reinterpret_cast<uint8_t *>(Out.Bytes.data() + Info->Offset + Off);
    if (Width) {
      for (unsigned I = 0; I < Width; ++I)
        Dst[I] = static_cast<uint8_t>(New >> (8 * I));
    } else
      encodeULEB128(New, Dst);
    ++(Decl ? Out.DeclLines : Out.CallLines);
    return true;
  }

  bool inspectDWARF(const ObjectFile &Obj) {
    auto ErrorHandler = [&](Error E) { fail(toString(std::move(E))); };
    auto Context = DWARFContext::create(
        Obj, DWARFContext::ProcessDebugRelocations::Process, nullptr, "",
        ErrorHandler, ErrorHandler);
    size_t Rel = 0;
    uint64_t NextUnit = 0;
    auto allowedRelocs = [&](uint64_t Start, uint64_t Length, unsigned Width,
                             StringRef Target) {
      if (Start > NextUnit || Length > NextUnit - Start)
        return fail("DWARF field crosses its compilation-unit boundary");
      if (Rel < InfoRelocs.size() && InfoRelocs[Rel].Offset < Start)
        return fail("relocation in a DIE structure");
      while (Rel < InfoRelocs.size() &&
             InfoRelocs[Rel].Offset < Start + Length) {
        const auto &R = InfoRelocs[Rel++];
        if (!Width || R.Offset != Start || R.Width != Width ||
            R.Width != Length)
          return fail("relocation in an unsupported DIE attribute");
        const auto &S = Sections[R.SymbolSection];
        if (Target.empty() ? !(S.Flags & ELF::SHF_ALLOC) : Target != S.Name)
          return fail("DIE relocation points to an unexpected section");
      }
      return true;
    };
    for (auto &Unit : Context->normal_units()) {
      DWARFUnit &U = *Unit;
      if (U.getVersion() != 4 || U.getAddressByteSize() != 8 ||
          U.getFormat() != dwarf::DWARF32 || U.getOffset() != NextUnit ||
          U.getNextUnitOffset() <= NextUnit ||
          U.getNextUnitOffset() > Info->Size)
        return fail("unsupported or noncontiguous DWARF compilation unit");
      NextUnit = U.getNextUnitOffset();
      if (!allowedRelocs(U.getOffset() + 6, 4, 4, ".debug_abbrev"))
        return false;
      if (auto E = U.tryExtractDIEsIfNeeded(false))
        return fail(toString(std::move(E)));
      auto Root = U.getUnitDIE();
      if (!Root || Root.getTag() != dwarf::DW_TAG_compile_unit)
        return fail("expected a DWARF compile unit");
      auto SD = Root.find(dwarf::DW_AT_stmt_list),
           CD = Root.find(dwarf::DW_AT_comp_dir);
      if (!SD || !CD)
        return fail("missing line table or compilation directory");
      if (SD->getForm() != dwarf::DW_FORM_sec_offset ||
          (CD->getForm() != dwarf::DW_FORM_string &&
           CD->getForm() != dwarf::DW_FORM_strp))
        return fail("unsupported compilation-directory or statement-list form");
      auto Stmt = SD->getAsSectionOffset();
      auto Comp = CD->getAsCString();
      if (!Comp)
        return fail(toString(Comp.takeError()));
      if (!*Comp || !Stmt || *Stmt >= Line->Size ||
          !StringRef(*Comp).starts_with("/"))
        return fail("unresolvable line table or compilation directory");
      if (!parseLines(*Stmt, *Comp))
        return false;
      const UnitLines &UnitTable = LineUnits.at(*Stmt);
      uint64_t Position = U.getOffset() + U.getHeaderSize();
      bool SawDirectory = false;
      bool SawStatementList = false;
      for (unsigned I = 0, N = U.getNumDIEs(); I < N; ++I) {
        DWARFDie D = U.getDIEAtIndex(I);
        if (!D || D.getOffset() != Position || Position >= NextUnit)
          return fail("incomplete or overlapping DIE sequence");
        if (D.isNULL()) {
          if (Raw[Info->Offset + Position++] != 0)
            return fail("noncanonical DIE terminator");
          continue;
        }
        const auto *Abbrev = D.getAbbreviationDeclarationPtr();
        Position += getULEB128Size(Abbrev->getCode());
        std::optional<DWARFAttribute> DF, DL, CF, CL;
        for (const auto &A : D.attributes()) {
          // LLVM's reader accepts some last fixed-width fields which run past
          // the current unit into the next one. Such a field must never patch
          // the next unit's header, even when it parses as a valid line.
          if (A.Offset != Position || Position > NextUnit ||
              A.ByteSize > NextUnit - Position)
            return fail("DIE field crosses its compilation-unit boundary");
          Position += A.ByteSize;
          const auto Attr = A.Attr;
          if (Attr == dwarf::DW_AT_comp_dir || Attr == dwarf::DW_AT_stmt_list) {
            bool &Seen =
                Attr == dwarf::DW_AT_comp_dir ? SawDirectory : SawStatementList;
            if (I != 0 || std::exchange(Seen, true))
              return fail(
                  "duplicate or misplaced line-table identity attribute");
          }
          // Unknown vendor attributes can carry source coordinates; accept only
          // the non-coordinate extensions used by these ordinary DWARF4
          // objects.
          if (Attr == dwarf::DW_AT_macro_info || Attr == dwarf::DW_AT_macros ||
              (Attr > dwarf::DW_AT_linkage_name &&
               Attr != dwarf::DW_AT_GNU_discriminator &&
               Attr != dwarf::DW_AT_MIPS_linkage_name &&
               Attr != dwarf::DW_AT_GNU_all_call_sites &&
               Attr != dwarf::DW_AT_GNU_all_tail_call_sites))
            return fail("unsupported DWARF attribute " +
                        Twine(static_cast<unsigned>(Attr)));
          unsigned RelWidth = 0;
          StringRef Target;
          switch (A.Value.getForm()) {
          case dwarf::DW_FORM_addr:
            RelWidth = 8;
            break;
          case dwarf::DW_FORM_strp:
            RelWidth = 4;
            Target = ".debug_str";
            break;
          case dwarf::DW_FORM_ref_addr:
            RelWidth = 4;
            Target = ".debug_info";
            break;
          case dwarf::DW_FORM_sec_offset:
            if (Attr == dwarf::DW_AT_stmt_list) {
              RelWidth = 4;
              Target = ".debug_line";
            } else if (Attr == dwarf::DW_AT_ranges ||
                       Attr == dwarf::DW_AT_start_scope) {
              RelWidth = 4;
              Target = ".debug_ranges";
            } else
              return fail("unsupported DWARF section offset attribute");
            break;
          case dwarf::DW_FORM_data1:
          case dwarf::DW_FORM_data2:
          case dwarf::DW_FORM_data4:
          case dwarf::DW_FORM_data8:
          case dwarf::DW_FORM_udata:
          case dwarf::DW_FORM_sdata:
          case dwarf::DW_FORM_string:
          case dwarf::DW_FORM_flag:
          case dwarf::DW_FORM_flag_present:
          case dwarf::DW_FORM_block:
          case dwarf::DW_FORM_block1:
          case dwarf::DW_FORM_block2:
          case dwarf::DW_FORM_block4:
          case dwarf::DW_FORM_exprloc:
          case dwarf::DW_FORM_ref1:
          case dwarf::DW_FORM_ref2:
          case dwarf::DW_FORM_ref4:
          case dwarf::DW_FORM_ref8:
          case dwarf::DW_FORM_ref_udata:
            break;
          default:
            return fail("unsupported DWARF attribute form");
          }
          if (!allowedRelocs(A.Offset, A.ByteSize, RelWidth, Target))
            return false;
          std::optional<DWARFAttribute> *Field = nullptr;
          if (Attr == dwarf::DW_AT_decl_file)
            Field = &DF;
          if (Attr == dwarf::DW_AT_decl_line)
            Field = &DL;
          if (Attr == dwarf::DW_AT_call_file)
            Field = &CF;
          if (Attr == dwarf::DW_AT_call_line)
            Field = &CL;
          if (Field) {
            if (Field->has_value())
              return fail("duplicate DIE source coordinate");
            *Field = A;
          }
        }
        if (!fixCoordinate(DF, DL, UnitTable, true) ||
            !fixCoordinate(CF, CL, UnitTable, false))
          return false;
      }
      if (Position != NextUnit)
        return fail("unparsed data after the compilation-unit DIE tree");
    }
    if (!Problem.empty())
      return false;
    if (NextUnit != Info->Size || !NextUnit || Rel != InfoRelocs.size())
      return fail("incomplete DWARF unit or relocation coverage");
    uint64_t Pos = 0;
    for (const auto &KV : LineUnits) {
      if (KV.first != Pos)
        return fail("unreferenced or overlapping DWARF line tables");
      Pos = KV.second.End;
    }
    if (Pos != Line->Size)
      return fail("unreferenced DWARF line data");
    for (const auto &R : LineRelocs)
      if (R.Type != ELF::R_X86_64_64 || !AddressOperands.count(R.Offset) ||
          !(Sections[R.SymbolSection].Flags & ELF::SHF_EXECINSTR))
        return fail("line relocation is not a set_address operand");
    return true;
  }

public:
  Rewriter(MemoryBufferRef Input, const SourceMap &Lines)
      : Old(Input), Map(Lines), Raw(Input.getBuffer()) {}
  Expected<RebasedObject> run() {
    auto Binary = ObjectFile::createObjectFile(Old);
    if (!Binary)
      return Binary.takeError();
    auto *Obj = dyn_cast<ELF64LEObjectFile>(Binary->get());
    if (!Obj)
      return unsupported("requires ELF64 little endian");
    if (!inspectELF(*Obj))
      return unsupported(Problem);
    Out.Bytes.assign(Raw.begin(), Raw.end());
    if (!inspectDWARF(*Obj))
      return unsupported(Problem);
    return std::move(Out);
  }
};

// JSON object names must be unique independently of the consumer's choice of
// first/last-wins semantics. The LLVM parser first validates the full syntax.
bool uniqueObjectKeys(StringRef Text) {
  std::vector<std::set<std::string>> Keys;
  for (size_t I = 0; I < Text.size(); ++I) {
    char Ch = Text[I];
    if (Ch == '{') {
      Keys.emplace_back();
      continue;
    }
    if (Ch == '}') {
      if (Keys.empty())
        return false;
      Keys.pop_back();
      continue;
    }
    if (Ch != '"')
      continue;
    size_t Begin = I;
    while (++I < Text.size()) {
      if (Text[I] == '\\') {
        ++I;
        continue;
      }
      if (Text[I] == '"')
        break;
    }
    const size_t End = I + 1;
    size_t J = End;
    while (J < Text.size() && (Text[J] == ' ' || Text[J] == '\t' ||
                               Text[J] == '\r' || Text[J] == '\n'))
      ++J;
    if (J < Text.size() && Text[J] == ':') {
      if (Keys.empty())
        return false;
      auto Key = json::parse(Text.slice(Begin, End));
      if (!Key) {
        consumeError(Key.takeError());
        return false;
      }
      auto S = Key->getAsString();
      if (!S || !Keys.back().insert(S->str()).second)
        return false;
    }
  }
  return Keys.empty();
}
} // namespace

Expected<SourceMap> llvm::ohm::parseSourceMap(StringRef Json) {
  auto V = json::parse(Json);
  if (!V)
    return V.takeError();
  if (!uniqueObjectKeys(Json))
    return unsupported("duplicate JSON map key");
  auto *O = V->getAsObject();
  if (!O || O->size() != 2 || O->getInteger("version") != 1)
    return unsupported("invalid source map version or fields");
  auto *Files = O->getArray("files");
  if (!Files || Files->empty())
    return unsupported("empty or missing file map");
  SourceMap Result;
  for (const auto &File : *Files) {
    auto *F = File.getAsObject();
    if (!F || F->size() != 3)
      return unsupported("invalid file-map fields");
    auto D = F->getString("directory"), N = F->getString("filename");
    auto *Lines = F->getArray("lines");
    if (!D || !N || !Lines || !canonicalMapPath(*D, true) ||
        !canonicalMapPath(*N, N->starts_with("/")))
      return unsupported(
          "expected an unambiguous absolute directory and source filename");
    auto Pair = Result.try_emplace(joinPath(*D, *N));
    if (!Pair.second)
      return unsupported("duplicate mapped source path");
    for (const auto &Item : *Lines) {
      auto *A = Item.getAsArray();
      if (!A || A->size() != 2)
        return unsupported("invalid source line pair");
      auto Old = (*A)[0].getAsInteger(), New = (*A)[1].getAsInteger();
      if (!Old || !New || *Old <= 0 || *New <= 0 ||
          uint64_t(*Old) > UINT32_MAX || uint64_t(*New) > UINT32_MAX ||
          !Pair.first->second.emplace(uint32_t(*Old), uint32_t(*New)).second)
        return unsupported("invalid or duplicate source line mapping");
    }
  }
  return Result;
}

Expected<RebasedObject> llvm::ohm::rebaseDwarf4(MemoryBufferRef Old,
                                                const SourceMap &Map) {
  return Rewriter(Old, Map).run();
}
