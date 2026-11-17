//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/OhmLineCertificate.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
using namespace llvm;
namespace {
using Name = std::pair<std::string, std::string>;
using Edge = std::array<uint32_t, 4>;
struct Recorder {
  bool Valid = true, Began = false, Finished = false;
  DenseMap<unsigned, const DIFile *> Numbers;
  DenseMap<const DIFile *, uint32_t> IDs;
  std::vector<Name> Names;
  std::set<Edge> Edges;
  uint32_t file(const DIFile *F) {
    auto Existing = IDs.find(F);
    if (Existing != IDs.end())
      return Existing->second;
    uint32_t I = Names.size();
    Names.emplace_back(F->getDirectory().str(), F->getFilename().str());
    IDs.try_emplace(F, I);
    if (I >= 65535 || Names.back().second.empty())
      Valid = false;
    return I;
  }
};
thread_local std::unique_ptr<Recorder> Active;
constexpr StringLiteral Magic("OHM-LINE-DECISIONS-V1");
void number(std::string &Out, uint32_t N) {
  for (unsigned I = 0; I != 4; ++I)
    Out.push_back(char(N >> (I * 8)));
}
bool number(StringRef &Data, uint32_t &N) {
  if (Data.size() < 4)
    return false;
  N = 0;
  for (unsigned I = 0; I != 4; ++I)
    N |= uint32_t(uint8_t(Data[I])) << (I * 8);
  Data = Data.drop_front(4);
  return true;
}
void text(std::string &Out, const std::string &Value) {
  number(Out, Value.size());
  Out += Value;
}
bool text(StringRef &Data, std::string &Value) {
  uint32_t N;
  if (!number(Data, N) || N > Data.size() || N > 1048576)
    return false;
  Value = Data.take_front(N).str();
  Data = Data.drop_front(N);
  return true;
}
std::string encode(const Recorder &R) {
  if (!R.Valid || !R.Began || !R.Finished)
    return {};
  auto Out = Magic.str();
  number(Out, R.Names.size());
  for (const auto &P : R.Names) {
    text(Out, P.first);
    text(Out, P.second);
  }
  number(Out, R.Edges.size());
  for (const auto &E : R.Edges)
    for (uint32_t P : E)
      number(Out, P);
  return Out;
}
std::string canonicalPath(StringRef Directory, StringRef Filename) {
  SmallString<128> Path(Filename);
  if (!sys::path::is_absolute(Path, sys::path::Style::posix)) {
    Path = Directory;
    sys::path::append(Path, sys::path::Style::posix, Filename);
  }
  sys::path::remove_dots(Path, true, sys::path::Style::posix);
  return Path.str().str();
}
struct Transfer {
  Name File;
  std::map<uint32_t, uint32_t> Lines;
  std::string Canonical;
};
} // namespace
extern "C" void ohm_line_certificate_begin_v1() {
  if (Active) {
    Active->Valid = false;
    return;
  }
  Active = std::make_unique<Recorder>();
}
extern "C" void ohm_line_record_module_v1(const void *Arg) {
  const auto *M = static_cast<const Module *>(Arg);
  if (!Active)
    return;
  if (Active->Began || Active->Finished)
    Active->Valid = false;
  Active->Began = true;
  auto *CUs = M->getNamedMetadata("llvm.dbg.cu");
  // A single compile unit gives an unambiguous DWARF file-number namespace
  // when LastAsmLine supplies the previous source location.
  if (!CUs || CUs->getNumOperands() != 1)
    Active->Valid = false;
}
extern "C" void ohm_line_record_end_module_v1() {
  if (!Active)
    return;
  if (!Active->Began || Active->Finished)
    Active->Valid = false;
  Active->Finished = true;
}
extern "C" void ohm_line_record_file_v1(unsigned Number, const void *Arg) {
  if (!Active || !Active->Valid || !Arg)
    return;
  const auto *F = static_cast<const DIFile *>(Arg);
  auto It = Active->Numbers.find(Number);
  if (It == Active->Numbers.end())
    Active->Numbers.try_emplace(Number, F);
  else if (It->second && It->second != F &&
           (It->second->getDirectory() != F->getDirectory() ||
            It->second->getFilename() != F->getFilename()))
    It->second = nullptr;
}
extern "C" void ohm_line_record_pair_v1(const void *ArgA, unsigned LA,
                                        const void *ArgB, unsigned LB,
                                        unsigned DwarfB) {
  if (!Active || !Active->Valid || !LA || !LB)
    return;
  const auto *A = static_cast<const DIFile *>(ArgA);
  const auto *B = static_cast<const DIFile *>(ArgB);
  if (!B && DwarfB)
    B = Active->Numbers.lookup(DwarfB);
  if (!A || !B) {
    Active->Valid = false;
    return;
  }
  if (A == B || (A->getDirectory() == B->getDirectory() &&
                 A->getFilename() == B->getFilename()))
    return;
  uint32_t IA = Active->file(A), IB = Active->file(B);
  Edge E = IA < IB ? Edge{IA, LA, IB, LB} : Edge{IB, LB, IA, LA};
  Active->Edges.insert(E);
  if (Active->Edges.size() > 2000000)
    Active->Valid = false;
}
extern "C" LLVMMemoryBufferRef ohm_line_certificate_end_v1() {
  if (!Active)
    return nullptr;
  auto R = std::move(Active);
  auto Out = encode(*R);
  if (Out.empty())
    return nullptr;
  return reinterpret_cast<LLVMMemoryBufferRef>(
      MemoryBuffer::getMemBufferCopy(Out).release());
}

