//===- OhmDwarfRebaseTest.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/DWARF/OhmDwarfRebase.h"
#include "llvm-c/Core.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/DebugInfo/DWARF/OhmRuntimeLocations.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/ObjectYAML/yaml2obj.h"
#include "llvm/Support/LEB128.h"
#include "gtest/gtest.h"

using namespace llvm;
namespace {
void bytes(std::string &S, std::initializer_list<uint8_t> V) {
  for (uint8_t B : V)
    S.push_back(B);
}
void word(std::string &S, uint32_t V) {
  for (unsigned I = 0; I < 4; ++I)
    S.push_back(static_cast<char>(V >> (8 * I)));
}
void string0(std::string &S, StringRef V) {
  S.append(V.str());
  S.push_back(0);
}
std::string unit(StringRef Body) {
  std::string S;
  word(S, Body.size());
  S.append(Body.str());
  return S;
}

enum Case {
  Ordinary,
  InheritedLine,
  TruncatedCU,
  ResizedLine,
  TrailingTables,
  NoRoom,
  InteriorRef,
  CrossFileMCForm,
  ConstToSpecial,
  AdvanceToConst,
  NextRowZero,
  NegativeSourceRow
};
std::unique_ptr<object::ObjectFile> fixture(SmallString<0> &Storage,
                                            unsigned Decl, unsigned Call,
                                            unsigned Row, Case Kind = Ordinary,
                                            unsigned RuntimeLine = 0) {
  // A compile unit, a declaration, an inline call and a concrete instance
  // which may override only the file of an abstract declaration.
  const uint8_t Form = Call <= UINT8_MAX    ? dwarf::DW_FORM_data1
                       : Call <= UINT16_MAX ? dwarf::DW_FORM_data2
                                            : dwarf::DW_FORM_data4;
  std::string Abbrev;
  bytes(Abbrev,
        {1,    0x11, 1,    0x10, 0x17, 0x1b, 8,    0,    0,    2,    0x2e, 0,
         0x3a, 0x0b, 0x3b, 0x0b, 0,    0,    3,    0x1d, 0,    0x58, 0x0b, 0x59,
         Form, 0,    0,    4,    0x2e, 0,    0x31, 0x13, 0x3a, 0x0b, 0,    0,
         5,    0x11, 0,    0x10, 0x17, 0x1b, 8,    0x3a, 0x0b, 0x3b, 0x0b, 0,
         0,    6,    0x11, 0,    0x10, 0x17, 0x1b, 8,    0,    0,    0});
  auto Root = [](unsigned Code) {
    std::string B;
    bytes(B, {4, 0});
    word(B, 0);
    bytes(B, {8});
    B.push_back(Code);
    word(B, 0);
    string0(B, "/fixture");
    return B;
  };
  std::string Info;
  if (Kind == TruncatedCU) {
    // Code 5 still requires a one-byte decl_line. Its apparent value would
    // be the low byte of the second unit length, 21.
    std::string First = Root(5);
    bytes(First, {1});
    Info = unit(First) + unit(Root(6));
  } else {
    std::string B = Root(1);
    uint32_t AbstractOffset = 4 + B.size();
    bytes(B, {2, 1, static_cast<uint8_t>(Decl), 3, 1});
    if (Form == dwarf::DW_FORM_data4) {
      word(B, Call);
    } else {
      B.push_back(static_cast<char>(Call));
      if (Form == dwarf::DW_FORM_data2)
        B.push_back(static_cast<char>(Call >> 8));
    }
    if (Kind == InheritedLine) {
      bytes(B, {4});
      word(B, AbstractOffset);
      bytes(B, {2});
    }
    bytes(B, {0});
    Info = unit(B);
  }

  // Two source files share a basename. One line program emits a row using
  // the DWARF4 advance_line operand; all other program state is fixed.
  std::string Header;
  bytes(Header, {1, 1, 1, 251, 14, 13, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1});
  string0(Header, "src");
  string0(Header, "other");
  bytes(Header, {0});
  string0(Header, "same.rs");
  bytes(Header, {1, 0, 0});
  string0(Header, "same.rs");
  bytes(Header, {2, 0, 0, 0});
  std::string Line;
  bytes(Line, {4, 0});
  word(Line, Header.size());
  Line += Header;
  const bool Resize = Kind == ResizedLine || Kind == TrailingTables ||
                      Kind == NoRoom || Kind == InteriorRef;
  unsigned AddressOffset = 0;
  if (Resize) {
    if (Kind == TrailingTables) {
      // Align the end of the one-byte advance and keep the same line-program
      // prefix when an independently emitted advance needs two bytes.
      const unsigned Padding = (8 - ((4 + Line.size() + 19) % 8)) % 8;
      Line.append(Padding, char(dwarf::DW_LNS_set_basic_block));
    }
    bytes(Line, {3});
    uint8_t Encoded[16];
    unsigned N = encodeSLEB128(int64_t(Row) - 1, Encoded);
    Line.append(reinterpret_cast<char *>(Encoded), N);
    bytes(Line, {1, 0, 9, 2});
    AddressOffset = 4 + Line.size();
    Line.append(8, '\0');
    bytes(Line, {2, 1, 0, 1, 1});
  } else if (Kind == NextRowZero || Kind == NegativeSourceRow) {
    // The native encoder emits two one-byte SLEB128 advance_line operands.
    // The second row is zero, or malformed when testing invalid old input.
    bytes(Line, {3, static_cast<uint8_t>(Row - 1), 1, 3,
                 static_cast<uint8_t>(128 - Row - (Kind == NegativeSourceRow)),
                 1, 2, 1, 0, 1, 1});
  } else if (Kind == CrossFileMCForm) {
    // File 1, line 20, then file 2, line 50. The native second delta
    // changes from +30 to +5 after inserting 25 lines in file 1.
    bytes(Line, {3, 19, 1, 4, 2, 3, 30, 32, 2, 1, 0, 1, 1});
  } else if (Kind == ConstToSpecial || Kind == AdvanceToConst) {
    // In both cases, moving file 1's line 20 to 26 reduces the next
    // line delta from +3 to -3, crossing one MC address threshold.
    bytes(Line, {3, 19, 1, 4, 2});
    if (Kind == ConstToSpecial)
      bytes(Line, {8, 21});     // const_add_pc 17, special +3
    else
      bytes(Line, {2, 34, 21}); // advance_pc 34, special +3
    bytes(Line, {2, 1, 0, 1, 1});
  } else {
    bytes(Line, {3, static_cast<uint8_t>(Row - 1), 1, 2, 1, 0, 1, 1});
  }
  std::string Yaml = R"(!ELF
FileHeader:
  Class: ELFCLASS64
  Data: ELFDATA2LSB
  Type: ET_REL
  Machine: EM_X86_64
Sections:
  - Name: .text
    Type: SHT_PROGBITS
    Flags: [ SHF_ALLOC, SHF_EXECINSTR ]
)";
  Yaml +=
      RuntimeLine ? "    Content: '488D0500000000C3'\n" : "    Content: 'C3'\n";
  if (RuntimeLine) {
    std::string Record(16, '\0');
    Record[8] = 11; // Length of "src/same.rs".
    word(Record, RuntimeLine);
    word(Record, 13);
    Yaml += R"(  - Name: .rodata.str1.1
    Type: SHT_PROGBITS
    Flags: [SHF_ALLOC, SHF_MERGE, SHF_STRINGS]
    EntSize: 1
    AddressAlign: 1
    Content: '7372632f73616d652e727300'
  - Name: .data.rel.ro..Lanon.0123456789abcdef0123456789abcdef.3
    Type: SHT_PROGBITS
    Flags: [SHF_ALLOC, SHF_WRITE]
    AddressAlign: 8
)";
    Yaml += "    Content: '" + toHex(Record) + "'\n";
    Yaml +=
        R"(  - Name: .rela.data.rel.ro..Lanon.0123456789abcdef0123456789abcdef.3
    Type: SHT_RELA
    Link: .symtab
    Info: .data.rel.ro..Lanon.0123456789abcdef0123456789abcdef.3
    Relocations:
      - { Offset: 0, Type: R_X86_64_64, Symbol: file_section }
  - Name: .rela.text
    Type: SHT_RELA
    Link: .symtab
    Info: .text
    Relocations:
      - { Offset: 3, Type: R_X86_64_PC32, Symbol: runtime_section, Addend: -4 }
)";
  }
  auto Section = [&](StringRef N, StringRef V) {
    Yaml += "  - Name: " + N.str() + "\n    Type: SHT_PROGBITS\n";
    if (Kind == TrailingTables && N == ".debug_line")
      Yaml += "    AddressAlign: 8\n";
    Yaml += "    Content: '" + toHex(V) + "'\n";
  };
  Section(".debug_abbrev", Abbrev);
  Section(".debug_info", Info);
  Section(".debug_line", unit(Line));
  if (Resize) {
    if (Kind != TrailingTables)
      Yaml += "  - Name: .rodata.after_line\n    Type: SHT_PROGBITS\n    "
              "AddressAlign: " +
              std::to_string(Kind == NoRoom ? 1 : 32) +
              "\n    Content: '0000000000000000'\n";
    Yaml +=
        "  - Name: .rela.debug_line\n    Type: SHT_RELA\n    Link: .symtab\n"
        "    Info: .debug_line\n    Relocations:\n      - Offset: " +
        std::to_string(AddressOffset) +
        "\n        Type: R_X86_64_64\n        Symbol: text_section\n";
    if (Kind == InteriorRef)
      Yaml += "  - Name: .rela.rodata.after_line\n    Type: SHT_RELA\n    "
              "Link: .symtab\n    Info: .rodata.after_line\n"
              "    Relocations:\n      - Offset: 0\n        Type: "
              "R_X86_64_64\n        Symbol: line_section\n        Addend: 7\n";
    Yaml += "Symbols:\n  - Name: text_section\n    StName: 0\n    Type: "
            "STT_SECTION\n    Section: .text\n"
            "  - Name: line_section\n    StName: 0\n    Type: STT_SECTION\n    "
            "Section: .debug_line\n";
    if (RuntimeLine)
      Yaml +=
          R"(  - { Name: file_section, StName: 0, Type: STT_SECTION, Section: .rodata.str1.1 }
  - Name: runtime_section
    StName: 0
    Type: STT_SECTION
    Section: .data.rel.ro..Lanon.0123456789abcdef0123456789abcdef.3
)";
  }
  return yaml::yaml2ObjectFile(
      Storage, Yaml, [](const Twine &Error) { errs() << Error << "\n"; });
}
TEST(OhmDwarfRebase, MatchesIndependentlyEmittedFullObject) {
  SmallString<0> Old, Native;
  ASSERT_TRUE(fixture(Old, 20, 20, 20));
  ASSERT_TRUE(fixture(Native, 22, 22, 22));
  ohm::SourceMap Map{{"/fixture/src/same.rs", {{20, 22}}}};
  auto R = ohm::rebaseDwarf4({Old, "cached"}, Map);
  ASSERT_TRUE(static_cast<bool>(R)) << toString(R.takeError());
  EXPECT_EQ(StringRef(R->Bytes.data(), R->Bytes.size()), Native.str());
  EXPECT_EQ(R->LineRows, 1u);
  EXPECT_EQ(R->DeclLines, 1u);
  EXPECT_EQ(R->CallLines, 1u);
}
TEST(OhmDwarfRebase, AcceptsAnObjectAtAnOddPacketOffset) {
  SmallString<0> Old, Native, Packet;
  ASSERT_TRUE(fixture(Old, 20, 20, 20));
  ASSERT_TRUE(fixture(Native, 22, 22, 22));
  Packet.push_back('\0');
  Packet.append(Old);
  StringRef Input = Packet.str().drop_front();
  ASSERT_EQ(reinterpret_cast<uintptr_t>(Input.data()) % 2, 1u);

  StringRef Map =
      R"({"version":1,"files":[{"directory":"/fixture","filename":"src/same.rs","lines":[[20,22]]}]})";
  StringRef Locations = "[]";
  LLVMMemoryBufferRef Output = nullptr;
  const char *Reason = nullptr;
  ASSERT_EQ(0, ohm_debug_rebase_locations_v1(
                   Map.data(), Map.size(), Locations.data(), Locations.size(),
                   Input.data(), Input.size(), &Output, &Reason))
      << (Reason ? Reason : "");
  ASSERT_NE(Output, nullptr);
  EXPECT_EQ(Native.str(),
            StringRef(LLVMGetBufferStart(Output), LLVMGetBufferSize(Output)));
  LLVMDisposeMemoryBuffer(Output);
}

