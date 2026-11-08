//===- OhmDebugObjectSources.cpp - Verified compiler source snapshots -*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/OhmDebugObjectSources.h"

#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__linux__) && defined(SYS_renameat2)
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/DebugInfo/DWARF/OhmDwarfRebase.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA1.h"
#include "llvm/Support/SHA256.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <linux/fs.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

using namespace llvm;

namespace {
using Key = std::pair<std::string, std::string>;
struct File {
  Key ID;
  std::string Digest;
  std::string Blob;
  std::string Embedded;
  unsigned Kind = 0;
  char Category = 0;
};
using Files = std::map<Key, File>;
struct Hashes {
  std::string MD5, SHA1, SHA256;
};
struct Snapshot {
  std::string Blob;
  std::pair<dev_t, ino_t> Identity;
};
struct Alignment {
  std::vector<uint32_t> Last, Ambiguous;
  uint32_t NewSize = 0;
  Hashes OldHash, NewHash;
};
struct FD {
  int Value = -1;
  explicit FD(int V = -1) : Value(V) {}
  FD(const FD &) = delete;
  FD &operator=(const FD &) = delete;
  FD(FD &&Other) : Value(std::exchange(Other.Value, -1)) {}
  FD &operator=(FD &&Other) {
    std::swap(Value, Other.Value);
    return *this;
  }
  ~FD() {
    if (Value >= 0)
      close(Value);
  }
};
Error bad(const Twine &Reason) {
  return createStringError(inconvertibleErrorCode(), Reason);
}
thread_local std::string LastReason;
int miss(StringRef Why, const char **Reason) {
  if (Reason) {
    LastReason = Why.str();
    *Reason = LastReason.c_str();
  }
  return 2;
}
LLVMMemoryBufferRef buffer(StringRef Bytes) {
  return reinterpret_cast<LLVMMemoryBufferRef>(
      MemoryBuffer::getMemBufferCopy(Bytes).release());
}
std::string hash256(StringRef B) {
  return toHex(SHA256::hash(arrayRefFromStringRef(B)), true);
}
std::string hashMD5(StringRef B) {
  return toHex(MD5::hash(arrayRefFromStringRef(B)), true);
}
std::string hashSHA1(StringRef B) {
  return toHex(SHA1::hash(arrayRefFromStringRef(B)), true);
}
Hashes allHashes(StringRef B) { return {hashMD5(B), hashSHA1(B), hash256(B)}; }
bool validDigest(StringRef S, unsigned K) {
  size_t N = K == 1 ? 32 : K == 2 ? 40 : K == 3 ? 64 : 0;
  return N && S.size() == N && llvm::all_of(S, [](char C) {
           return isDigit(C) || (C >= 'a' && C <= 'f');
         });
}
bool cleanPath(StringRef S, bool Absolute) {
  if (S.empty() || S.starts_with("/") != Absolute || S.contains('\0') ||
      S.contains('\\') || S.contains('"') ||
      !llvm::all_of(S, [](unsigned char C) { return C >= 32 && C < 127; }))
    return false;
  if (S == "/")
    return Absolute;
  if (S.ends_with("/") || S.contains("//"))
    return false;
  SmallVector<StringRef, 16> P;
  S.split(P, '/', -1, false);
  return llvm::none_of(P, [](StringRef C) { return C == "." || C == ".."; });
}
StringRef under(StringRef Prefix, StringRef Absolute) {
  if (Prefix == "/")
    return Absolute.drop_front();
  if (!Absolute.consume_front(Prefix) || !Absolute.consume_front("/"))
    return {};
  return Absolute;
}
std::string joined(StringRef D, StringRef N) {
  if (N.starts_with("/"))
    return N.str();
  return (Twine(D) + (D == "/" ? "" : "/") + N).str();
}
bool sameStamp(const struct stat &A, const struct stat &B) {
  return A.st_dev == B.st_dev && A.st_ino == B.st_ino &&
         A.st_size == B.st_size && A.st_mtim.tv_sec == B.st_mtim.tv_sec &&
         A.st_mtim.tv_nsec == B.st_mtim.tv_nsec &&
         A.st_ctim.tv_sec == B.st_ctim.tv_sec &&
         A.st_ctim.tv_nsec == B.st_ctim.tv_nsec;
}
Expected<struct stat> regular(int F) {
  struct stat S;
  if (fstat(F, &S) || !S_ISREG(S.st_mode) || S.st_size < 0 ||
      uint64_t(S.st_size) > UINT32_MAX)
    return bad("expected a regular source or blob of representable size");
  return S;
}
Expected<std::string> readStable(int F, const struct stat &S) {
  std::string B(size_t(S.st_size), '\0');
  size_t Pos = 0;
  while (Pos < B.size()) {
    ssize_t N = read(F, B.data() + Pos, B.size() - Pos);
    if (N < 0 && errno == EINTR)
      continue;
    if (N <= 0)
      return bad("short or failed source read");
    Pos += size_t(N);
  }
  auto After = regular(F);
  if (!After)
    return After.takeError();
  if (!sameStamp(S, *After))
    return bad("source changed while being read");
  return B;
}
Expected<FD> rootedFile(int Root, StringRef Rel) {
  if (!cleanPath(Rel, false))
    return bad("invalid contained source path");
  int Dir = Root;
  FD Owned;
  while (Rel.contains('/')) {
    auto [Head, Tail] = Rel.split('/');
    FD Next(openat(Dir, Head.str().c_str(),
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (Next.Value < 0)
      return bad("unavailable or symlinked source directory");
    Owned = std::move(Next);
    Dir = Owned.Value;
    Rel = Tail;
  }
  FD F(openat(Dir, Rel.str().c_str(),
              O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (F.Value < 0)
    return bad("unavailable or symlinked contained source");
  return std::move(F);
}
struct Verified {
  struct stat Stamp;
  std::string Claim, Blob;
  struct stat BlobStamp;
};
struct Memo {
  std::mutex Lock;
  std::map<std::pair<std::string, std::string>, Verified> Sources;
  std::map<std::pair<std::string, std::string>,
           std::shared_ptr<const Alignment>>
      Alignments;
};
Memo &memo() {
  static Memo M;
  return M;
}

// LLVM's normal cache pruning recognizes the llvmcache- prefix. The
// manifest stores the digest itself so eviction never changes its identity.
static std::string blobName(StringRef Digest) {
  return (Twine("llvmcache-") + Digest).str();
}

Expected<struct stat> storeBlob(int Dir, StringRef Digest, StringRef Bytes) {
  const std::string Name = blobName(Digest);
  // Immutable publication. Competing writers can only install an absent name. A
  // reader can never see a partially written blob; an interrupted temp is
  // inert.
  auto Existing = [&]() -> Expected<struct stat> {
    FD F(openat(Dir, Name.c_str(),
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (F.Value < 0)
      return bad("existing source blob cannot be opened");
    auto S = regular(F.Value);
    if (!S)
      return S.takeError();
    auto Prior = readStable(F.Value, *S);
    if (!Prior)
      return Prior.takeError();
    if (*Prior != Bytes)
      return bad("source blob contents differ from their digest name");
    return *S;
  };
  struct stat S;
  if (fstatat(Dir, Name.c_str(), &S, AT_SYMLINK_NOFOLLOW) == 0)
    return Existing();
  if (errno != ENOENT)
    return bad("cannot inspect source blob");
  static std::atomic<uint64_t> Sequence{0};
  std::string Temp;
  FD F;
  do {
    // Standard LLVM pruning also collects unfinished Thin-* cache files.
    Temp = (Twine("Thin-src") + Twine(getpid()) + "-" +
            Twine(Sequence.fetch_add(1, std::memory_order_relaxed)))
               .str();
    F = FD(openat(Dir, Temp.c_str(),
                  O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_WRONLY, 0400));
  } while (F.Value < 0 && errno == EEXIST);
  if (F.Value < 0)
    return bad("cannot create source blob");
  auto Cleanup = scope_exit([&] { unlinkat(Dir, Temp.c_str(), 0); });
  size_t Pos = 0;
  while (Pos < Bytes.size()) {
    ssize_t N = write(F.Value, Bytes.data() + Pos, Bytes.size() - Pos);
    if (N < 0 && errno == EINTR)
      continue;
    if (N <= 0)
      return bad("short or failed source blob write");
    Pos += size_t(N);
  }
  if (close(std::exchange(F.Value, -1)))
    return bad("could not close source blob");
  // A no-replace rename performs one atomic namespace transition. Readers
  // also observe the final inode timestamp immediately under concurrent
  // capture.
  if (syscall(SYS_renameat2, Dir, Temp.c_str(), Dir, Name.c_str(),
              RENAME_NOREPLACE)) {
    if (errno == EEXIST)
      return Existing();
    return bad("could not publish immutable source blob");
  }
  Cleanup.release();
  if (fstatat(Dir, Name.c_str(), &S, AT_SYMLINK_NOFOLLOW) ||
      !S_ISREG(S.st_mode))
    return bad("published source blob unavailable");
  return S;
}
Expected<Snapshot> snapshot(int SourceDir, int BlobDir, StringRef MemoBank,
                            StringRef Rel, unsigned Kind, StringRef Claim,
                            const std::optional<StringRef> &Embedded) {
  auto F = rootedFile(SourceDir, Rel);
  if (!F)
    return F.takeError();
  auto S = regular(F->Value);
  if (!S)
    return S.takeError();
  auto MemoKey = std::make_pair(MemoBank.str(), Rel.str());
  std::string QualifiedClaim =
      (Twine(Kind) + ":" + Claim + ":" + (Embedded ? hash256(*Embedded) : ""))
          .str();
  {
    std::lock_guard<std::mutex> Guard(memo().Lock);
    auto It = memo().Sources.find(MemoKey);
    if (It != memo().Sources.end() && sameStamp(*S, It->second.Stamp) &&
        It->second.Claim == QualifiedClaim) {
      struct stat B;
      if (!fstatat(BlobDir, blobName(It->second.Blob).c_str(), &B,
                   AT_SYMLINK_NOFOLLOW) &&
          S_ISREG(B.st_mode) && sameStamp(B, It->second.BlobStamp))
        return Snapshot{It->second.Blob, {S->st_dev, S->st_ino}};
    }
  }
  auto B = readStable(F->Value, *S);
  if (!B)
    return B.takeError();
  std::string Strong = hash256(*B);
  std::string Actual = Kind == 1   ? hashMD5(*B)
                       : Kind == 2 ? hashSHA1(*B)
                                   : Strong;
  if (Actual != Claim || (Embedded && *Embedded != *B))
    return bad("workspace source bytes differ from their DIFile checksum or "
               "embedding");
  auto BS = storeBlob(BlobDir, Strong, *B);
  if (!BS)
    return BS.takeError();
  {
    std::lock_guard<std::mutex> Guard(memo().Lock);
    if (memo().Sources.size() >= 4096)
      memo().Sources.clear();
    memo().Sources[std::move(MemoKey)] = {*S, std::move(QualifiedClaim), Strong,
                                          *BS};
  }
  return Snapshot{std::move(Strong), {S->st_dev, S->st_ino}};
}

Expected<Files> gather(Module &M, StringRef Prefix, int SourceDir, int BlobDir,
                       StringRef Bank) {
  SmallPtrSet<const DIFile *, 8> UnitNames;
  for (const auto *U : M.debug_compile_units())
    UnitNames.insert(U->getFile());
  SmallPtrSet<const MDNode *, 32> Seen;
  SmallVector<const MDNode *, 64> Todo;
  auto add = [&](const Metadata *MD) {
    if (auto *N = dyn_cast_if_present<MDNode>(MD))
      if (Seen.insert(N).second)
        Todo.push_back(N);
  };
  for (const auto &N : M.named_metadata())
    for (const auto *V : N.operands())
      add(V);
  SmallVector<std::pair<unsigned, MDNode *>, 8> Attachments;
  auto attached = [&](const auto &V) {
    Attachments.clear();
    V.getAllMetadata(Attachments);
    for (const auto &A : Attachments)
      add(A.second);
  };
  for (const auto &G : M.globals())
    attached(G);
  for (const auto &G : M.ifuncs())
    attached(G);
  for (auto &F : M) {
    attached(F);
    for (auto &BB : F) {
      if (BB.getTrailingDbgRecords())
        return bad("unsupported trailing debug records");
      for (const auto &I : BB) {
        if (I.hasDbgRecords())
          return bad("unsupported variable debug records");
        attached(I);
        for (const auto &O : I.operands())
          if (const auto *V = dyn_cast<MetadataAsValue>(O.get()))
            add(V->getMetadata());
      }
    }
  }
  Files Result;
  std::set<std::string> ContainedPaths;
  std::set<std::pair<dev_t, ino_t>> ContainedInodes;
  while (!Todo.empty()) {
    const MDNode *N = Todo.pop_back_val();
    for (const MDOperand &O : N->operands())
      add(O.get());
    auto *DF = dyn_cast<DIFile>(N);
    if (!DF)
      continue;
    auto D = DF->getDirectory(), F = DF->getFilename();
    if (F.empty() || D.contains('\0') || F.contains('\0') ||
        (!F.starts_with("/") && !D.starts_with("/")) || !json::isUTF8(D) ||
        !json::isUTF8(F))
      return bad("unresolved DIFile identity");
    File Row;
    Row.ID = {D.str(), F.str()};
    if (auto C = DF->getChecksum()) {
      Row.Kind = unsigned(C->Kind);
      Row.Digest = C->Value.str();
      if (!validDigest(Row.Digest, Row.Kind))
        return bad("invalid DIFile checksum");
    }
    auto Embedded = DF->getSource();
    if (Embedded)
      Row.Embedded = hash256(*Embedded);
    std::string Absolute = joined(D, F);
    StringRef Rel = under(Prefix, Absolute);
    if (!Rel.empty()) {
      if (!cleanPath(Absolute, true) ||
          !ContainedPaths.insert(Rel.str()).second)
        return bad("ambiguous, traversing, or duplicate workspace DIFile");
      if (!Row.Kind) {
        // Rust adds one checksum-free virtual CU name: source.rs/@/module.
        // An ordinary workspace file without a front-end checksum cannot map.
        auto Tail = F.rsplit("/@/").second;
        if (!UnitNames.count(DF) || Embedded || !F.contains("/@/") ||
            Tail.empty() ||
            (Tail != M.getModuleIdentifier() && Tail != M.getSourceFileName()))
          return bad("workspace source lacks a checksum");
        Row.Category = 'V';
      } else {
        auto B = snapshot(SourceDir, BlobDir, Bank, Rel, Row.Kind, Row.Digest,
                          Embedded);
        if (!B)
          return bad(Twine(Rel) + ": " + toString(B.takeError()));
        if (!ContainedInodes.insert(B->Identity).second)
          return bad("multiple workspace file identities resolve to one inode");
        Row.Blob = std::move(B->Blob);
        Row.Category = 'W';
      }
    } else {
      // Preserve the exact opaque SDK/registry identity and checksum. Outside
      // sources are never opened or projected into the workspace.
      SmallString<256> Normal(Absolute);
      sys::path::remove_dots(Normal, true, sys::path::Style::posix);
      if (Absolute == Prefix || !under(Prefix, Normal).empty() || !Row.Kind)
        return bad("ambiguous or unchecksummed external source");
      if (Embedded) {
        std::string H = Row.Kind == 1   ? hashMD5(*Embedded)
                        : Row.Kind == 2 ? hashSHA1(*Embedded)
                                        : Row.Embedded;
        if (H != Row.Digest)
          return bad("embedded external source differs from its checksum");
      }
      Row.Category = 'E';
    }
    Key ID = Row.ID;
    if (!Result.emplace(std::move(ID), std::move(Row)).second)
      return bad("duplicate full DIFile identity");
  }
  if (Result.empty())
    return bad("module lacks source records");
  return Result;
}
std::string manifest(StringRef Module, StringRef Prefix, const Files &F) {
  json::Array Rows;
  for (const auto &P : F) {
    const auto &R = P.second;
    Rows.push_back(json::Array{R.ID.first, R.ID.second,
                               static_cast<int64_t>(R.Kind), R.Digest, R.Blob,
                               R.Embedded, std::string(1, R.Category)});
  }
  std::string Out;
  raw_string_ostream OS(Out);
  OS << json::Value(json::Array{1, Module, Prefix, std::move(Rows)}) << '\n';
  return Out;
}
Expected<Files> decode(StringRef Bytes, StringRef Module, StringRef Prefix) {
  auto J = json::parse(Bytes);
  if (!J)
    return J.takeError();
  auto *A = J->getAsArray();
  if (!A || A->size() != 4 || (*A)[0].getAsInteger() != 1 ||
      (*A)[1].getAsString() != Module || (*A)[2].getAsString() != Prefix)
    return bad("incompatible source manifest");
  auto *Rows = (*A)[3].getAsArray();
  if (!Rows || Rows->empty())
    return bad("empty source inventory");
  Files F;
  for (const auto &Row : *Rows) {
    auto *R = Row.getAsArray();
    if (!R || R->size() != 7)
      return bad("invalid source manifest record");
    auto D = (*R)[0].getAsString(), N = (*R)[1].getAsString(),
         C = (*R)[3].getAsString(), B = (*R)[4].getAsString(),
         Src = (*R)[5].getAsString(), Cat = (*R)[6].getAsString();
    auto K = (*R)[2].getAsInteger();
    if (!D || !N || !C || !B || !Src || !Cat || !K || *K < 0 || *K > 3 ||
        Cat->size() != 1 || (!Src->empty() && !validDigest(*Src, 3)))
      return bad("invalid source manifest fields");
    char Type = Cat->front();
    if (Type != 'W' && Type != 'E' && Type != 'V')
      return bad("invalid source class");
    if (Type == 'V' ? (*K != 0 || !C->empty()) : !validDigest(*C, *K))
      return bad("invalid declared source digest");
    if (Type == 'W' ? !validDigest(*B, 3) : !B->empty())
      return bad("invalid contained source blob");
    Key ID{D->str(), N->str()};
    if (!F.emplace(ID,
                   File{ID, C->str(), B->str(), Src->str(), unsigned(*K), Type})
             .second)
      return bad("duplicate file in source manifest");
  }
  return F;
}
Expected<std::string> blob(int Dir, StringRef Digest) {
  if (!validDigest(Digest, 3))
    return bad("invalid blob identity");
  FD F(openat(Dir, blobName(Digest).c_str(),
              O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  if (F.Value < 0)
    return bad("missing predecessor source blob");
  auto S = regular(F.Value);
  if (!S)
    return S.takeError();
  auto B = readStable(F.Value, *S);
  if (!B)
    return B.takeError();
  if (hash256(*B) != Digest)
    return bad("corrupt source blob");
  return B;
}
Expected<std::vector<StringRef>> lines(StringRef B) {
  if (B.empty() || !B.ends_with("\n") || B.contains('\0') || B.contains('\r') ||
      !json::isUTF8(B))
    return bad("requires complete, LF-terminated UTF-8 source bytes");
  std::vector<StringRef> L;
  while (!B.empty()) {
    auto [S, T] = B.split('\n');
    L.push_back(S);
    B = T;
  }
  if (L.size() >= UINT32_MAX)
    return bad("source line count exceeds representable coordinates");
  return L;
}
Expected<std::shared_ptr<const Alignment>> align(StringRef Old, StringRef New) {
  auto A = lines(Old);
  if (!A)
    return A.takeError();
  auto B = lines(New);
  if (!B)
    return B.takeError();
  if (A->size() >= B->size())
    return bad("changed source is not a strict insertion");
  auto R = std::make_shared<Alignment>();
  R->Last.resize(A->size());
  R->NewSize = B->size();
  size_t J = 0;
  for (size_t I = 0; I < A->size(); ++I) {
    while (J < B->size() && (*A)[I] != (*B)[J])
      ++J;
    if (J == B->size())
      return bad("old source contains a deleted, moved, or replaced line");
    R->Last[I] = uint32_t(++J);
  }
  // Tight bounds on every possible ordered, byte-identical insertion map.
  // If these disagree, a source-only diff cannot establish a unique location.
  J = B->size();
  for (size_t I = A->size(); I > 0; --I) {
    while (J && (*A)[I - 1] != (*B)[J - 1])
      --J;
    if (!J)
      return bad("source has no reverse embedding");
    if (R->Last[I - 1] != J)
      R->Ambiguous.push_back(uint32_t(I));
    R->Last[I - 1] = uint32_t(J--);
  }
  std::reverse(R->Ambiguous.begin(), R->Ambiguous.end());
  R->OldHash = allHashes(Old);
  R->NewHash = allHashes(New);
  return std::shared_ptr<const Alignment>(std::move(R));
}
Expected<std::shared_ptr<const Alignment>> proven(int Dir, const File &Old,
                                                  const File &New) {
  // An on-disk predecessor is verified even when the alignment itself was
  // memoized by this compiler process. Missing or changed blobs always miss.
  auto O = blob(Dir, Old.Blob);
  if (!O)
    return O.takeError();
  auto N = blob(Dir, New.Blob);
  if (!N)
    return N.takeError();
  auto Key = std::make_pair(Old.Blob, New.Blob);
  std::shared_ptr<const Alignment> R;
  {
    std::lock_guard<std::mutex> G(memo().Lock);
    auto I = memo().Alignments.find(Key);
    if (I != memo().Alignments.end())
      R = I->second;
  }
  if (!R) {
    auto A = align(*O, *N);
    if (!A)
      return A.takeError();
    R = std::move(*A);
    std::lock_guard<std::mutex> G(memo().Lock);
    if (memo().Alignments.size() >= 128)
      memo().Alignments.clear();
    R = memo().Alignments.emplace(Key, R).first->second;
  }
  auto Declared = [](const Hashes &H, unsigned K) -> const std::string & {
    return K == 1 ? H.MD5 : K == 2 ? H.SHA1 : H.SHA256;
  };
  if (Declared(R->OldHash, Old.Kind) != Old.Digest ||
      Declared(R->NewHash, New.Kind) != New.Digest ||
      (!Old.Embedded.empty() && Old.Embedded != R->OldHash.SHA256) ||
      (!New.Embedded.empty() && New.Embedded != R->NewHash.SHA256))
    return bad("source proof disagrees with the originating DIFile");
  return R;
}
using Changes = std::map<Key, std::shared_ptr<const Alignment>>;
std::string encodeMap(const Changes &C) {
  std::string Out;
  raw_string_ostream OS(Out);
  json::OStream J(OS);
  J.object([&] {
    J.attribute("version", 1);
    J.attributeArray("files", [&] {
      for (const auto &V : C)
        J.object([&] {
          J.attribute("directory", V.first.first);
          J.attribute("filename", V.first.second);
          J.attributeArray("lines", [&] {
            auto &A = *V.second;
            for (size_t I = 0; I < A.Last.size(); ++I)
              if (A.Last[I] != I + 1)
                J.array([&] {
                  J.value(int64_t(I + 1));
                  J.value(int64_t(A.Last[I]));
                });
          });
        });
    });
  });
  OS << '\n';
  return Out;
}
std::string proof(StringRef Map, const Changes &C) {
  json::Array Files;
  auto values = [](const Hashes &H) {
    return json::Object{{"md5", H.MD5}, {"sha1", H.SHA1}, {"sha256", H.SHA256}};
  };
  for (const auto &V : C) {
    const auto &A = *V.second;
    json::Array Segments, Ambiguity;
    for (size_t I = 0; I < A.Last.size();) {
      size_t J = I + 1;
      while (J < A.Last.size() &&
             uint64_t(A.Last[J]) == uint64_t(A.Last[I]) + J - I)
        ++J;
      Segments.push_back(json::Object{{"old_start", int64_t(I + 1)},
                                      {"new_start", int64_t(A.Last[I])},
                                      {"length", int64_t(J - I)}});
      I = J;
    }
    for (auto L : A.Ambiguous)
      Ambiguity.push_back(int64_t(L));
    Files.push_back(json::Object{
        {"directory", V.first.first},
        {"filename", V.first.second},
        {"ordered_byte_identical_old_lines", int64_t(A.Last.size())},
        {"new_lines", int64_t(A.NewSize)},
        {"segments", std::move(Segments)},
        {"old", values(A.OldHash)},
        {"new", values(A.NewHash)},
        {"ambiguous_old_lines_absent_from_cached_object",
         std::move(Ambiguity)}});
  }
  std::string Out;
  raw_string_ostream OS(Out);
  OS << json::Value(json::Object{{"version", 1},
                                 {"map_sha256", hash256(Map)},
                                 {"files", std::move(Files)}})
     << '\n';
  return Out;
}
Expected<Changes> compare(int Dir, const Files &Old, const Files &New,
                          StringRef Object) {
  if (Old.size() != New.size())
    return bad("source inventory changed");
  Changes C;
  ohm::SourceMap Observe;
  for (const auto &KV : New) {
    auto I = Old.find(KV.first);
    if (I == Old.end())
      return bad("source identity changed");
    const File &O = I->second, &N = KV.second;
    if (O.Category != N.Category || O.Kind != N.Kind)
      return bad("source class or checksum kind changed");
    if (O.Category != 'W') {
      if (O.Digest != N.Digest || O.Embedded != N.Embedded)
        return bad("unmapped external or virtual source changed");
      continue;
    }
    if (O.Blob == N.Blob) {
      if (O.Digest != N.Digest || O.Embedded != N.Embedded)
        return bad("inconsistent source metadata");
      continue;
    }
    if (O.Digest == N.Digest)
      return bad("distinct source bytes have the same embedded checksum");
    auto R = proven(Dir, O, N);
    if (!R)
      return R.takeError();
    if (!(*R)->Ambiguous.empty()) {
      auto &Affected = Observe[joined(N.ID.first, N.ID.second)];
      for (uint32_t L : (*R)->Ambiguous)
        Affected[L] = L == UINT32_MAX ? L - 1 : L + 1;
    }
    C.emplace(KV.first, std::move(*R));
  }
  if (C.empty())
    return bad("no mapped source change");
  if (!Observe.empty()) {
    // Changing only ambiguous lines is an occurrence query. A changed row,
    // declaration or call, or any unsupported encoding, declines the candidate.
    auto R = ohm::rebaseDwarf4(MemoryBufferRef(Object, "cached predecessor"),
                               Observe);
    if (!R)
      return R.takeError();
    if (R->LineRows || R->DeclLines || R->CallLines)
      return bad("ambiguous source line is referenced by the cached object");
  }
  return C;
}
} // namespace

extern "C" int
ohm_debug_sources_v1(LLVMModuleRef Ref, const char *Cwd, const char *Prefix,
                     const char *Blobs, const void *Prior, size_t PriorSize,
                     const void *Object, size_t ObjectSize,
                     LLVMMemoryBufferRef *Next, LLVMMemoryBufferRef *Map,
                     LLVMMemoryBufferRef *Proof, const char **Reason) {
  if (Next)
    *Next = nullptr;
  if (Map)
    *Map = nullptr;
  if (Proof)
    *Proof = nullptr;
  if (Reason)
    *Reason = nullptr;
  if (!Ref || !Cwd || !Prefix || !Blobs || !Next || !Map || !Proof ||
      Next == Map || Next == Proof || Map == Proof || Cwd[0] != '/' ||
      !cleanPath(Prefix, true))
    return miss("invalid source context", Reason);
  FD Root(open(Cwd, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  FD Bank(open(Blobs, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  struct stat S;
  if (Root.Value < 0 || Bank.Value < 0 || fstat(Bank.Value, &S) ||
      S.st_uid != geteuid() || (S.st_mode & 0022))
    return miss("unavailable workspace or private source-blob directory",
                Reason);
  Module &M = *reinterpret_cast<Module *>(Ref);
  if (M.getModuleIdentifier().empty() ||
      StringRef(M.getModuleIdentifier()).contains('\0') ||
      !json::isUTF8(M.getModuleIdentifier()))
    return miss("invalid machine-input identity", Reason);
  auto Current = gather(M, Prefix, Root.Value, Bank.Value, Blobs);
  if (!Current)
    return miss(toString(Current.takeError()), Reason);
  *Next = buffer(manifest(M.getModuleIdentifier(), Prefix, *Current));
  if (!Prior || !PriorSize || !Object || !ObjectSize)
    return miss("no complete predecessor", Reason);
  auto Old = decode(StringRef(static_cast<const char *>(Prior), PriorSize),
                    M.getModuleIdentifier(), Prefix);
  if (!Old)
    return miss(toString(Old.takeError()), Reason);
  auto C = compare(Bank.Value, *Old, *Current,
                   StringRef(static_cast<const char *>(Object), ObjectSize));
  if (!C)
    return miss(toString(C.takeError()), Reason);
  std::string Bytes = encodeMap(*C);
  *Map = buffer(Bytes);
  *Proof = buffer(proof(Bytes, *C));
  return 0;
}
extern "C" int ohm_debug_rebase_v1(const void *Map, size_t MapSize,
                                   const void *Object, size_t ObjectSize,
                                   LLVMMemoryBufferRef *Output,
                                   const char **Reason) {
  if (Output)
    *Output = nullptr;
  if (Reason)
    *Reason = nullptr;
  if (!Map || !MapSize || !Object || !ObjectSize || !Output)
    return miss("invalid object rebase inputs", Reason);
  auto M =
      ohm::parseSourceMap(StringRef(static_cast<const char *>(Map), MapSize));
  if (!M)
    return miss(toString(M.takeError()), Reason);
  auto O = ohm::rebaseDwarf4(
      MemoryBufferRef(StringRef(static_cast<const char *>(Object), ObjectSize),
                      "cached predecessor"),
      *M);
  if (!O)
    return miss(toString(O.takeError()), Reason);
  *Output = buffer(StringRef(O->Bytes.data(), O->Bytes.size()));
  return 0;
}

#else

// The source store uses Linux descriptor-relative opens and atomic no-replace
// publication. Other hosts continue through the ordinary backend.
extern "C" int
ohm_debug_sources_v1(LLVMModuleRef, const char *, const char *, const char *,
                     const void *, size_t, const void *, size_t,
                     LLVMMemoryBufferRef *Next, LLVMMemoryBufferRef *Map,
                     LLVMMemoryBufferRef *Proof, const char **Reason) {
  if (Next)
    *Next = nullptr;
  if (Map)
    *Map = nullptr;
  if (Proof)
    *Proof = nullptr;
  if (Reason)
    *Reason = "unsupported source snapshot host";
  return 2;
}
extern "C" int ohm_debug_rebase_v1(const void *, size_t, const void *, size_t,
                                   LLVMMemoryBufferRef *Output,
                                   const char **Reason) {
  if (Output)
    *Output = nullptr;
  if (Reason)
    *Reason = "unsupported source snapshot host";
  return 2;
}
#endif
