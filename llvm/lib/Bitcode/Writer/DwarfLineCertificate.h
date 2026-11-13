//===- DwarfLineCertificate.h - Normalize source locations for bitcode ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_BITCODE_WRITER_DWARFLINECERTIFICATE_H
#define LLVM_LIB_BITCODE_WRITER_DWARFLINECERTIFICATE_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/BLAKE3.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace {
class DwarfLineCertificate {
  using File = llvm::DIFile;
  using Metadata = llvm::Metadata;
  using MDString = llvm::MDString;

  llvm::SmallPtrSet<const Metadata *, 32> Visited;
  llvm::SmallPtrSet<const MDString *, 32> Checksums;
  llvm::SmallPtrSet<const MDString *, 32> OtherStrings;
  llvm::DenseMap<const File *, uint32_t> FileIds;
  llvm::DenseMap<uint64_t, uint32_t> LineRanks;
  std::vector<const File *> Files;
  std::vector<uint32_t> NextRank;
  std::vector<std::pair<uint32_t, uint32_t>> Lines;
  std::vector<uint8_t> MaskedStrings;
  llvm::StringRef CompilationDirectory;
  bool Valid = true;

  // DILocation::getMergedLocation may unwrap a lexical block file when it
  // combines instructions. A line originating in a different file can then
  // select a different output scope after an otherwise monotone source edit.
  static bool hasReplayableFile(const llvm::DILexicalBlockBase &Scope) {
    const File *F = Scope.getFile();
    if (llvm::isa<llvm::DILexicalBlockFile>(Scope) && !F)
      return false;
    auto *Parent = Scope.getRawScope();
    for (unsigned Depth = 0; Depth <= 128; ++Depth) {
      if (llvm::isa_and_nonnull<llvm::DISubprogram>(Parent))
        return true;
      const auto *Block =
          llvm::dyn_cast_or_null<llvm::DILexicalBlockBase>(Parent);
      if (!Block)
        return false;
      if (llvm::isa<llvm::DILexicalBlockFile>(Block))
        return F && F == Block->getFile();
      Parent = Block->getRawScope();
    }
    return false;
  }

  void visit(const Metadata *MD, unsigned Depth = 0) {
    if (!MD || !Visited.insert(MD).second)
      return;
    if (Depth > 128) {
      Valid = false;
      return;
    }

    const auto *Node = llvm::dyn_cast<llvm::MDNode>(MD);
    if (!Node)
      return;
    if (const auto *Scope = llvm::dyn_cast<llvm::DILexicalBlockBase>(Node);
        Scope && !hasReplayableFile(*Scope)) {
      Valid = false;
      return;
    }
    const auto *F = llvm::dyn_cast<File>(Node);
    for (auto [Index, Operand] : llvm::enumerate(Node->operands())) {
      const auto *Str = llvm::dyn_cast_or_null<MDString>(Operand.get());
      if (!Str) {
        visit(Operand.get(), Depth + 1);
        continue;
      }
      // DIFile's third operand is its checksum value. The same uniqued string
      // may also occur in arbitrary program metadata or in another DIFile
      // field. Masking it there would omit a real input to code generation.
      if (F && Index == 2 && F->getRawChecksum() &&
          F->getRawChecksum()->Value == Str && !Str->getString().empty() &&
          llvm::all_of(Str->getString(), llvm::isHexDigit))
        Checksums.insert(Str);
      else
        OtherStrings.insert(Str);
    }
  }

public:
  bool prepare(const llvm::Module &M) {
    unsigned Units = 0;
    for (const auto *CU : M.debug_compile_units()) {
      if (++Units != 1 || !CU->getFile() ||
          CU->getEmissionKind() != llvm::DICompileUnit::LineTablesOnly)
        return false;
      CompilationDirectory = CU->getFile()->getDirectory();
    }
    if (Units != 1)
      return false;

    llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>> Attachments;
    auto VisitAttachments = [&](const auto &V) {
      if (!V.hasMetadata())
        return;
      Attachments.clear();
      V.getAllMetadata(Attachments);
      for (auto [Kind, Node] : Attachments)
        visit(Node);
    };
    for (const auto &N : M.named_metadata())
      for (const auto *MD : N.operands())
        visit(MD);
    for (const auto &G : M.globals())
      VisitAttachments(G);
    for (const auto &IFunc : M.ifuncs())
      VisitAttachments(IFunc);
    for (const auto &F : M) {
      VisitAttachments(F);
      for (const auto &BB : F) {
        if (const_cast<llvm::BasicBlock &>(BB).getTrailingDbgRecords())
          return false;
        for (const auto &I : BB) {
          if (I.hasDbgRecords() || llvm::isa<llvm::DbgInfoIntrinsic>(I))
            return false;
          VisitAttachments(I);
          for (const auto &Op : I.operands()) {
            auto *MV = llvm::dyn_cast_or_null<llvm::MetadataAsValue>(Op.get());
            if (!MV)
              continue;
            if (auto *S = llvm::dyn_cast<MDString>(MV->getMetadata()))
              OtherStrings.insert(S);
            else
              visit(MV->getMetadata());
          }
        }
      }
    }
    return Valid;
  }

