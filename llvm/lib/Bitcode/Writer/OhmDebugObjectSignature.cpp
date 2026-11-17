// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "llvm/Bitcode/OhmDebugObjectSignature.h"
#include "DebugObjectSourceMap.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"
#include <cstdlib>
#include <cstring>
#include <optional>
using namespace llvm;

namespace {
using Key = std::pair<std::string, std::string>;
std::optional<Key> fileKey(const json::Object &O) {
  auto Directory = O.getString("directory");
  auto Filename = O.getString("filename");
  if (!Directory || !Filename || Filename->empty())
    return std::nullopt;
  return Key{Directory->str(), Filename->str()};
}

std::optional<DebugObjectSourceMap> inverseMap(StringRef Input,
                                               StringRef Proof) {
  auto M = json::parse(Input);
  auto P = json::parse(Proof);
  if (!M || !P) {
    if (!M)
      consumeError(M.takeError());
    if (!P)
      consumeError(P.takeError());
    return std::nullopt;
  }
  const auto *Map = M->getAsObject();
  const auto *Record = P->getAsObject();
  if (!Map || !Record || Map->getInteger("version") != 1 ||
      Record->getInteger("version") != 1 || !Map->getArray("files") ||
      !Record->getArray("files"))
    return std::nullopt;
  auto Digest = Record->getString("map_sha256");
  if (!Digest ||
      *Digest != toHex(SHA256::hash(arrayRefFromStringRef(Input)), true))
    return std::nullopt;

  std::map<Key, const json::Object *> Origin;
  for (const auto &V : *Record->getArray("files")) {
    const auto *O = V.getAsObject();
    if (!O)
      return std::nullopt;
    auto K = fileKey(*O);
    if (!K || !Origin.emplace(*K, O).second)
      return std::nullopt;
  }
  DebugObjectSourceMap Result;
  for (const auto &Entry : *Map->getArray("files")) {
    const auto *O = Entry.getAsObject();
    if (!O || !O->getArray("lines"))
      return std::nullopt;
    auto K = fileKey(*O);
    if (!K)
      return std::nullopt;
    auto Verified = Origin.find(*K);
    if (Verified == Origin.end() || !Result.Files.try_emplace(*K).second)
      return std::nullopt;
    auto &F = Result.Files.at(*K);
    const auto &P = *Verified->second;
    auto OldCount = P.getInteger("ordered_byte_identical_old_lines");
    auto NewCount = P.getInteger("new_lines");
    if (!OldCount || !NewCount || *OldCount < 1 || *NewCount < *OldCount ||
        *NewCount > 16000000)
      return std::nullopt;
    std::vector<unsigned> Forward(*OldCount + 1);
    for (const auto &Line : *O->getArray("lines")) {
      auto *A = Line.getAsArray();
      if (!A || A->size() != 2)
        return std::nullopt;
      auto L = (*A)[0].getAsInteger(), R = (*A)[1].getAsInteger();
      if (!L || !R || *L < 1 || *L > *OldCount || *R < 1 || *R > *NewCount ||
          Forward[*L] != 0)
        return std::nullopt;
      Forward[*L] = *R;
    }
    F.Lines.resize(*NewCount + 1);
    unsigned Last = 0;
    for (unsigned Old = 1; Old < Forward.size(); ++Old) {
      unsigned New = Forward[Old] ? Forward[Old] : Old;
      if (New <= Last)
        return std::nullopt;
      Last = New;
      F.Lines[New] = Old;
    }
    const auto *OldSource = P.getObject("old");
    const auto *NewSource = P.getObject("new");
    if (!OldSource || !NewSource)
      return std::nullopt;
    for (const auto &[Name, Size] : {std::pair<StringRef, size_t>{"md5", 32},
                                     {"sha1", 40},
                                     {"sha256", 64}}) {
      auto Old = OldSource->getString(Name), New = NewSource->getString(Name);
      if (!Old || !New || Old->size() != Size || New->size() != Size ||
          !llvm::all_of(*Old, isHexDigit) || !llvm::all_of(*New, isHexDigit))
        return std::nullopt;
      F.Checksums[New->str()] = Old->str();
    }
  }
  if (Origin.size() != Result.Files.size())
    return std::nullopt;
  return Result;
}

} // namespace

