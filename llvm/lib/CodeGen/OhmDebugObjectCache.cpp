//===- OhmDebugObjectCache.cpp - Checked incremental machine objects ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OhmDebugObjectCache.h"
#include "llvm-c/Core.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Bitcode/OhmDebugObjectSignature.h"
#include "llvm/CodeGen/CodeGenTargetMachineImpl.h"
#include "llvm/CodeGen/OhmDebugObjectSources.h"
#include "llvm/CodeGen/OhmLineCertificate.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/BLAKE3.h"
#include "llvm/Support/CachePruning.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#if defined(__linux__) && defined(__x86_64__)
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#endif

using namespace llvm;

#if defined(__linux__) && defined(__x86_64__)
std::optional<ohm::DebugObjectImageGeneration>
llvm::ohm::getDebugObjectImageGeneration() {
  std::optional<DebugObjectImageGeneration> Generation;
  ::dl_iterate_phdr(
      [](dl_phdr_info *I, size_t Size, void *Out) {
        if (Size >= offsetof(dl_phdr_info, dlpi_subs) + sizeof(I->dlpi_subs))
          static_cast<decltype(Generation) *>(Out)->emplace(I->dlpi_adds,
                                                            I->dlpi_subs);
        return 1;
      },
      &Generation);
  return Generation;
}

SmallVector<ohm::DebugObjectImage> llvm::ohm::getDebugObjectImages() {
  SmallVector<DebugObjectImage> Images;
  ::dl_iterate_phdr(
      [](dl_phdr_info *I, size_t, void *Out) {
        // The kernel supplies the vDSO without a backing file.
        if (I->dlpi_addr && I->dlpi_addr == ::getauxval(AT_SYSINFO_EHDR))
          return 0;
        static_cast<decltype(Images) *>(Out)->push_back(
            {I->dlpi_name ? I->dlpi_name : "",
             reinterpret_cast<uintptr_t>(I->dlpi_phdr)});
        return 0;
      },
      &Images);
  return Images;
}
#endif