  uint32_t file(const File *F) {
    if (!F || Files.size() >= UINT32_MAX) {
      Valid = false;
      return 0;
    }
    auto [It, Inserted] = FileIds.try_emplace(F, Files.size());
    if (Inserted) {
      Files.push_back(F);
      NextRank.push_back(1);
    }
    return It->second;
  }

  uint32_t line(const File *F, uint32_t Line) {
    uint32_t ID = file(F);
    if (!Valid || !Line)
      return 0;
    if (Lines.size() >= UINT32_MAX || NextRank[ID] == UINT32_MAX) {
      Valid = false;
      return 0;
    }
    auto [It, Inserted] =
        LineRanks.try_emplace((uint64_t(ID) << 32) | Line, NextRank[ID]);
    if (Inserted) {
      ++NextRank[ID];
      Lines.emplace_back(ID, Line);
    }
    return It->second;
  }

  bool mask(const MDString *Str) {
    bool Mask = Checksums.contains(Str) && !OtherStrings.contains(Str);
    MaskedStrings.push_back(Mask);
    return Mask;
  }

  bool write(llvm::StringRef Bitcode, llvm::StringRef ModuleIdentifier,
             llvm::raw_ostream &Out) const {
    if (!Valid)
      return false;
    llvm::BLAKE3 Hash;
    Hash.update(llvm::arrayRefFromStringRef("ohm-line-certificate-v3"));
    auto HashField = [&](llvm::ArrayRef<uint8_t> Bytes) {
      uint64_t Size = Bytes.size();
      std::array<uint8_t, 8> Length;
      for (unsigned I = 0; I < 8; ++I)
        Length[I] = Size >> (8 * I);
      Hash.update(Length);
      Hash.update(Bytes);
    };
    HashField(llvm::arrayRefFromStringRef(Bitcode));
    HashField(MaskedStrings);
    // Bitcode readers assign the identifier, and native emitters can use it.
    HashField(llvm::arrayRefFromStringRef(ModuleIdentifier));
    auto Digest = Hash.final();

    // The consumer caps a proof at 128 MiB before decoding it. Encode into a
    // private buffer so an oversized or unsupported input leaves Out untouched.
    llvm::SmallString<0> Bytes;
    llvm::raw_svector_ostream OS(Bytes);
    auto Word = [&](uint32_t N) {
      for (unsigned I = 0; I < 32; I += 8)
        OS << char(N >> I);
    };
    auto String = [&](llvm::StringRef S) {
      if (S.size() > UINT32_MAX || S.contains('\0'))
        return false;
      Word(S.size());
      OS << S;
      return true;
    };
    if (!String(CompilationDirectory))
      return false;
    OS.write(reinterpret_cast<const char *>(Digest.data()), Digest.size());
    Word(Files.size());
    for (const auto *F : Files) {
      llvm::SmallString<256> Path;
      if (!llvm::sys::path::is_absolute(F->getFilename()))
        Path = F->getDirectory();
      llvm::sys::path::append(Path, F->getFilename());
      auto Digest = F->getChecksum();
      if (!String(Path) || !String(Digest ? Digest->Value : llvm::StringRef()))
        return false;
    }
    Word(Lines.size());
    for (auto [F, N] : Lines) {
      Word(F);
      Word(N);
    }
    if (Bytes.size() > 128 * 1024 * 1024)
      return false;
    Out << Bytes;
    return true;
  }
};
} // namespace
#endif