extern "C" int ohm_line_certificate_check_v1(const void *Data, size_t Len,
                                             const char *JSON, size_t JSONLen) {
  if (!Data || !Len || !JSON)
    return 2;
  auto Parsed = json::parse(StringRef(JSON, JSONLen));
  if (!Parsed) {
    consumeError(Parsed.takeError());
    return 2;
  }
  auto *Object = Parsed->getAsObject();
  if (!Object || Object->getInteger("version") != 1)
    return 2;
  const auto *Files = Object->getArray("files");
  if (!Files || Files->empty())
    return 2;
  std::vector<Transfer> Moves;
  for (const auto &V : *Files) {
    const auto *Row = V.getAsObject();
    if (!Row || Row->size() != 3)
      return 2;
    auto D = Row->getString("directory"), F = Row->getString("filename");
    const auto *Lines = Row->getArray("lines");
    if (!D || !F || F->empty() || !Lines)
      return 2;
    Transfer T{{D->str(), F->str()}, {}, canonicalPath(*D, *F)};
    for (const auto &Old : Moves)
      if (T.Canonical == Old.Canonical)
        return 2;
    int64_t LastA = 0, LastB = 0;
    for (const auto &P : *Lines) {
      const auto *Entry = P.getAsArray();
      if (!Entry || Entry->size() != 2)
        return 2;
      auto A = (*Entry)[0].getAsInteger(), B = (*Entry)[1].getAsInteger();
      if (!A || !B || *A <= LastA || *B <= LastB || *A > UINT32_MAX ||
          *B > UINT32_MAX)
        return 2;
      LastA = *A;
      LastB = *B;
      T.Lines.emplace(*A, *B);
    }
    Moves.push_back(std::move(T));
  }
  StringRef Bytes(static_cast<const char *>(Data), Len);
  uint32_t FileCount;
  if (!Bytes.consume_front(Magic) || !number(Bytes, FileCount) ||
      FileCount > 65535)
    return 2;
  std::vector<const Transfer *> Index;
  for (uint32_t I = 0; I < FileCount; ++I) {
    Name N;
    if (!text(Bytes, N.first) || !text(Bytes, N.second) || N.second.empty())
      return 2;
    const Transfer *Found = nullptr;
    auto P = canonicalPath(N.first, N.second);
    for (const auto &T : Moves) {
      if (N == T.File)
        Found = &T;
      else if (P == T.Canonical)
        return 2;
    }
    Index.push_back(Found);
  }
  uint32_t Count;
  if (!number(Bytes, Count) || Count > 2000000 ||
      Bytes.size() != uint64_t(Count) * 16)
    return 2;
  auto move = [&](uint32_t F, uint32_t L) {
    const auto *T = Index[F];
    if (!T)
      return L;
    auto I = T->Lines.find(L);
    return I == T->Lines.end() ? L : I->second;
  };
  for (uint32_t I = 0; I < Count; ++I) {
    Edge E;
    for (auto &V : E)
      if (!number(Bytes, V))
        return 2;
    if (E[0] >= FileCount || E[2] >= FileCount || E[0] == E[2] || !E[1] ||
        !E[3])
      return 2;
    if ((E[1] == E[3]) != (move(E[0], E[1]) == move(E[2], E[3])))
      return 2;
  }
  return 0;
}
