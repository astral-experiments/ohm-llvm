//===- OhmDebugObjectCache.cpp - Checked incremental machine objects ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OhmDebugObjectCache.h"
#include "OhmLeafObject.h"
#include "OhmTraitImports.h"
#include "llvm-c/Core.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitcode/OhmDebugObjectSignature.h"
#include "llvm/CodeGen/CodeGenTargetMachineImpl.h"
#include "llvm/CodeGen/OhmDebugObjectSources.h"
#include "llvm/CodeGen/OhmEarlyObjectCache.h"
#include "llvm/CodeGen/OhmLineCertificate.h"
#include "llvm/DebugInfo/DWARF/OhmRuntimeLocations.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DiagnosticObservation.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/OhmOptimizerState.h"
#include "llvm/Support/BLAKE3.h"
#include "llvm/Support/CachePruning.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
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
  int FD = ::open(Path.str().c_str(),
                  O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
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
bool writeAtomic(StringRef Path, ArrayRef<StringRef> Parts,
                 unsigned Mode = 0600) {
  int FD;
  SmallString<256> Tmp;
  if (sys::fs::createUniqueFile(Twine(Path) + ".%%%%%%", FD, Tmp,
                                sys::fs::OF_None, Mode))
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
  int FD = ::open(Component.Path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
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
  bool WithoutDebug = false;
  bool TestHarness = false;
  std::optional<ohm::DebugObjectImageGeneration> Generation;
  bool imagesUnchanged() const {
    return Generation && Generation == ohm::getDebugObjectImageGeneration();
  }
  explicit Invocation(bool ForLineObjects = false) {
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
    bool Strip = false;
    bool CheckRelinkInputs = false;
    bool EmitRelink = false;
    StringRef TempsDir;
    for (size_t I = 1; I < Args.size(); ++I) {
      StringRef A = Args[I], Key, V;
      // Cargo requests this frontend lint when compiling workspace crates.
      // The whole command line still contributes to the object namespace.
      if (ForLineObjects && A == "--test") {
        TestHarness = true;
        continue;
      }
      if (ForLineObjects && A == "--force-warn=unused_crate_dependencies")
        continue;
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
                         "strip", "debug-assertions", "metadata",
                         "extra-filename", "incremental", "linker-features",
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
          else if (O == "strip") {
            if (!contains({"none", "debuginfo", "symbols"}, Value))
              return;
            Strip = true;
          } else if (O == "incremental")
            Incremental = Value;
        } else if (Key == "-Z") {
          if (O == "emit-rlink" && Value.empty()) {
            EmitRelink = true;
            continue;
          }
          if (O == "temps-dir" && sys::path::is_absolute(Value)) {
            TempsDir = Value;
            continue;
          }
          if (!contains({"ohm-shared-stages", "public-api-hash", "threads",
                         "ohm-caller-local-cgus", "ohm-stable-cgu-merging",
                         "ohm-incremental-mono-partitions", "remap-cwd-prefix",
                         "ohm-early-metadata", "checksum-hash-algorithm",
                         "ohm-check-relink-inputs"},
                        O))
            return;
          if (O == "remap-cwd-prefix")
            Remap = Value.str();
          if (O == "ohm-check-relink-inputs" && Value.empty())
            CheckRelinkInputs = true;
          if (O == "ohm-shared-stages")
            Ohm = true;
        }
      } else if (Source.empty() && A.ends_with(".rs"))
        Source = A;
      else
        return;
    }
    WithoutDebug = Debug.empty() || Debug == "0" || Debug == "none";
    // Cargo builds the test crate as an executable, emitting its objects
    // for a separate relink step. This mode has no rlib metadata output.
    if (TestHarness) {
      if (!CheckRelinkInputs || !EmitRelink || TempsDir.empty() ||
          !Crate.empty() || Outputs != "dep-info,link" || WithoutDebug)
        return;
    } else if (!Ohm || EmitRelink || !TempsDir.empty() ||
               !contains({"lib", "rlib"}, Crate) ||
               Outputs != "dep-info,metadata,link") {
      return;
    }
    if (Source.empty() || !sys::path::is_absolute(Incremental) || Opt != "1" ||
        LTO != "off" || Embed != "no" ||
        (!WithoutDebug && (Debug != "line-tables-only" || Strip)) ||
        Remap.empty())
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
    if (!privateDirectory(Entries) ||
        (!WithoutDebug && !privateDirectory(Blobs)))
      return;
    // Source blobs can be pruned independently: a missing origin source is a
    // checked miss. The content keys also cover entries from older toolchains.
    // Cache access can continue if pruning cannot query the available space.
    CachePruningPolicy Policy;
    Policy.MaxSizeBytes = 4ULL << 30;
    consumeError(pruneCache(Entries, Policy).takeError());
    if (!WithoutDebug) {
      Policy.MaxSizeBytes = 1ULL << 30;
      consumeError(pruneCache(Blobs, Policy).takeError());
    }
    Root = std::move(Base);
  }
};
Invocation &invocation(bool ForLineObjects = false) {
  if (ForLineObjects) {
    static Invocation WithLines(/*ForLineObjects=*/true);
    return WithLines;
  }
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

bool ordinaryModule(const Module &M, bool WithoutDebug) {
  if (!M.getModuleInlineAsm().empty() || M.getModuleFlag("ProfileSummary") ||
      M.getModuleFlag("CSProfileSummary") ||
      M.getModuleFlag("Debug Info For Profiling"))
    return false;
  const auto *Units = M.getNamedMetadata("llvm.dbg.cu");
  if (WithoutDebug) {
    if (Units)
      return false;
  } else {
    if (!Units || Units->getNumOperands() != 1)
      return false;
    const auto *CU = dyn_cast<DICompileUnit>(Units->getOperand(0));
    if (!CU || CU->getEmissionKind() != DICompileUnit::LineTablesOnly ||
        CU->getDebugInfoForProfiling() || CU->getDWOId() ||
        !CU->getSplitDebugFilename().empty())
      return false;
  }
  for (const NamedMDNode &N : M.named_metadata())
    if (N.getName().starts_with("llvm.pseudo_probe") ||
        N.getName().starts_with("llvm.coverage"))
      return false;
  for (const GlobalVariable &G : M.globals())
    if ((WithoutDebug && G.getMetadata(LLVMContext::MD_dbg)) ||
        G.getName().starts_with("__llvm_prf") ||
        G.getName().starts_with("__llvm_cov") ||
        G.getName().starts_with("__sancov"))
      return false;
  for (const Function &F : M) {
    StringRef N = F.getName();
    const auto *SP = F.getSubprogram();
    if (F.hasGC() ||
        (SP && (WithoutDebug || SP->getKeyInstructionsEnabled())) ||
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
        if (Inst.hasDbgRecords() || (WithoutDebug && Inst.getDebugLoc()))
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
// For a module without debug information, an exact machine input needs no
// source snapshots or changes to its object. Bind its output to the full
// input, target and toolchain, including the use-list order in the bitcode.
std::string exactObjectKey(Module &M, const Invocation &I, StringRef Target) {
  Signature Input;
  if (!ohm_bc_save_v1(reinterpret_cast<LLVMModuleRef>(&M), &Input.Data,
                      &Input.Size))
    return {};
  BLAKE3 H;
  frame(H, "ohm-exact-machine-input-v1");
  frame(H, I.Namespace);
  frame(H, Target);
  frame(H, M.getModuleIdentifier());
  frame(H, Input.bytes());
  return toHex(H.final(), true);
}

constexpr StringLiteral ExactMagic("OHMOBJ01");

bool saveCheckedObject(StringRef Path, StringRef Key, StringRef Object,
                       StringRef Header = ExactMagic) {
  if (Object.size() > MaxOrigin - Header.size() - 64 ||
      !Object.starts_with("\x7f"
                          "ELF"))
    return false;
  BLAKE3 H;
  frame(H, Key);
  frame(H, Object);
  auto Sum = toHex(H.final(), true);
  return writeAtomic(Path, {Header, Sum, Object});
}
StringRef exactObjectPayload(StringRef Record, StringRef Key,
                             StringRef Header = ExactMagic) {
  if (!Record.consume_front(Header) || Record.size() < 64)
    return {};
  auto Stored = Record.take_front(64);
  auto Payload = Record.drop_front(64);
  BLAKE3 H;
  frame(H, Key);
  frame(H, Payload);
  if (!Payload.starts_with("\x7f"
                           "ELF") ||
      Stored != toHex(H.final(), true))
    return {};
  return Payload;
}
std::string machineTarget(const Module &M, const TargetMachine &TM) {
  return TM.getTargetTriple().str() + "|" + TM.getTargetCPU().str() + "|" +
         TM.getTargetFeatureString().str() + "|" +
         std::to_string(unsigned(TM.getRelocationModel())) + "|" +
         std::to_string(unsigned(TM.getCodeModel())) + "|" +
         M.getDataLayoutStr();
}
struct TraitOrigin {
  const TargetMachine *TM;
  SmallVector<char, 0> Manifest;
  std::string Key;
  std::string Object;
};
struct LeafOrigin {
  const TargetMachine *Target;
  std::string Proof;
  std::string Object;
};
struct EarlyOrigin {
  std::unique_ptr<TraitOrigin> Trait;
  Module *Input;
  std::string Key;
  std::string MachineKey;
  DiagnosticObservation Diagnostics;
  std::optional<LeafOrigin> Leaf;
  EarlyOrigin(Module &M, std::string K)
      : Input(&M), Key(std::move(K)), Diagnostics(M.getContext()) {}
};
thread_local std::weak_ptr<EarlyOrigin> PendingEarly;
constexpr StringLiteral TraitMagic("OHMTRAI2");

// The checked payload contains the original ELF, followed by the line
// certificate and import manifest. Only deliver the ELF after rebinding.
bool splitTraitRecord(StringRef Payload, StringRef &Object,
                      StringRef &Imports) {
  if (Payload.size() < 8)
    return false;
  uint64_t Size = support::endian::read64le(Payload.take_back(8).data());
  Payload = Payload.drop_back(8);
  if (!Size || Size >= Payload.size())
    return false;
  Imports = Payload.take_back(Size);
  Object = Payload.drop_back(Size);
  return true;
}

// A quick way to recognize a unit from the common ancestor when switching
// branches. This only decides when to serialize the full LLVM input. A
// collision can cost a probe, but cannot authorize a hit.
std::string inputShape(const Module &M) {
  BLAKE3 Hash;
  for (const Function &F : M) {
    frame(Hash, F.getName());
    char Blocks[8];
    support::endian::write64le(Blocks, F.size());
    Hash.update(StringRef(Blocks, sizeof(Blocks)));
    auto *SP = F.getSubprogram();
    auto *File = SP ? SP->getFile() : nullptr;
    if (File && File->getChecksum())
      frame(Hash, File->getChecksum()->Value);
  }
  for (const GlobalVariable &G : M.globals())
    frame(Hash, G.getName());
  return toHex(Hash.final(), true);
}

bool tryTraitObject(Module &M, TargetMachine &TM, const Invocation &I,
                    StringRef Output, void **Ticket) {
  if (TM.getOptLevel() != CodeGenOptLevel::Less)
    return false;
  auto Trait = std::make_unique<TraitOrigin>();
  Trait->TM = &TM;
  SmallVector<char, 0> Input;
  BLAKE3 H;
  frame(H, TraitMagic);
  frame(H, I.Namespace);
  frame(H, machineTarget(M, TM));
  frame(H, M.getModuleIdentifier());
  // Keep the first shape for this unit until pruning, and the most recent
  // import roster for trait rebinding. A ticket suppresses rustc's optimized
  // DWARF lookup, so defer to it if neither hint suggests a donor.
  auto ShouldProbe = [&](StringRef Imports) {
    BLAKE3 Slot;
    frame(Slot, "ohm-line-object-hint-v2");
    frame(Slot, I.Namespace);
    frame(Slot, M.getModuleIdentifier());
    auto Path = joinPath(I.Entries, "llvmcache-" + toHex(Slot.final(), true));
    std::string Shape = inputShape(M);
    std::string Roster = hexDigest(Imports);
    StringRef First = Shape;
    size_t Size = Shape.size() + Roster.size();
    auto Hint = readPrivate(Path, Size);
    if (Hint && Hint->getBufferSize() == Size) {
      First = Hint->getBuffer().take_front(Shape.size());
      if (Hint->getBuffer().drop_front(Shape.size()) == Roster)
        return First == Shape;
    }
    writeAtomic(Path, {First, Roster});
    return true;
  };
  if (!ohm::writeTraitImportSignature(M, Input, Trait->Manifest, ShouldProbe) ||
      Trait->Manifest.size() >= MaxOrigin)
    return false;
  frame(H, StringRef(Input.data(), Input.size()));
  auto Key = toHex(H.final(), true);
  StringRef Current(Trait->Manifest.data(), Trait->Manifest.size());
  auto Saved = readPrivate(joinPath(I.Entries, "llvmcache-" + Key), MaxOrigin);
  StringRef ObjectBytes, Manifest;
  if (Saved &&
      splitTraitRecord(exactObjectPayload(Saved->getBuffer(), Key, TraitMagic),
                       ObjectBytes, Manifest)) {
    auto Install = [&](StringRef Result, StringRef State) {
      if (!I.imagesUnchanged() || !writeAtomic(Output, {Result}, 0666))
        return false;
      event(M.getModuleIdentifier(), State);
      return true;
    };
    if (Current == Manifest)
      return Install(ObjectBytes, "exact-line-hit");
    SmallVector<char, 0> Object;
    if (ohm::rebindTraitImports(ObjectBytes, Manifest, Current, Object))
      return Install(StringRef(Object.data(), Object.size()), "trait-import-hit");
  }
  Trait->Key = std::move(Key);
  auto Origin = std::make_shared<EarlyOrigin>(M, std::string());
  Origin->Trait = std::move(Trait);
  PendingEarly = Origin;
  *Ticket = new std::shared_ptr<EarlyOrigin>(std::move(Origin));
  return false;
}

// Check the binding to this exact object before adjusting any leaf.
std::string loadLeafObject(StringRef Path, StringRef Proof) {
  auto Buffer = readPrivate(Path, MaxOrigin);
  if (!Buffer)
    return {};
  StringRef Record = Buffer->getBuffer();
  if (!Record.consume_front("OHMLEAF1") || Record.size() < 8)
    return {};
  uint64_t N = support::endian::read64le(Record.data());
  Record = Record.drop_front(8);
  if (!N || N >= Record.size())
    return {};
  return ohm_leaf::tryReuse(Proof, Record.take_front(N), Record.drop_front(N));
}

void startLeafObject(Module &M, TargetMachine &TM, const Invocation &I,
                     void **Ticket) {
  BLAKE3 H;
  frame(H, "ohm-leaf-object-v2");
  frame(H, I.Namespace);
  frame(H, machineTarget(M, TM));
  frame(H, M.getModuleIdentifier());
  auto Key = toHex(H.final(), true);
  if (*Ticket) {
    // Both caches can enroll the object emitted for this module. Share
    // its diagnostic observation and retain their independent keys.
    auto &Origin = *static_cast<std::shared_ptr<EarlyOrigin> *>(*Ticket);
    Origin->Key = std::move(Key);
    Origin->Leaf = LeafOrigin{&TM, {}, {}};
    return;
  }
  auto Origin = std::make_shared<EarlyOrigin>(M, std::move(Key));
  Origin->Leaf = LeafOrigin{&TM, {}, {}};
  PendingEarly = Origin;
  *Ticket = new std::shared_ptr<EarlyOrigin>(std::move(Origin));
}

void storeLeafObject(StringRef Path, const EarlyOrigin &Entry) {
  const auto &L = *Entry.Leaf;
  if (L.Object.empty())
    return;
  auto Witness = ohm_leaf::enroll(L.Proof, L.Object);
  if (Witness.empty() || Witness.size() + L.Object.size() + 16 > MaxOrigin)
    return;
  char Size[8];
  support::endian::write64le(Size, Witness.size());
  if (writeAtomic(Path, {"OHMLEAF1", StringRef(Size, sizeof(Size)), Witness,
                         L.Object}))
    event(Entry.Input->getModuleIdentifier(), "leaf-committed");
}

void observedMachineObject(Module &M, StringRef Key) {
  if (auto Origin = PendingEarly.lock(); Origin && Origin->Input == &M)
    Origin->MachineKey = Key.str();
}
std::string earlyIndexChecksum(StringRef Early, StringRef Machine) {
  BLAKE3 H;
  frame(H, "ohm-verified-optimizer-origin-v1");
  frame(H, Early);
  frame(H, Machine);
  return toHex(H.final(), true);
}
bool runExactObjectCache(Module &M, const Invocation &I, StringRef Target,
                         raw_svector_ostream &Out, function_ref<bool()> Emit) {
  LLVMContext &Ctx = M.getContext();
  const auto *DH = Ctx.getDiagHandlerPtr();
  if (!DH || DH->HasErrors || DH->isAnyRemarkEnabled() ||
      Ctx.getMainRemarkStreamer() || Ctx.getLLVMRemarkStreamer()) {
    event(M.getModuleIdentifier(), "bypass", "explicit compiler diagnostics");
    return Emit();
  }
  std::string Key = exactObjectKey(M, I, Target);
  if (Key.empty())
    return Emit();
  std::string Path = joinPath(I.Entries, "llvmcache-" + Key);
  if (auto Saved = readPrivate(Path, MaxOrigin)) {
    StringRef Object = exactObjectPayload(Saved->getBuffer(), Key);
    if (!Object.empty() && I.imagesUnchanged()) {
      Out << Object;
      observedMachineObject(M, Key);
      event(M.getModuleIdentifier(), "hit", "exact machine input");
      return false;
    }
  }

  DiagnosticObservation Diagnostics(Ctx);
  bool Changed = Emit();
  StringRef Object = Out.str();
  if (Diagnostics.hasDiagnostics()) {
    event(M.getModuleIdentifier(), "bypass", "machine diagnostics");
    return Changed;
  }
  if (I.imagesUnchanged() && saveCheckedObject(Path, Key, Object)) {
    observedMachineObject(M, Key);
    event(M.getModuleIdentifier(), "committed", "exact machine input");
  }
  return Changed;
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

bool llvm::ohm::hasPendingLeaf(CodeGenTargetMachineImpl &TM) {
#if defined(__linux__) && defined(__x86_64__)
  if (auto Origin = PendingEarly.lock())
    return Origin->Leaf && Origin->Leaf->Target == &TM;
#endif
  return false;
}

bool llvm::ohm::runLeafObjectCache(Module &M, CodeGenTargetMachineImpl &TM,
                                   raw_svector_ostream &Out,
                                   function_ref<bool()> Emit) {
#if defined(__linux__) && defined(__x86_64__)
  auto Origin = PendingEarly.lock();
  if (!Origin || !Origin->Leaf || Origin->Input != &M ||
      Origin->Leaf->Target != &TM || !Out.str().empty() ||
      TM.Options.EnableFastISel || TM.Options.EnableGlobalISel ||
      Origin->Diagnostics.hasDiagnostics() || !ordinaryModule(M, false))
    return Emit();
  PendingEarly.reset();
  auto &Leaf = *Origin->Leaf;
  Leaf.Proof = ohm_leaf::prepare(M, Origin->Key);
  if (Leaf.Proof.empty())
    return Emit();
  const auto &I = invocation(/*ForLineObjects=*/true);
  auto Path = joinPath(I.Entries, "llvmcache-" + Origin->Key);
  auto Object = loadLeafObject(Path, Leaf.Proof);
  if (!Object.empty() && I.imagesUnchanged()) {
    Out << Object;
    event(M.getModuleIdentifier(), "leaf-hit");
    return false;
  }
  bool Changed = Emit();
  if (Out.str().size() < MaxOrigin)
    Leaf.Object = Out.str().str();
  return Changed;
#else
  return Emit();
#endif
}

bool llvm::ohm::runDebugObjectCache(Module &M, CodeGenTargetMachineImpl &TM,
                                    raw_svector_ostream &Out,
                                    function_ref<bool()> Emit) {
#if defined(__linux__) && defined(__x86_64__)
  // Bind the donor to the bytes produced for this live module, before the
  // caller writes its object file. Finish the record only on rustc success.
  auto Pending = PendingEarly.lock();
  TraitOrigin *Trait = nullptr;
  if (Pending && Pending->Input == &M && Pending->Trait &&
      Pending->Trait->TM == &TM && Out.str().empty()) {
    Trait = Pending->Trait.get();
    if (!Pending->Leaf)
      PendingEarly.reset();
  }
  auto Capture = scope_exit([&] {
    if (Trait && Out.str().size() <= MaxOrigin - Trait->Manifest.size())
      Trait->Object = Out.str().str();
  });

  // This precedes all cache filesystem activity, including toolchain identity
  // reads and the creation of source or object directories.
  if (!Out.str().empty() ||
      !isLargeDebugObjectInput(M, !M.getNamedMetadata("llvm.dbg.cu")))
    return Emit();
  auto &I = invocation();
  if (I.Root.empty() || !I.imagesUnchanged() ||
      !ordinaryModule(M, I.WithoutDebug))
    return Emit();
  // Recording the outer line certificate requires observing instruction
  // selection and emission. Give this cache precedence over a leaf ticket.
  if (auto Origin = PendingEarly.lock(); Origin && Origin->Input == &M &&
                                         Origin->Leaf &&
                                         Origin->Leaf->Target == &TM)
    PendingEarly.reset();
  std::string Target = machineTarget(M, TM);
  if (I.WithoutDebug)
    return runExactObjectCache(M, I, Target, Out, Emit);
  BLAKE3 Key;
  frame(Key, I.Namespace);
  frame(Key, M.getModuleIdentifier());
  std::string Path =
      joinPath(I.Entries, "llvmcache-" + toHex(Key.final(), true));
  Target = I.Namespace + "|" + Target;
  auto Prior = Origin::read(Path, M.getModuleIdentifier(), Target);
  auto Ref = reinterpret_cast<LLVMModuleRef>(&M);
  Buffer Sources, Map, Proof, Certificate;
  Signature Next;
  const char *Why = nullptr;
  int Status = ohm_debug_sources_v1(
      Ref, I.Cwd.c_str(), I.Remap.c_str(), I.Blobs.c_str(),
      Prior ? Prior->Source.data() : nullptr, Prior ? Prior->Source.size() : 0,
      Prior ? Prior->Object.data() : nullptr, Prior ? Prior->Object.size() : 0,
      &Sources.Value, &Map.Value, &Proof.Value, &Why);
  if (Prior && Status == 0 && !Map.bytes().empty() && !Proof.bytes().empty() &&
      ohm_line_certificate_check_v1(Prior->Certificate.data(),
                                    Prior->Certificate.size(),
                                    Map.bytes().data(), Map.bytes().size()) == 0) {
    Buffer Rebased;
    struct ObjectProjection {
      StringRef Map;
      StringRef Object;
      Buffer &Output;
      const char *&Reason;
    } Projection{Map.bytes(), Prior->Object, Rebased, Why};
    auto CheckLocations = [](void *Context, const char *Records, size_t Size) {
      auto &P = *static_cast<ObjectProjection *>(Context);
      return int(ohm_debug_rebase_locations_v1(
                     P.Map.data(), P.Map.size(), Records, Size, P.Object.data(),
                     P.Object.size(), &P.Output.Value, &P.Reason) == 0);
    };
    // The original ELF's line field selects the comparison. As soon as the
    // old object proves a different runtime word, a plain signature cannot
    // match. Both paths serialize the live module exactly once.
    int Match = ohm_bc_match_relocatable_v1(
        Ref, Prior->SignatureBytes.data(), Prior->SignatureBytes.size(),
        Map.bytes().data(), Map.bytes().size(), Proof.bytes().data(),
        Proof.bytes().size(), CheckLocations, &Projection);
    if ((Match == 2 ||
         (Match == 1 && ohm_debug_rebase_v1(
                            Map.bytes().data(), Map.bytes().size(),
                            Prior->Object.data(), Prior->Object.size(),
                            &Rebased.Value, &Why) == 0)) &&
        Rebased.bytes().starts_with("\x7f"
                                    "ELF") && I.imagesUnchanged()) {
      Out << Rebased.bytes();
      event(M.getModuleIdentifier(), "hit");
      return false;
    }
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

extern "C" int ohm_early_object_begin_v1(LLVMModuleRef Ref,
                                         LLVMTargetMachineRef TM,
                                         const char *Output, size_t Length,
                                         void **Ticket) {
  if (!Ticket)
    return 0;
  *Ticket = nullptr;
#if defined(__linux__) && defined(__x86_64__)
  if (!Ref || !TM)
    return 0;
  auto &M = *unwrap(Ref);
  if (!Output || !Length ||
      !sys::path::is_absolute(StringRef(Output, Length)) ||
      std::getenv("OHM_DISABLE_EARLY_OBJECT_CACHE"))
    return 0;
  const bool WithoutDebug = !M.getNamedMetadata("llvm.dbg.cu");
  const bool Large = ohm::isLargeDebugObjectInput(M, true);
  const bool Leaf = !WithoutDebug && ohm_leaf::candidate(M);
  if (!Large && !Leaf)
    return 0;
  const auto &I = invocation(/*ForLineObjects=*/!WithoutDebug);
  if (I.Root.empty() || I.WithoutDebug != WithoutDebug || !I.imagesUnchanged() ||
      !ordinaryModule(M, I.WithoutDebug))
    return 0;
  auto &C = M.getContext();
  auto *Handler = C.getDiagHandlerPtr();
  if (!Handler || Handler->HasErrors || Handler->isAnyRemarkEnabled() ||
      C.getMainRemarkStreamer() || C.getLLVMRemarkStreamer())
    return 0;
  if (!WithoutDebug) {
    if (!ohm::canUseDebugObjectCache())
      return 0;
    auto &Machine = *reinterpret_cast<TargetMachine *>(TM);
    if (Large && tryTraitObject(M, Machine, I, StringRef(Output, Length), Ticket))
      return 1;
    if (Leaf && !I.TestHarness)
      startLeafObject(M, Machine, I, Ticket);
    return 0;
  }
  SmallVector<char, 128> State;
  if (!ohm::getOptimizerContextState(M, State)) {
    event(M.getModuleIdentifier(), "early-bypass", "unrepresented context");
    return 0;
  }
  SmallVector<char, 0> Input;
  if (!writeClosedOptimizerInput(M, Input))
    return 0;
  BLAKE3 H;
  frame(H, "ohm-optimizer-input-v1");
  frame(H, I.Namespace);
  frame(H, machineTarget(M, *reinterpret_cast<TargetMachine *>(TM)));
  frame(H, M.getModuleIdentifier());
  frame(H, StringRef(Input.data(), Input.size()));
  frame(H, StringRef(State.data(), State.size()));
  auto Key = toHex(H.final(), true);
  auto Index = readPrivate(joinPath(I.Entries, "llvmcache-" + Key), 136);
  if (Index) {
    StringRef R = Index->getBuffer();
    if (R.consume_front("OHMOPT01") && R.size() == 128) {
      auto MK = R.drop_front(64);
      if (llvm::all_of(MK, isHexDigit) &&
          R.take_front(64) == earlyIndexChecksum(Key, MK)) {
        if (auto Saved = readPrivate(
                joinPath(I.Entries, ("llvmcache-" + MK).str()), MaxOrigin)) {
          auto Object = exactObjectPayload(Saved->getBuffer(), MK);
          if (!Object.empty() && I.imagesUnchanged() &&
              writeAtomic(StringRef(Output, Length), {Object}, 0666)) {
            event(M.getModuleIdentifier(), "early-hit",
                  "checked native object");
            return 1;
          }
        }
      }
    }
  }
  auto Origin = std::make_shared<EarlyOrigin>(M, std::move(Key));
  PendingEarly = Origin;
  *Ticket = new std::shared_ptr<EarlyOrigin>(std::move(Origin));
#endif
  return 0;
}
extern "C" void ohm_early_object_end_v1(void *Ticket, int Completed) {
#if defined(__linux__) && defined(__x86_64__)
  std::unique_ptr<std::shared_ptr<EarlyOrigin>> Holder(
      static_cast<std::shared_ptr<EarlyOrigin> *>(Ticket));
  if (!Holder || !Completed)
    return;
  const auto &Entry = **Holder;
  const auto &I = invocation(Entry.Trait || Entry.Leaf);
  if (Entry.Diagnostics.hasDiagnostics() || !I.imagesUnchanged())
    return;
  if (Entry.Leaf)
    storeLeafObject(joinPath(I.Entries, "llvmcache-" + Entry.Key), Entry);
  if (Entry.Trait) {
    auto &R = *Entry.Trait;
    if (R.Object.empty())
      return;
    R.Object.append(R.Manifest.data(), R.Manifest.size());
    char Size[8];
    support::endian::write64le(Size, R.Manifest.size());
    R.Object.append(Size, sizeof(Size));
    if (saveCheckedObject(joinPath(I.Entries, "llvmcache-" + R.Key),
                          R.Key, R.Object, TraitMagic))
      event(Entry.Input->getModuleIdentifier(), "trait-import-committed");
    return;
  }
  if (Entry.Leaf)
    return;
  if (Entry.MachineKey.empty())
    return;
  auto Sum = earlyIndexChecksum(Entry.Key, Entry.MachineKey);
  if (writeAtomic(joinPath(I.Entries, "llvmcache-" + Entry.Key),
                  {"OHMOPT01", Sum, Entry.MachineKey}))
    event(Entry.Input->getModuleIdentifier(), "early-committed",
          "checked native object");
#endif
}
