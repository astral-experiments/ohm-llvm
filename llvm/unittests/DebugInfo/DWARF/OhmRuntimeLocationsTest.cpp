//===- OhmRuntimeLocationsTest.cpp ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "llvm/DebugInfo/DWARF/OhmRuntimeLocations.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/ObjectYAML/yaml2obj.h"
#include "llvm/Support/Endian.h"
#include "gtest/gtest.h"

using namespace llvm;
namespace {
constexpr StringLiteral Name =
    ".data.rel.ro..Lanon.0123456789abcdef0123456789abcdef.3";
constexpr StringLiteral Witness =
    R"([{"global":"anon.0123456789abcdef0123456789abcdef.3","file_global":"anon.0123456789abcdef0123456789abcdef.2","directory":"/fixture","file":"src/f.rs","file_nul":true,"old_line":7,"new_line":37,"column":13}])";
const ohm::SourceMap Map{{"/fixture/src/f.rs", {{7, 37}}}};

std::string record(unsigned Line) {
  char Data[24] = {};
  support::endian::write64le(Data + 8, 8);
  support::endian::write32le(Data + 16, Line);
  support::endian::write32le(Data + 20, 13);
  return toHex(StringRef(Data, sizeof(Data)));
}
enum Mutation { None, Load, FileSuffix, Alias, NewCollision };
std::unique_ptr<object::ObjectFile> fixture(SmallString<0> &Out, unsigned Line,
                                            Mutation M = None) {
  std::string Yaml;
  raw_string_ostream O(Yaml);
  O << R"(!ELF
FileHeader:
  Class: ELFCLASS64
  Data: ELFDATA2LSB
  Type: ET_REL
  Machine: EM_X86_64
Sections:
  - Name: .text
    Type: SHT_PROGBITS
    Flags: [SHF_ALLOC, SHF_EXECINSTR]
    Content: )"
    << (M == Load ? "'488B0500000000C3'" : "'488D0500000000C3'") << R"(
  - Name: .rodata.str1.1
    Type: SHT_PROGBITS
    Flags: [SHF_ALLOC, SHF_MERGE, SHF_STRINGS]
    EntSize: 1
    AddressAlign: 1
    Content: '6f74686572007372632f662e727300'
)";
  auto Data = [&](StringRef N, unsigned V) {
    O << "  - Name: " << N
      << "\n    Type: SHT_PROGBITS\n    Flags: [SHF_ALLOC, SHF_WRITE]\n"
      << "    AddressAlign: 8\n    Content: '" << record(V) << "'\n"
      << "  - Name: .rela" << N
      << "\n    Type: SHT_RELA\n    Link: .symtab\n    Info: " << N
      << "\n    Relocations:\n      - Offset: 0\n        Type: R_X86_64_64\n"
      << "        Symbol: file_section\n        Addend: "
      << (M == FileSuffix ? 7 : 6) << "\n";
  };
  Data(Name, Line);
  if (M == NewCollision)
    Data(".data.rel.ro.other-aggregate", 37);
  O << R"(  - Name: .rela.text
    Type: SHT_RELA
    Link: .symtab
    Info: .text
    Relocations:
      - Offset: 3
        Type: R_X86_64_PC32
        Symbol: runtime_section
        Addend: -4
Symbols:
  - Name: file_section
    StName: 0
    Type: STT_SECTION
    Section: .rodata.str1.1
  - Name: runtime_section
    StName: 0
    Type: STT_SECTION
    Section: )"
    << Name << "\n";
  if (M == Alias)
    O << "  - Name: alias\n    Type: STT_OBJECT\n    Section: " << Name
      << "\n    Size: 24\n";
  return yaml::yaml2ObjectFile(Out, Yaml,
                               [](const Twine &E) { errs() << E << "\n"; });
}

TEST(OhmRuntimeLocations, RewritesTheNamedGlobalAndPreservesTheWholeObject) {
  SmallString<0> Old, Future;
  ASSERT_TRUE(fixture(Old, 7));
  ASSERT_TRUE(fixture(Future, 37));
  SmallVector<char> Result(Old.begin(), Old.end());
  auto E = ohm::rebaseRuntimeLocations({Old, "cached"}, Map, Witness, Result);
  ASSERT_FALSE(bool(E)) << toString(std::move(E));
  EXPECT_EQ(Future.str(), StringRef(Result.data(), Result.size()));
}

TEST(OhmRuntimeLocations, RejectsLoadSuffixAliasAndProspectiveMerge) {
  for (Mutation M : {Load, FileSuffix, Alias, NewCollision}) {
    SmallString<0> Old;
    ASSERT_TRUE(fixture(Old, 7, M));
    SmallVector<char> Result(Old.begin(), Old.end());
    auto E = ohm::rebaseRuntimeLocations({Old, "cached"}, Map, Witness, Result);
    EXPECT_TRUE(bool(E));
    consumeError(std::move(E));
    EXPECT_EQ(Old.str(), StringRef(Result.data(), Result.size()));
  }
}

TEST(OhmRuntimeLocations, RejectsAnAmbiguousOrUnmappedChange) {
  SmallString<0> Old;
  ASSERT_TRUE(fixture(Old, 7));
  SmallVector<char> Result(Old.begin(), Old.end());
  std::string Duplicate = Witness.str();
  Duplicate.insert(Duplicate.find("\"old_line\""), "\"old_line\":90,");
  for (const auto &Bad : {Duplicate, Witness.str()}) {
    const ohm::SourceMap Other{{"/unrelated/src/f.rs", {{7, 37}}}};
    auto E = ohm::rebaseRuntimeLocations(
        {Old, "cached"}, Bad == Duplicate ? Map : Other, Bad, Result);
    EXPECT_TRUE(bool(E));
    consumeError(std::move(E));
    EXPECT_EQ(Old.str(), StringRef(Result.data(), Result.size()));
  }
}
} // namespace
