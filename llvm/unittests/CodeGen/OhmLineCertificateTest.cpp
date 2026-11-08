//===- OhmLineCertificateTest.cpp ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/OhmLineCertificate.h"
#include "llvm-c/Core.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {
class OhmLineCertificateTest : public testing::Test {
protected:
  LLVMContext Context;
  Module M{"guard", Context};
  DIFile *Moved = DIFile::get(Context, "moved.rs", "/source");
  DIFile *Other = DIFile::get(Context, "other.rs", "/source");
  static constexpr StringLiteral Map =
      R"({"version":1,"files":[{"directory":"/source","filename":"moved.rs","lines":[[20,22],[21,23]]}]})";

  void SetUp() override {
    DIBuilder Builder(M);
    Builder.createCompileUnit(dwarf::DW_LANG_Rust, Moved, "test", false, "", 0);
    Builder.finalize();
    ohm_line_certificate_begin_v1();
    ohm_line_record_module_v1(&M);
  }

  void expectTransfer(int Expected) {
    ohm_line_record_end_module_v1();
    auto Buffer = ohm_line_certificate_end_v1();
    ASSERT_NE(Buffer, nullptr);
    EXPECT_EQ(ohm_line_certificate_check_v1(
                  LLVMGetBufferStart(Buffer), LLVMGetBufferSize(Buffer),
                  Map.data(), Map.size()),
              Expected);
    LLVMDisposeMemoryBuffer(Buffer);
  }
};

TEST_F(OhmLineCertificateTest, EqualLinesInDifferentFilesBecomeUnequal) {
  ohm_line_record_pair_v1(Moved, 20, Other, 20, 0);
  expectTransfer(2);
}

TEST_F(OhmLineCertificateTest, UnequalLinesInDifferentFilesBecomeEqual) {
  ohm_line_record_pair_v1(Moved, 20, Other, 22, 0);
  expectTransfer(2);
}

TEST_F(OhmLineCertificateTest, PreservesUnchangedComparisons) {
  ohm_line_record_pair_v1(Moved, 20, Other, 19, 0);
  ohm_line_record_pair_v1(Moved, 20, Moved, 21, 0);
  ohm_line_record_pair_v1(Moved, 0, Other, 21, 0);
  expectTransfer(0);
}

TEST_F(OhmLineCertificateTest, ResolvesThePreviousDwarfFileNumber) {
  ohm_line_record_file_v1(4, Other);
  ohm_line_record_pair_v1(Moved, 20, nullptr, 20, 4);
  expectTransfer(2);
}

TEST_F(OhmLineCertificateTest, RejectsAnAmbiguousDwarfFileNumber) {
  ohm_line_record_file_v1(4, Moved);
  ohm_line_record_file_v1(4, Other);
  ohm_line_record_pair_v1(Moved, 20, nullptr, 20, 4);
  ohm_line_record_end_module_v1();
  EXPECT_EQ(ohm_line_certificate_end_v1(), nullptr);
}

TEST_F(OhmLineCertificateTest, RequiresACompletedNativeEmission) {
  EXPECT_EQ(ohm_line_certificate_end_v1(), nullptr);
}
} // namespace