TEST(OhmDwarfRebase, RejectsUnrepresentableDIEAndInheritedFile) {
  SmallString<0> Old;
  ASSERT_TRUE(fixture(Old, 250, 20, 20));
  auto Width = ohm::rebaseDwarf4({Old, "cached"},
                                 {{"/fixture/src/same.rs", {{250, 256}}}});
  ASSERT_FALSE(static_cast<bool>(Width));
  EXPECT_NE(toString(Width.takeError()).find("encoded width"),
            std::string::npos);
  Old.clear();
  ASSERT_TRUE(fixture(Old, 20, 20, 20, InheritedLine));
  auto Split = ohm::rebaseDwarf4({Old, "cached"},
                                 {{"/fixture/src/same.rs", {{20, 22}}}});
  ASSERT_FALSE(static_cast<bool>(Split));
  EXPECT_NE(toString(Split.takeError()).find("paired line"), std::string::npos);
}
TEST(OhmDwarfRebase, RejectsNarrowerInlineCallLines) {
  for (unsigned Boundary : {256u, 65536u}) {
    SmallString<0> Old, Native;
    ASSERT_TRUE(fixture(Old, 20, Boundary + 1, 20));
    ASSERT_TRUE(fixture(Native, 20, Boundary, 20));
    auto SameWidth = ohm::rebaseDwarf4(
        {Old, "cached"},
        {{"/fixture/src/same.rs", {{Boundary + 1, Boundary}}}});
    ASSERT_TRUE(static_cast<bool>(SameWidth))
        << toString(SameWidth.takeError());
    EXPECT_EQ(StringRef(SameWidth->Bytes.data(), SameWidth->Bytes.size()),
              Native.str());

    auto Narrower = ohm::rebaseDwarf4(
        {Native, "cached"},
        {{"/fixture/src/same.rs", {{Boundary, Boundary - 1}}}});
    ASSERT_FALSE(static_cast<bool>(Narrower));
    EXPECT_NE(toString(Narrower.takeError()).find("encoded width"),
              std::string::npos);
  }
}

