// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "llvm/Bitcode/OhmDebugObjectSignature.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {
class DebugObjectSignatureTest : public testing::Test {
protected:
  const std::string OldSum = std::string(32, 'a');
  const std::string NewSum = std::string(32, 'b');
  const std::string Mapping =
      R"({"version":1,"files":[{"directory":"/src","filename":"f.c","lines":[[3,4]]}]})";
  std::string Proof;
  char *Origin = nullptr;
  size_t OriginSize = 0;

  void SetUp() override {
    auto Digest = toHex(SHA256::hash(arrayRefFromStringRef(Mapping)), true);
    auto File =
        formatv(
            R"("directory":"/src","filename":"f.c","ordered_byte_identical_old_lines":3,"new_lines":4,"old":{{"md5":"{0}","sha1":"{2}","sha256":"{3}"},"new":{{"md5":"{1}","sha1":"{2}","sha256":"{3}"})",
            OldSum, NewSum, std::string(40, '1'), std::string(64, '2'))
            .str();
    Proof = formatv(R"({{"version":1,"map_sha256":"{0}","files":[{{{1}}]})",
                    Digest, File)
                .str();
  }

  ~DebugObjectSignatureTest() override { ohm_bc_free_v1(Origin); }

  std::unique_ptr<Module> module(LLVMContext &Context, StringRef Checksum,
                                 unsigned Line, StringRef Operand) {
    auto IR = formatv(R"(
      target triple = "x86_64-unknown-linux-gnu"
      define i32 @f(i32 %x) !dbg !4 {{
        %y = add i32 %x, 1, !dbg !5, !guard !6
        ret i32 %y, !dbg !5
      }
      !llvm.dbg.cu = !{{!0}
      !llvm.module.flags = !{{!3}
      !0 = distinct !DICompileUnit(language: DW_LANG_C, file: !1, producer: "test", isOptimized: true, runtimeVersion: 0, emissionKind: LineTablesOnly)
      !1 = !DIFile(filename: "f.c", directory: "/src", checksumkind: CSK_MD5, checksum: "{0}")
      !2 = !DISubroutineType(types: !{{})
      !3 = !{{i32 2, !"Debug Info Version", i32 3}
      !4 = distinct !DISubprogram(name: "f", scope: !1, file: !1, line: 1, type: !2, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)
      !5 = !DILocation(line: {1}, column: 2, scope: !4)
      !6 = !{{!"{2}"}
    )",
                      Checksum, Line, Operand)
                  .str();
    SMDiagnostic Error;
    auto M = parseAssemblyString(IR, Error, Context);
    if (!M)
      ADD_FAILURE() << Error.getMessage().str() << " at " << Error.getLineNo()
                    << ": " << Error.getLineContents().str();
    return M;
  }

  bool matches(Module &M) {
    return ohm_bc_match_v1(wrap(&M), Origin, OriginSize, Mapping.data(),
                           Mapping.size(), Proof.data(), Proof.size());
  }
};

TEST_F(DebugObjectSignatureTest, MatchesOnlyExistingSourceCoordinates) {
  LLVMContext Before, After, Inserted;
  auto Old = module(Before, OldSum, 3, "payload");
  auto Moved = module(After, NewSum, 4, "payload");
  auto Insertion = module(Inserted, NewSum, 3, "payload");
  ASSERT_TRUE(Old && Moved && Insertion);
  ASSERT_TRUE(ohm_bc_save_v1(wrap(Old.get()), &Origin, &OriginSize));
  EXPECT_TRUE(matches(*Moved));
  EXPECT_FALSE(matches(*Insertion));
  Proof.clear();
  EXPECT_FALSE(matches(*Moved));
}

TEST_F(DebugObjectSignatureTest, RejectsChecksumAlsoUsedAsOrdinaryMetadata) {
  LLVMContext Before, After;
  auto Old = module(Before, OldSum, 3, OldSum);
  auto Moved = module(After, NewSum, 4, NewSum);
  ASSERT_TRUE(Old && Moved);
  ASSERT_TRUE(ohm_bc_save_v1(wrap(Old.get()), &Origin, &OriginSize));
  EXPECT_FALSE(matches(*Moved));
}
} // namespace