extern "C" int ohm_bc_save_v1(LLVMModuleRef Ref, char **Data, size_t *Length) {
  if (!Data || !Length)
    return 0;
  *Data = nullptr;
  *Length = 0;
  if (!Ref)
    return 0;
  SmallVector<char, 0> Bytes;
  if (!writeDebugObjectBitcode(*unwrap(Ref), Bytes, nullptr))
    return 0;
  char *Storage = static_cast<char *>(std::malloc(Bytes.size()));
  if (!Storage)
    return 0;
  std::memcpy(Storage, Bytes.data(), Bytes.size());
  *Data = Storage;
  *Length = Bytes.size();
  return 1;
}

extern "C" int ohm_bc_match_v1(LLVMModuleRef Ref, const char *Origin,
                               size_t OriginLength, const char *Mapping,
                               size_t MappingLength, const char *Proof,
                               size_t ProofLength) {
  if (!Ref || !Origin || !OriginLength || !Mapping || !Proof)
    return 0;
  auto Map = inverseMap({Mapping, MappingLength}, {Proof, ProofLength});
  if (!Map)
    return 0;
  SmallVector<char, 0> Bytes;
  return writeDebugObjectBitcode(*unwrap(Ref), Bytes, &*Map) &&
         StringRef(Origin, OriginLength) ==
             StringRef(Bytes.data(), Bytes.size());
}
extern "C" void ohm_bc_free_v1(void *P) { std::free(P); }

void DebugObjectSourceMap::prepare() {
  Resolved.clear();
  Strings.clear();
  for (const auto &Named : Files) {
    for (const auto &Checksum : Named.second.Checksums) {
      if (Checksum.first == Checksum.second)
        continue;
      auto [I, Added] = Strings.try_emplace(Checksum.first, Checksum.second);
      if (!Added && I->second != Checksum.second)
        Valid = false;
    }
  }
}

const DebugObjectFile *DebugObjectSourceMap::lookup(const DIFile *F) {
  if (!F)
    return nullptr;
  auto [Entry, Added] = Resolved.try_emplace(F, nullptr);
  if (Added) {
    auto I = Files.find({F->getDirectory().str(), F->getFilename().str()});
    if (I != Files.end())
      Entry->second = &I->second;
  }
  return Entry->second;
}

void DebugObjectSourceMap::checkFileChecksum(const DIFile *F) {
  const DebugObjectFile *Mapped = lookup(F);
  auto Checksum = F->getRawChecksum();
  if (Mapped && Checksum) {
    auto S = Mapped->Checksums.find(Checksum->Value->getString().str());
    if (S == Mapped->Checksums.end())
      Valid = false;
  } else if (Checksum) {
    // An independent DIFile can share this interned checksum MDString.
    checkReference(Checksum->Value);
  }
}

unsigned DebugObjectSourceMap::line(const Metadata *N, unsigned L) {
  if (!L)
    return L;
  const DIFile *F = nullptr;
  if (auto *D = dyn_cast<DILocation>(N))
    F = D->getScope()->getFile();
  else if (auto *D = dyn_cast<DIScope>(N))
    F = D->getFile();
  else if (auto *D = dyn_cast<DIVariable>(N))
    F = D->getFile();
  else if (auto *D = dyn_cast<DILabel>(N))
    F = D->getFile();
  else if (auto *D = dyn_cast<DIImportedEntity>(N))
    F = D->getFile();
  else if (auto *D = dyn_cast<DIObjCProperty>(N))
    F = D->getFile();
  else {
    Valid = false;
    return L;
  }
  const DebugObjectFile *Mapped = lookup(F);
  if (!Mapped)
    return L;
  if (L >= Mapped->Lines.size() || Mapped->Lines[L] == 0) {
    Valid = false;
    return L;
  }
  return Mapped->Lines[L];
}

StringRef DebugObjectSourceMap::string(const MDString *S) const {
  auto I = Strings.find(S->getString());
  return I == Strings.end() ? S->getString() : StringRef(I->second);
}