TEST(OhmDwarfRebase, TruncatedDIECannotRewriteFollowingUnitLength) {
  SmallString<0> Old;
  ASSERT_TRUE(fixture(Old, 0, 0, 20, TruncatedCU));
  auto R = ohm::rebaseDwarf4({Old, "cached"},
                             {{"/fixture/src/same.rs", {{21, 22}}}});
  ASSERT_FALSE(static_cast<bool>(R));
  EXPECT_NE(toString(R.takeError()).find("compilation-unit boundary"),
            std::string::npos);
}
TEST(OhmDwarfRebase, MovesBackBeforeAnUnchangedZeroLine) {
  SmallString<0> Old, Future;
  ASSERT_TRUE(fixture(Old, 20, 20, 20, NextRowZero));
  ASSERT_TRUE(fixture(Future, 16, 16, 16, NextRowZero));
  ohm::SourceMap Map{{"/fixture/src/same.rs", {{20, 16}}}};
  auto Rebased = ohm::rebaseDwarf4({Old, "original"}, Map);
  ASSERT_TRUE(bool(Rebased)) << toString(Rebased.takeError());
  EXPECT_EQ(Future.str(), StringRef(Rebased->Bytes.data(), Rebased->Bytes.size()));

  Old.clear();
  ASSERT_TRUE(fixture(Old, 20, 20, 20, NegativeSourceRow));
  auto Invalid = ohm::rebaseDwarf4({Old, "malformed"}, Map);
  ASSERT_FALSE(bool(Invalid));
  EXPECT_NE(toString(Invalid.takeError()).find("invalid emitted source line"),
            std::string::npos);
}

