//===- OhmDwarfRebaseTest.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/DWARF/OhmDwarfRebase.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/ObjectYAML/yaml2obj.h"
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

enum Case { Ordinary, InheritedLine, TruncatedCU };
std::unique_ptr<object::ObjectFile> fixture(SmallString<0> &Storage,
                                            unsigned Decl, unsigned Call,
                                            unsigned Row,
                                            Case Kind = Ordinary) {
  // A compile unit, a declaration, an inline call and a concrete instance
  // which may override only the file of an abstract declaration.
  std::string Abbrev;
  bytes(Abbrev,
        {1,    0x11, 1,    0x10, 0x17, 0x1b, 8,    0,    0,    2,    0x2e, 0,
         0x3a, 0x0b, 0x3b, 0x0b, 0,    0,    3,    0x1d, 0,    0x58, 0x0b, 0x59,
         0x0b, 0,    0,    4,    0x2e, 0,    0x31, 0x13, 0x3a, 0x0b, 0,    0,
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
    bytes(B,
          {2, 1, static_cast<uint8_t>(Decl), 3, 1, static_cast<uint8_t>(Call)});
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
  bytes(Line, {3, static_cast<uint8_t>(Row - 1), 1, 2, 1, 0, 1, 1});
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
    Content: 'C3'
)";
  auto Section = [&](StringRef N, StringRef V) {
    Yaml += "  - Name: " + N.str() +
            "\n    Type: SHT_PROGBITS\n    Content: '" + toHex(V) + "'\n";
  };
  Section(".debug_abbrev", Abbrev);
  Section(".debug_info", Info);
  Section(".debug_line", unit(Line));
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
TEST(OhmDwarfRebase, TruncatedDIECannotRewriteFollowingUnitLength) {
  SmallString<0> Old;
  ASSERT_TRUE(fixture(Old, 0, 0, 20, TruncatedCU));
  auto R = ohm::rebaseDwarf4({Old, "cached"},
                             {{"/fixture/src/same.rs", {{21, 22}}}});
  ASSERT_FALSE(static_cast<bool>(R));
  EXPECT_NE(toString(R.takeError()).find("compilation-unit boundary"),
            std::string::npos);
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