namespace {
#if defined(__linux__) && defined(__x86_64__)
constexpr StringLiteral Magic("OHMELFO2");
constexpr uint64_t MaxOrigin = 256ULL << 20;

struct Buffer {
  LLVMMemoryBufferRef Value = nullptr;
  Buffer() = default;
  Buffer(const Buffer &) = delete;
  Buffer &operator=(const Buffer &) = delete;
  ~Buffer() {
    if (Value)
      LLVMDisposeMemoryBuffer(Value);
  }
  StringRef bytes() const {
    return Value
               ? StringRef(LLVMGetBufferStart(Value), LLVMGetBufferSize(Value))
               : StringRef();
  }
};

struct Signature {
  char *Data = nullptr;
  size_t Size = 0;
  ~Signature() {
    if (Data)
      ohm_bc_free_v1(Data);
  }
  StringRef bytes() const { return Data ? StringRef(Data, Size) : StringRef(); }
};

std::string joinPath(StringRef A, StringRef B) {
  SmallString<256> Path(A);
  sys::path::append(Path, B);
  return Path.str().str();
}
std::string hexDigest(StringRef S) {
  return toHex(BLAKE3::hash(arrayRefFromStringRef(S)), true);
}
void frame(BLAKE3 &Hash, StringRef Field) {
  uint8_t N[8];
  support::endian::write64le(N, Field.size());
  Hash.update(ArrayRef(N));
  Hash.update(Field);
}

bool privateDirectory(StringRef Name) {
  if (::mkdir(Name.str().c_str(), 0700) < 0 && errno != EEXIST)
    return false;
  struct stat S;
  return ::lstat(Name.str().c_str(), &S) == 0 && S_ISDIR(S.st_mode) &&
         S.st_uid == ::geteuid() && (S.st_mode & 0077) == 0;
}
std::unique_ptr<MemoryBuffer> readPrivate(StringRef Path, uint64_t Limit) {
  int FD = ::open(Path.str().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (FD < 0)
    return nullptr;
  auto Close = scope_exit([&] { ::close(FD); });
  struct stat S;
  if (::fstat(FD, &S) || !S_ISREG(S.st_mode) || S.st_uid != ::geteuid() ||
      (S.st_mode & 0022) || S.st_size < 0 || uint64_t(S.st_size) > Limit)
    return nullptr;
  auto Data = MemoryBuffer::getOpenFile(FD, Path, uint64_t(S.st_size),
                                        /*RequiresNullTerminator=*/false,
                                        /*IsVolatile=*/true);
  return Data ? std::move(*Data) : nullptr;
}
bool writeAtomic(StringRef Path, ArrayRef<StringRef> Parts) {
  int FD;
  SmallString<256> Tmp;
  if (sys::fs::createUniqueFile(Twine(Path) + ".%%%%%%", FD, Tmp,
                                sys::fs::OF_None, 0600))
    return false;
  auto Remove = scope_exit([&] { sys::fs::remove(Tmp); });
  raw_fd_ostream Out(FD, /*shouldClose=*/true);
  for (StringRef Part : Parts)
    Out << Part;
  Out.close();
  if (Out.has_error()) {
    Out.clear_error();
    return false;
  }
  return !sys::fs::rename(Tmp, Path);
}
std::string stamp(const struct stat &S) {
  return (Twine(S.st_dev) + "." + Twine(S.st_ino) + "." + Twine(S.st_size) +
          "." + Twine(S.st_mtim.tv_sec) + "." + Twine(S.st_mtim.tv_nsec) + "." +
          Twine(S.st_ctim.tv_sec) + "." + Twine(S.st_ctim.tv_nsec))
      .str();
}

// An atomic toolchain update can replace a pathname after its old DSO has
// already been loaded. Match the open file to the mapped image before trusting
// a cached content digest for that pathname.
bool sameMappedFile(StringRef Maps, uintptr_t Address, const struct stat &S) {
  while (!Maps.empty()) {
    StringRef Row, Span, Permissions, Offset, Device, Inode;
    std::tie(Row, Maps) = Maps.split('\n');
    std::tie(Span, Row) = Row.split(' ');
    auto [Left, Right] = Span.split('-');
    uint64_t Start, End;
    if (Left.getAsInteger(16, Start) || Right.getAsInteger(16, End) ||
        Address < Start || Address >= End)
      continue;
    std::tie(Permissions, Row) = Row.ltrim().split(' ');
    std::tie(Offset, Row) = Row.ltrim().split(' ');
    std::tie(Device, Row) = Row.ltrim().split(' ');
    std::tie(Inode, Row) = Row.ltrim().split(' ');
    auto [A, B] = Device.split(':');
    uint64_t Major, Minor, Number;
    return !A.getAsInteger(16, Major) && !B.getAsInteger(16, Minor) &&
           !Inode.getAsInteger(10, Number) && Number && Number == S.st_ino &&
           Major == major(S.st_dev) && Minor == minor(S.st_dev);
  }
  return false;
}
std::optional<std::string>
componentIdentity(const ohm::DebugObjectImage &Component, StringRef Maps,
                  StringRef Cache) {
  StringRef Path = Component.Path;
  int FD = ::open(Component.Path.c_str(), O_RDONLY | O_CLOEXEC);
  if (FD < 0)
    return std::nullopt;
  auto Close = scope_exit([&] { ::close(FD); });
  struct stat Before, After;
  if (::fstat(FD, &Before) || !S_ISREG(Before.st_mode) ||
      !sameMappedFile(Maps, Component.Address, Before))
    return std::nullopt;
  std::string Stamp = stamp(Before);
  std::string Index = joinPath(Cache, hexDigest(Path));
  if (auto B = readPrivate(Index, 4096)) {
    auto [Old, H] = B->getBuffer().split('\n');
    if (Old == Stamp && H.size() == 64 &&
        llvm::all_of(H, [](char Ch) { return isHexDigit(Ch); }))
      return H.str();
  }
  BLAKE3 H;
  char Bytes[64 * 1024];
  for (;;) {
    ssize_t N = ::read(FD, Bytes, sizeof(Bytes));
    if (N < 0 && errno == EINTR)
      continue;
    if (N < 0)
      return std::nullopt;
    if (!N)
      break;
    H.update(StringRef(Bytes, size_t(N)));
  }
  if (::fstat(FD, &After) || stamp(After) != Stamp)
    return std::nullopt;
  std::string Digest = toHex(H.final(), true);
  writeAtomic(Index, {Stamp, "\n", Digest});
  return Digest;
}
bool contains(ArrayRef<StringRef> Choices, StringRef Value) {
  return llvm::is_contained(Choices, Value);
}
struct Invocation {
  std::string Root, Entries, Blobs, Cwd, Remap, Namespace;
  std::optional<ohm::DebugObjectImageGeneration> Generation;
  bool imagesUnchanged() const {
    return Generation && Generation == ohm::getDebugObjectImageGeneration();
  }
  Invocation() {
    if (std::getenv("OHM_DISABLE_DEBUG_OBJECT_CACHE"))
      return;
    auto Cmd = MemoryBuffer::getFileAsStream("/proc/self/cmdline");
    if (!Cmd)
      return;
    StringRef All = (*Cmd)->getBuffer();
    SmallVector<StringRef, 128> Args;
    All.split(Args, '\0', -1, false);
    if (Args.empty() || sys::path::filename(Args[0]) != "rustc" ||
        !sys::path::is_absolute(Args[0]))
      return;
    StringRef Incremental, Opt, LTO, Embed, Debug, Crate, Outputs, Source;
    bool Ohm = false;
    for (size_t I = 1; I < Args.size(); ++I) {
      StringRef A = Args[I], Key, V;
      if (A.starts_with("--")) {
        std::tie(Key, V) = A.split('=');
        if (!contains({"--crate-name", "--crate-type", "--edition", "--emit",
                       "--error-format", "--json", "--out-dir", "--extern",
                       "--check-cfg", "--cfg", "--allow", "--warn", "--deny",
                       "--forbid", "--cap-lints", "--remap-path-scope"},
                      Key))
          return;
        if (!A.contains('=')) {
          if (++I == Args.size())
            return;
          V = Args[I];
        }
        if (Key == "--crate-type") {
          if (!Crate.empty())
            return;
          Crate = V;
        }
        if (Key == "--emit") {
          if (!Outputs.empty())
            return;
          Outputs = V;
        }
      } else if (A.size() >= 2 &&
                 contains({"-C", "-Z", "-L", "-A", "-W", "-D", "-F"},
                          A.take_front(2))) {
        Key = A.take_front(2);
        V = A.drop_front(2);
        if (V.empty()) {
          if (++I == Args.size())
            return;
          V = Args[I];
        }
        auto [O, Value] = V.split('=');
        if (Key == "-C") {
          if (!contains({"opt-level", "lto", "embed-bitcode", "debuginfo",
                         "debug-assertions", "metadata", "extra-filename",
                         "incremental", "linker-features",
                         "link-self-contained", "link-arg", "codegen-units",
                         "overflow-checks", "target-cpu", "target-feature",
                         "relocation-model", "code-model", "panic"},
                        O))
            return;
          if (O == "opt-level")
            Opt = Value;
          else if (O == "lto")
            LTO = Value;
          else if (O == "embed-bitcode")
            Embed = Value;
          else if (O == "debuginfo")
            Debug = Value;
          else if (O == "incremental")
            Incremental = Value;
        } else if (Key == "-Z") {
          if (!contains({"ohm-shared-stages", "public-api-hash", "threads",
                         "ohm-caller-local-cgus", "ohm-stable-cgu-merging",
                         "ohm-incremental-mono-partitions", "remap-cwd-prefix",
                         "ohm-early-metadata", "checksum-hash-algorithm",
                         "ohm-check-relink-inputs"},
                        O))
            return;
          if (O == "remap-cwd-prefix")
            Remap = Value.str();
          if (O == "ohm-shared-stages")
            Ohm = true;
        }
      } else if (Source.empty() && A.ends_with(".rs"))
        Source = A;
      else
        return;
    }
    if (!Ohm || Source.empty() || !contains({"lib", "rlib"}, Crate) ||
        Outputs != "dep-info,metadata,link" ||
        !sys::path::is_absolute(Incremental) || Opt != "1" || LTO != "off" ||
        Embed != "no" || Debug != "line-tables-only" || Remap.empty())
      return;
    // An ELF extension can change target passes without changing rustc, its
    // driver, libLLVM, or the input IR. getenv() loses constructors' unsets.
    auto InitialEnvironment =
        MemoryBuffer::getFileAsStream("/proc/self/environ");
    if (!InitialEnvironment ||
        !ohm::allowsDebugObjectCacheLoader((*InitialEnvironment)->getBuffer()))
      return;
    SmallString<256> WD;
    if (sys::fs::current_path(WD))
      return;
    Cwd = WD.str().str();
    std::string Base = joinPath(Incremental, ".ohm-checked-objects-v1");
    if (!privateDirectory(Base))
      return;
    std::string IDs = joinPath(Base, "identity");
    if (!privateDirectory(IDs))
      return;
    auto Maps = MemoryBuffer::getFileAsStream("/proc/self/maps");
    if (!Maps)
      return;
    Generation = ohm::getDebugObjectImageGeneration();
    if (!Generation)
      return;
    auto Binaries = ohm::getDebugObjectImages();
    if (Binaries.empty() || !Binaries.front().Path.empty())
      return;
    Binaries.front().Path = Args[0].str();
    BLAKE3 Key;
    frame(Key, "ohm-debug-object-cache-images-v2");
    frame(Key, All);
    frame(Key, Cwd);
    unsigned Drivers = 0, LLVMs = 0;
    // A linked compiler extension can change machine code even when the IR and
    // the main compiler binaries are identical. ELF symbol resolution also
    // depends on the order in which the loader makes these images visible.
    for (const auto &C : Binaries) {
      StringRef Name = sys::path::filename(C.Path);
      Drivers += Name.starts_with("librustc_driver-");
      LLVMs += Name.starts_with("libLLVM.so.");
      auto ID = componentIdentity(C, (*Maps)->getBuffer(), IDs);
      if (!ID)
        return;
      frame(Key, C.Path);
      frame(Key, *ID);
    }
    if (Drivers != 1 || LLVMs != 1)
      return;
    Namespace = toHex(Key.final(), true);
    Entries = joinPath(Base, "objects");
    Blobs = joinPath(Base, "sources");
    if (!privateDirectory(Entries) || !privateDirectory(Blobs))
      return;
    // Source blobs can be pruned independently: a missing origin source is a
    // checked miss. The content keys also cover entries from older toolchains.
    CachePruningPolicy Policy;
    Policy.MaxSizeBytes = 4ULL << 30;
    pruneCache(Entries, Policy);
    Policy.MaxSizeBytes = 1ULL << 30;
    pruneCache(Blobs, Policy);
    Root = std::move(Base);
  }
};
Invocation &invocation() {
  static Invocation I;
  return I;
}

struct Origin {
  std::unique_ptr<MemoryBuffer> Data;
  StringRef Source, SignatureBytes, Object, Certificate;

  static std::optional<Origin> read(StringRef Path, StringRef Module,
                                    StringRef Target) {
    auto B = readPrivate(Path, MaxOrigin);
    if (!B)
      return std::nullopt;
    StringRef Record = B->getBuffer();
    if (!Record.consume_front(Magic) || Record.size() < 32)
      return std::nullopt;
    StringRef Stored = Record.take_front(32);
    Record = Record.drop_front(32);
    const auto Digest = BLAKE3::hash(arrayRefFromStringRef(Record));
    if (StringRef(reinterpret_cast<const char *>(Digest.data()),
                  Digest.size()) != Stored)
      return std::nullopt;
    SmallVector<StringRef, 6> Parts;
    for (unsigned I = 0; I < 6; ++I) {
      if (Record.size() < 8)
        return std::nullopt;
      uint64_t N = support::endian::read64le(Record.data());
      Record = Record.drop_front(8);
      if (N > Record.size())
        return std::nullopt;
      Parts.push_back(Record.take_front(N));
      Record = Record.drop_front(N);
      size_t Padding = (8 - N % 8) % 8;
      if (Record.size() < Padding ||
          !llvm::all_of(Record.take_front(Padding),
                        [](char C) { return C == 0; }))
        return std::nullopt;
      Record = Record.drop_front(Padding);
    }
    if (!Record.empty() || Parts[0] != Module || Parts[1] != Target ||
        Parts[2].empty() || Parts[3].empty() ||
        !Parts[4].starts_with("\x7f"
                              "ELF") ||
        Parts[5].empty())
      return std::nullopt;
    return Origin{std::move(B), Parts[2], Parts[3], Parts[4], Parts[5]};
  }

  static bool save(StringRef Path, ArrayRef<StringRef> Fields) {
    assert(Fields.size() == 6);
    size_t N = Magic.size() + 32;
    for (StringRef S : Fields) {
      size_t Padding = (8 - S.size() % 8) % 8;
      if (S.empty() || S.size() > MaxOrigin - 15 ||
          N > MaxOrigin - S.size() - 8 - Padding)
        return false;
      N += 8 + S.size() + Padding;
    }
    uint8_t Length[6][8];
    SmallVector<StringRef, 16> Parts{Magic, {}};
    BLAKE3 Hash;
    for (auto [I, S] : llvm::enumerate(Fields)) {
      support::endian::write64le(Length[I], S.size());
      Parts.emplace_back(reinterpret_cast<const char *>(Length[I]), 8);
      Parts.push_back(S);
      frame(Hash, S);
      static constexpr char Zero[8] = {};
      StringRef Padding(Zero, (8 - S.size() % 8) % 8);
      Parts.push_back(Padding);
      Hash.update(Padding);
    }
    auto H = Hash.final();
    Parts[1] = StringRef(reinterpret_cast<const char *>(H.data()), H.size());
    return writeAtomic(Path, Parts);
  }
};

bool ordinaryModule(const Module &M) {
  if (!M.getModuleInlineAsm().empty() || M.getModuleFlag("ProfileSummary") ||
      M.getModuleFlag("CSProfileSummary") ||
      M.getModuleFlag("Debug Info For Profiling"))
    return false;
  const auto *Units = M.getNamedMetadata("llvm.dbg.cu");
  if (!Units || Units->getNumOperands() != 1)
    return false;
  const auto *CU = dyn_cast<DICompileUnit>(Units->getOperand(0));
  if (!CU || CU->getEmissionKind() != DICompileUnit::LineTablesOnly ||
      CU->getDebugInfoForProfiling() || CU->getDWOId() ||
      !CU->getSplitDebugFilename().empty())
    return false;
  for (const NamedMDNode &N : M.named_metadata())
    if (N.getName().starts_with("llvm.pseudo_probe") ||
        N.getName().starts_with("llvm.coverage"))
      return false;
  for (const GlobalVariable &G : M.globals())
    if (G.getName().starts_with("__llvm_prf") ||
        G.getName().starts_with("__llvm_cov") ||
        G.getName().starts_with("__sancov"))
      return false;
  for (const Function &F : M) {
    StringRef N = F.getName();
    const auto *SP = F.getSubprogram();
    if (F.hasGC() || (SP && SP->getKeyInstructionsEnabled()) ||
        N.starts_with("llvm.instrprof") || N.starts_with("llvm.pseudoprobe") ||
        N.starts_with("llvm.experimental.gc.") ||
        N.starts_with("llvm.experimental.stackmap") ||
        N.starts_with("llvm.experimental.patchpoint") ||
        N.starts_with("__sanitizer") || N.starts_with("__sancov") ||
        N.starts_with("__llvm_profile"))
      return false;
    for (Attribute A : F.getAttributes().getFnAttrs()) {
      StringRef Name = A.isStringAttribute()
                           ? A.getKindAsString()
                           : Attribute::getNameFromAttrKind(A.getKindAsEnum());
      if (Name.starts_with("sanitize_") || Name.starts_with("xray-") ||
          Name.starts_with("instrument-function") ||
          Name == "function-instrument" || Name == "use-sample-profile" ||
          Name == "profile-sample-accurate" ||
          Name == "fs-sample-profile-use" || Name == "shadowcallstack")
        return false;
    }
    for (const BasicBlock &BB : F)
      for (const Instruction &Inst : BB) {
        if (Inst.hasDbgRecords())
          return false;
        if (const auto *CB = dyn_cast<CallBase>(&Inst); CB && CB->isInlineAsm())
          return false;
      }
  }
  return true;
}
void event(StringRef M, StringRef State, StringRef Reason = {}) {
  auto *P = std::getenv("LLVM_OHM_DEBUG_OBJECT_CACHE_TRACE");
  if (!P)
    return;
  int FD =
      ::open(P, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (FD < 0)
    return;
  auto S = (Twine(::getpid()) + "\t" + M + "\t" + State + "\t" + Reason + "\n")
               .str();
  (void)::write(FD, S.data(), S.size());
  ::close(FD);
}
#endif
} // namespace

bool llvm::ohm::canUseDebugObjectCache() {
#if defined(__linux__) && defined(__x86_64__)
  // At pipeline construction the Module is not available yet. This cheap
  // predicate avoids attaching an executor to other ELF compiler clients.
  static const bool Candidate = [] {
    const char *Exe = reinterpret_cast<const char *>(::getauxval(AT_EXECFN));
    return Exe && sys::path::filename(Exe) == "rustc" &&
           !std::getenv("OHM_DISABLE_DEBUG_OBJECT_CACHE");
  }();
  return Candidate;
#else
  return false;
#endif
}

bool llvm::ohm::runDebugObjectCache(Module &M, CodeGenTargetMachineImpl &TM,
                                    raw_svector_ostream &Out,
                                    function_ref<bool()> Emit) {
#if defined(__linux__) && defined(__x86_64__)
  // This precedes all cache filesystem activity, including toolchain identity
  // reads and the creation of source or object directories.
  if (!Out.str().empty() || !isLargeDebugObjectInput(M))
    return Emit();
  auto &I = invocation();
  if (I.Root.empty() || !I.imagesUnchanged() || !ordinaryModule(M))
    return Emit();
  std::string Target =
      TM.getTargetTriple().str() + "|" + TM.getTargetCPU().str() + "|" +
      TM.getTargetFeatureString().str() + "|" +
      std::to_string(unsigned(TM.getRelocationModel())) + "|" +
      std::to_string(unsigned(TM.getCodeModel())) + "|" + M.getDataLayoutStr();
  BLAKE3 Key;
  frame(Key, I.Namespace);
  frame(Key, M.getModuleIdentifier());
  std::string Path =
      joinPath(I.Entries, "llvmcache-" + toHex(Key.final(), true));
  Target = I.Namespace + "|" + Target;
  auto Prior = Origin::read(Path, M.getModuleIdentifier(), Target);
  auto Ref = reinterpret_cast<LLVMModuleRef>(&M);
  Buffer Sources, Map, Proof, Rebased, Certificate;
  Signature Next;
  const char *Why = nullptr;
  int Status = ohm_debug_sources_v1(
      Ref, I.Cwd.c_str(), I.Remap.c_str(), I.Blobs.c_str(),
      Prior ? Prior->Source.data() : nullptr, Prior ? Prior->Source.size() : 0,
      Prior ? Prior->Object.data() : nullptr, Prior ? Prior->Object.size() : 0,
      &Sources.Value, &Map.Value, &Proof.Value, &Why);
  if (Prior && Status == 0 && !Map.bytes().empty() && !Proof.bytes().empty() &&
      ohm_bc_match_v1(Ref, Prior->SignatureBytes.data(),
                      Prior->SignatureBytes.size(), Map.bytes().data(),
                      Map.bytes().size(), Proof.bytes().data(),
                      Proof.bytes().size()) == 1 &&
      ohm_line_certificate_check_v1(
          Prior->Certificate.data(), Prior->Certificate.size(),
          Map.bytes().data(), Map.bytes().size()) == 0 &&
      ohm_debug_rebase_v1(Map.bytes().data(), Map.bytes().size(),
                          Prior->Object.data(), Prior->Object.size(),
                          &Rebased.Value, &Why) == 0 &&
      Rebased.bytes().starts_with("\x7f"
                                  "ELF") && I.imagesUnchanged()) {
    Out << Rebased.bytes();
    event(M.getModuleIdentifier(), "hit");
    return false;
  }
  // An unsupported source or IR form always runs the ordinary pipeline.
  if (Sources.bytes().empty() || !ohm_bc_save_v1(Ref, &Next.Data, &Next.Size)) {
    event(M.getModuleIdentifier(), "unsupported", Why ? Why : "source or IR");
    return Emit();
  }
  ohm_line_certificate_begin_v1();
  bool Changed = Emit();
  Certificate.Value = ohm_line_certificate_end_v1();
  if (Out.str().starts_with("\x7f"
                            "ELF") &&
      !Certificate.bytes().empty() && I.imagesUnchanged() &&
      Origin::save(Path, {M.getModuleIdentifier(), Target, Sources.bytes(),
                          Next.bytes(), Out.str(), Certificate.bytes()}))
    event(M.getModuleIdentifier(), "committed", Why ? Why : "");
  else
    event(M.getModuleIdentifier(), "save-miss");
  return Changed;
#else
  return Emit();
#endif
}