TEST(OhmDwarfRebase, ResizesAnExplicitLineOperandAndRelocatesAddresses) {
  SmallString<0> Old, Future;
  ASSERT_TRUE(fixture(Old, 20, 20, 60, ResizedLine));
  ASSERT_TRUE(fixture(Future, 20, 20, 90, ResizedLine));
  auto R = ohm::rebaseDwarf4({Old, "cached"},
                             {{"/fixture/src/same.rs", {{60, 90}}}});
  ASSERT_TRUE(bool(R)) << toString(R.takeError());
  EXPECT_EQ(Future.str(), StringRef(R->Bytes.data(), R->Bytes.size()));
}

TEST(OhmDwarfRebase, GrowsAndShrinksLineTablesBeforeRelocations) {
  SmallString<0> Old, Future;
  ASSERT_TRUE(fixture(Old, 20, 20, 60, TrailingTables));
  ASSERT_TRUE(fixture(Future, 20, 20, 90, TrailingTables));
  ASSERT_GT(Future.size(), Old.size());
  auto Grow = ohm::rebaseDwarf4({Old, "cached"},
                                {{"/fixture/src/same.rs", {{60, 90}}}});
  ASSERT_TRUE(bool(Grow)) << toString(Grow.takeError());
  EXPECT_EQ(StringRef(Grow->Bytes.data(), Grow->Bytes.size()), Future.str());
  auto Shrink = ohm::rebaseDwarf4({Future, "cached"},
                                  {{"/fixture/src/same.rs", {{90, 60}}}});
  ASSERT_TRUE(bool(Shrink)) << toString(Shrink.takeError());
  EXPECT_EQ(StringRef(Shrink->Bytes.data(), Shrink->Bytes.size()), Old.str());
}

TEST(OhmDwarfRebase, UpdatesTheCallerRecordBeforeItsObjectGrows) {
  SmallString<0> Old, Future;
  ASSERT_TRUE(fixture(Old, 20, 20, 60, TrailingTables, 60));
  ASSERT_TRUE(fixture(Future, 20, 20, 90, TrailingTables, 90));
  ASSERT_GT(Future.size(), Old.size());
  constexpr StringLiteral Map =
      R"({"version":1,"files":[{"directory":"/fixture","filename":"src/same.rs","lines":[[60,90]]}]})";
  constexpr StringLiteral Witness =
      R"([{"global":"anon.0123456789abcdef0123456789abcdef.3","file_global":"anon.0123456789abcdef0123456789abcdef.2","directory":"/fixture","file":"src/same.rs","file_nul":true,"old_line":60,"new_line":90,"column":13}])";
  const char *Reason = nullptr;
  LLVMMemoryBufferRef Result = nullptr;
  ASSERT_EQ(0, ohm_debug_rebase_locations_v1(
                   Map.data(), Map.size(), Witness.data(), Witness.size(),
                   Old.data(), Old.size(), &Result, &Reason))
      << (Reason ? Reason : "");
  ASSERT_NE(Result, nullptr);
  EXPECT_EQ(Future.str(),
            StringRef(LLVMGetBufferStart(Result), LLVMGetBufferSize(Result)));
  LLVMDisposeMemoryBuffer(Result);
}

TEST(OhmDwarfRebase, ResizingRequiresStableLayoutAndNoInteriorReferences) {
  for (Case Kind : {NoRoom, InteriorRef}) {
    SmallString<0> Old;
    ASSERT_TRUE(fixture(Old, 20, 20, 60, Kind));
    auto R = ohm::rebaseDwarf4({Old, "cached"},
                               {{"/fixture/src/same.rs", {{60, 90}}}});
    EXPECT_FALSE(bool(R));
    if (!R)
      consumeError(R.takeError());
  }
}
TEST(OhmDwarfRebase, RejectsCrossFileNativeOpcodeChange) {
  SmallString<0> Old;
  for (Case Kind : {CrossFileMCForm, ConstToSpecial, AdvanceToConst}) {
    Old.clear();
    ASSERT_TRUE(fixture(Old, 20, 20, 20, Kind));
    auto R = ohm::rebaseDwarf4(
        {Old, "cached"},
        {{"/fixture/src/same.rs", {{20, Kind == CrossFileMCForm ? 45u : 26u}}}});
    ASSERT_FALSE(bool(R));
    EXPECT_NE(toString(R.takeError()).find("native MC line opcode form"),
              std::string::npos);
  }
}
TEST(OhmDwarfRebase, RejectsDuplicatePathAndJsonKeys) {
  for (
      StringRef Json :
      {R"({"version":1,"version":1,"files":[]})",
       R"({"version":1,"files":[{"directory":"/w","filename":"f","lines":[]},{"directory":"/w","filename":"f","lines":[]}]})"}) {
    auto R = ohm::parseSourceMap(Json);
    ASSERT_FALSE(static_cast<bool>(R));
    consumeError(R.takeError());
  }
}
} // namespace
