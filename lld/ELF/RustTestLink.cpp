//===- RustTestLink.cpp ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RustTestLink.h"
#include "Config.h"
#include "Driver.h"
#include "LinkerScript.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/BLAKE3.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/Path.h"
#ifdef __linux__
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <unistd.h>
#endif

using namespace llvm;
using namespace lld::elf;

namespace {
std::string digest(StringRef bytes) {
  auto hash = BLAKE3::hash(arrayRefFromStringRef(bytes));
  return std::string(reinterpret_cast<const char *>(hash.data()), hash.size());
}

void field(BLAKE3 &hash, StringRef value) {
  uint8_t length[8];
  support::endian::write64le(length, value.size());
  hash.update(length);
  hash.update(value);
}

#ifdef __linux__
using Stamp = std::array<uint64_t, 6>;
Stamp generation(const struct stat &st) {
  return {st.st_dev,
          st.st_ino,
          uint64_t(st.st_size),
          uint64_t(st.st_ctim.tv_sec),
          uint64_t(st.st_ctim.tv_nsec),
          st.st_mode};
}

enum class LeaseProof { Stable, Busy, Unavailable };

LeaseProof checkReadLease(int fd, const struct stat &st) {
  // Success excludes existing writable descriptors and shared writable
  // mappings, even when their owners closed the original descriptor. A later
  // writer's first modification must then update this older timestamp. Take
  // the lease only around fstat; a future link checks the generation again.
  if (fcntl(fd, F_SETSIG, SIGURG) < 0)
    return LeaseProof::Unavailable;
  if (fcntl(fd, F_SETLEASE, F_RDLCK) < 0)
    return errno == EAGAIN || errno == EBUSY ? LeaseProof::Busy
                                             : LeaseProof::Unavailable;
  struct stat observed;
  bool stable = !fstat(fd, &observed) && generation(st) == generation(observed);
  return !fcntl(fd, F_SETLEASE, F_UNLCK) && stable ? LeaseProof::Stable
                                                   : LeaseProof::Busy;
}

struct LoadedImages {
  BLAKE3 &hash;
  ArrayRef<StringRef> maps;
  int64_t processStart;
  bool sawLLVM = false;
};

// Initializers may use a provider before LLD even parses its response files.
// Obtain the kernel's process start time, rounding conservatively to seconds.
int64_t processStartTime() {
  auto stat = MemoryBuffer::getFileAsStream("/proc/self/stat");
  struct timespec uptime;
  long ticksPerSecond = sysconf(_SC_CLK_TCK);
  if (!stat || ticksPerSecond <= 0 || clock_gettime(CLOCK_BOOTTIME, &uptime))
    return -1;
  SmallVector<StringRef> fields;
  (*stat)->getBuffer().rsplit(')').second.trim().split(fields, ' ');
  uint64_t startTicks;
  if (fields.size() <= 19 || fields[19].getAsInteger(10, startTicks))
    return -1;
  return time(nullptr) - uptime.tv_sec + int64_t(startTicks / ticksPerSecond);
}

bool isSystemLibrary(StringRef path) {
  return path.starts_with("/usr/lib/") || path.starts_with("/lib/") ||
         path.starts_with("/usr/lib64/") || path.starts_with("/lib64/");
}

bool knownProvider(StringRef name, StringRef path) {
  if (name.empty() || sys::path::filename(name).starts_with("libLLVM.so."))
    return true;
  if (!isSystemLibrary(path))
    return false;
  return is_contained({StringRef("ld-linux-x86-64.so.2"),
                       StringRef("libc.so.6"), StringRef("libdl.so.2"),
                       StringRef("libgcc_s.so.1"), StringRef("libm.so.6"),
                       StringRef("libpthread.so.0"), StringRef("librt.so.1"),
                       StringRef("libstdc++.so.6"), StringRef("libz.so.1"),
                       StringRef("libzstd.so.1")},
                      sys::path::filename(name));
}

// System-owned DSOs often cannot be leased by a developer. Authenticate small
// providers from the opened file, even if an existing writable mapping can
// change their bytes without updating ctime. Large unprovable providers make
// the normal linker cheaper than repeatedly hashing the compiler backend.
bool hashProvider(BLAKE3 &hash, int fd, const struct stat &st, StringRef path) {
  auto stamp = generation(st);
  field(hash,
        StringRef(reinterpret_cast<const char *>(stamp.data()), sizeof(stamp)));
  LeaseProof lease = checkReadLease(fd, st);
  if (lease == LeaseProof::Stable) {
    field(hash, "generation");
    return true;
  }
  // A developer-owned provider must exclude open writers when its generation
  // is captured. Two byte samples can both miss a transient mapping update.
  // Use the content fallback only for non-writable installed system libraries.
  if (lease == LeaseProof::Busy || !isSystemLibrary(path) ||
      st.st_uid == geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH)) ||
      st.st_size < 0 || st.st_size > 32 * 1024 * 1024)
    return false;
  BLAKE3 contents;
  uint8_t data[16384];
  for (off_t pos = 0; pos < st.st_size;) {
    ssize_t n =
        pread(fd, data, std::min<off_t>(sizeof(data), st.st_size - pos), pos);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    contents.update(ArrayRef(data, n));
    pos += n;
  }
  field(hash, "bytes");
  auto value = contents.final();
  field(hash,
        StringRef(reinterpret_cast<const char *>(value.data()), value.size()));
  return true;
}

// A binary may retain its GNU build ID after a post-link edit. Check the file
// that supplied its actual mapping, including DSOs loaded before an upgrade.
int hashLoadedImage(dl_phdr_info *info, size_t, void *opaque) {
  auto &images = *static_cast<LoadedImages *>(opaque);
  // The boot ID already identifies the kernel's optional vDSO.
  if (StringRef(info->dlpi_name) == "linux-vdso.so.1")
    return 0;
  if (sys::path::filename(info->dlpi_name).starts_with("libLLVM.so.")) {
    if (images.sawLLVM)
      return 1;
    images.sawLLVM = true;
  }
  auto headers = ArrayRef(info->dlpi_phdr, info->dlpi_phnum);
  auto load = llvm::find_if(
      headers, [](const auto &p) { return p.p_type == PT_LOAD && p.p_memsz; });
  if (load == headers.end())
    return 1;
  auto address = info->dlpi_addr + load->p_vaddr;
  for (StringRef line : images.maps) {
    unsigned long start, end;
    unsigned devMajor, devMinor;
    unsigned long long ino;
    int pathOffset = 0;
    if (sscanf(line.str().c_str(), "%lx-%lx %*s %*s %x:%x %llu %n", &start,
               &end, &devMajor, &devMinor, &ino, &pathOffset) != 5 ||
        address < start || address >= end)
      continue;
    StringRef path = line.drop_front(pathOffset);
    if (!sys::path::is_absolute(path) || !knownProvider(info->dlpi_name, path))
      return 1;
    int fd = open(path.str().c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
      return 1;
    auto closeFile = llvm::scope_exit([&] { close(fd); });
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) ||
        st.st_ctim.tv_sec + 2 >= images.processStart || ino != st.st_ino ||
        devMajor != major(st.st_dev) || devMinor != minor(st.st_dev))
      return 1;
    if (!*info->dlpi_name) {
      // Invoking the dynamic loader directly also permits --preload and
      // --audit outside the corresponding environment variables.
      char executable[PATH_MAX + 1];
      ssize_t n = readlink("/proc/self/exe", executable, PATH_MAX);
      if (n <= 0 || n == PATH_MAX)
        return 1;
      executable[n] = '\0';
      struct stat executableStat;
      if (stat(executable, &executableStat) ||
          executableStat.st_dev != st.st_dev ||
          executableStat.st_ino != st.st_ino)
        return 1;
    }
    field(images.hash, path);
    return hashProvider(images.hash, fd, st, path) ? 0 : 1;
  }
  return 1;
}

std::string loadedImageHash() {
  auto maps = MemoryBuffer::getFileAsStream("/proc/self/maps");
  if (!maps)
    return {};
  SmallVector<StringRef> lines;
  (*maps)->getBuffer().split(lines, '\n');
  BLAKE3 hash;
  auto start = processStartTime();
  if (start < 0)
    return {};
  LoadedImages images{hash, lines, start};
  return dl_iterate_phdr(hashLoadedImage, &images) ? "" : toHex(hash.final());
}

bool ordinaryArgument(const opt::Arg &arg) {
  switch (arg.getOption().getID()) {
  case OPT_INPUT:
  case OPT_o:
  case OPT_m:
  case OPT_pie:
  case OPT_dynamic_linker:
  case OPT_as_needed:
  case OPT_Bstatic:
  case OPT_Bdynamic:
  case OPT_library:
  case OPT_library_path:
  case OPT_hash_style:
  case OPT_eh_frame_hdr:
  case OPT_gc_sections:
    return true;
  case OPT_build_id:
    return StringRef(arg.getValue()) == "sha1";
  case OPT_z:
    return is_contained(
        {StringRef("relro"), StringRef("now"), StringRef("noexecstack")},
        StringRef(arg.getValue()));
  default:
    return false;
  }
}

constexpr StringLiteral receiptMagic = "OHMLLD2\n";

std::string readReceipt(int fd) {
  struct stat st;
  char data[112];
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & 0077) || st.st_nlink != 1 || st.st_size != sizeof(data) ||
      pread(fd, data, sizeof(data), 0) != sizeof(data))
    return {};
  StringRef body(data, sizeof(data) - 32);
  if (!body.starts_with(receiptMagic) ||
      digest(body) != StringRef(data + body.size(), 32))
    return {};
  return body.drop_front(receiptMagic.size()).str();
}

std::string previousReceipt(const std::string &path) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return {};
  auto closeFile = llvm::scope_exit([&] { close(fd); });
  return readReceipt(fd);
}
#endif
} // namespace

void RustTestLink::configure(Ctx &ctx, const opt::InputArgList &args) {
#ifdef __linux__
  const char *pkg = getenv("CARGO_PKG_NAME");
  StringRef path = ctx.arg.outputFile;
  StringRef tail = sys::path::filename(path).rsplit('-').second;
  if (!pkg || !*pkg || !getenv("CARGO_MANIFEST_DIR") || getenv("LD_PRELOAD") ||
      getenv("LD_AUDIT") || access("/etc/ld.so.preload", F_OK) == 0 ||
      !sys::path::is_absolute(path) || !ctx.e.exitEarly ||
      ctx.e.disableOutput || ctx.tar || !args.hasArg(OPT_pie) ||
      args.getLastArgValue(OPT_m) != "elf_x86_64" || tail.size() != 16 ||
      !llvm::all_of(tail, llvm::isHexDigit) ||
      !llvm::all_of(args, [](auto *a) { return ordinaryArgument(*a); }))
    return;
  output = path.str();
  objectDir = output + ".rlink-objects";
  bool retainedObject = false;
  std::string self = "lib" + std::string(pkg);
  std::replace(self.begin(), self.end(), '-', '_');
  self += '-';
  for (auto *a : args.filtered(OPT_INPUT)) {
    StringRef input = a->getValue();
    StringRef dir = sys::path::parent_path(input);
    StringRef name = sys::path::filename(input);
    retainedObject |= dir == objectDir && name.ends_with(".rcgu.o");
    if (name.starts_with(self) && name.ends_with(".rlib")) {
      disable();
      return;
    }
    if (name == "symbols.o" &&
        sys::path::parent_path(dir) == sys::path::parent_path(path) &&
        sys::path::filename(dir).starts_with("rustc"))
      scratchName = dir.str();
  }
  if (!retainedObject || scratchName.empty()) {
    disable();
    return;
  }
  auto boot = MemoryBuffer::getFileAsStream("/proc/sys/kernel/random/boot_id");
  imageHash = loadedImageHash();
  if (!boot || (*boot)->getBufferSize() != 37 || imageHash.empty()) {
    disable();
    return;
  }
  BLAKE3 h;
  field(h, imageHash);
  field(h, "Ohm Rust ELF test link v2");
  field(h, (*boot)->getBuffer());
  field(h, getenv("LLD_VERSION"));
  for (auto *a : args) {
    field(h, normalize(a->getSpelling()));
    for (unsigned i = 0; i != a->getNumValues(); ++i)
      field(h, normalize(a->getValue(i)));
  }
  argsHash = toHex(h.final());
#endif
}

std::string RustTestLink::normalize(StringRef path) const {
  if (sys::path::parent_path(path) == objectDir && path.ends_with(".rcgu.o")) {
    auto [stem, session] = path.drop_back(7).rsplit('.');
    if (session.size() == 7 && llvm::all_of(session, llvm::isAlnum))
      return (stem + ".rcgu.o").str();
  }
  if (sys::path::parent_path(path) == scratchName &&
      (sys::path::filename(path) == "symbols.o" ||
       sys::path::filename(path) == "raw-dylibs"))
    return (Twine("<rustc-scratch>/") + sys::path::filename(path)).str();
  return path.str();
}

ErrorOr<std::unique_ptr<MemoryBuffer>> RustTestLink::readFile(StringRef path) {
  auto ordinary = [&] { return MemoryBuffer::getFile(path, false, false); };
#ifdef __linux__
  // A library requested while resolving an object is outside the initial
  // driver search. Its selection must run again at the next link.
  if (!key.empty())
    disable();
  if (output.empty())
    return ordinary();
  auto copyInput = [&](ErrorOr<std::unique_ptr<MemoryBuffer>> mb)
      -> ErrorOr<std::unique_ptr<MemoryBuffer>> {
    if (!mb)
      return mb.getError();
    if ((*mb)->getBufferSize() > 32 * 1024 * 1024) {
      disable();
      return mb;
    }
    // The digest must describe the bytes the linker consumes, including a
    // file edited and restored while the symbol table is being constructed.
    return MemoryBuffer::getMemBufferCopy((*mb)->getBuffer(), path);
  };
  if (!path.ends_with(".rlib"))
    return copyInput(ordinary());
  int fd = open(path.str().c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0)
    return std::error_code(errno, std::generic_category());
  auto closeFile = llvm::scope_exit([&] { close(fd); });
  struct stat st;
  struct statfs fs;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || fstatfs(fd, &fs) ||
      (fs.f_type != 0xef53 && fs.f_type != 0x58465342 &&
       fs.f_type != 0x9123683e && fs.f_type != 0x01021994))
    return copyInput(ordinary());
  auto buffer = MemoryBuffer::getOpenFile(fd, path, st.st_size, false, false);
  // Recently written files may still share the filesystem's current timestamp
  // tick. Authenticate their bytes, as for all native objects and scripts.
  if (buffer && st.st_size >= 64 * 1024 &&
      st.st_ctim.tv_sec + 2 < time(nullptr) &&
      checkReadLease(fd, st) == LeaseProof::Stable) {
    archives[buffer->get()] = generation(st);
    return buffer;
  }
  return copyInput(std::move(buffer));
#else
  return ordinary();
#endif
}

bool RustTestLink::inputsStillPresent() const {
#ifdef __linux__
  for (const auto &a : archives) {
    struct stat st;
    if (stat(a.first->getBufferIdentifier().str().c_str(), &st) ||
        generation(st) != a.second)
      return false;
  }
  return true;
#else
  return false;
#endif
}

std::string RustTestLink::inputKey(Ctx &ctx) const {
  BLAKE3 h;
  field(h, argsHash);
  for (const auto &mb : ctx.memoryBuffers) {
    StringRef name = mb->getBufferIdentifier();
    if (!sys::path::is_absolute(name) || mb->getBuffer().starts_with("!<thin>"))
      return {};
    auto it = archives.find(mb.get());
    field(h, normalize(name));
    if (it == archives.end()) {
      field(h, "bytes");
      field(h, digest(mb->getBuffer()));
    } else {
      field(h, "generation");
      field(h, StringRef(reinterpret_cast<const char *>(it->second.data()),
                         sizeof(it->second)));
    }
  }
  auto hash = h.final();
  return std::string(reinterpret_cast<const char *>(hash.data()), hash.size());
}

bool RustTestLink::reuse(Ctx &ctx) {
#ifdef __linux__
  if (output.empty() || ctx.e.warningCount || ctx.e.errorCount ||
      !ctx.bitcodeFiles.empty() || ctx.script->hasSectionsCommand ||
      !inputsStillPresent())
    return false;
  key = inputKey(ctx);
  if (key.empty())
    return false;
  std::string receipt = previousReceipt(output + ".lld-relink");
  if (!StringRef(receipt).starts_with(key))
    return false;
  int fd = open(output.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd == -1)
    return false;
  auto closeFile = llvm::scope_exit([&] { close(fd); });
  struct stat st;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
      st.st_size <= 0 ||
      uint64_t(st.st_size) != support::endian::read64le(receipt.data() + 32))
    return false;
  unsigned flags = FileOutputBuffer::F_executable;
  if (ctx.arg.mmapOutputFile)
    flags |= FileOutputBuffer::F_mmap;
  auto copy = FileOutputBuffer::create(output, st.st_size, flags);
  if (!copy) {
    consumeError(copy.takeError());
    return false;
  }
  auto *data = (*copy)->getBufferStart();
  // Read directly into the new output. A concurrent truncation of the old
  // executable produces a short read instead of a fault on a mapped page.
  for (size_t pos = 0, size = st.st_size; pos != size;) {
    ssize_t n = read(fd, data + pos, size - pos);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    pos += n;
  }
  if (digest(StringRef(reinterpret_cast<const char *>(data), st.st_size)) !=
          StringRef(receipt).drop_front(40) ||
      !inputsStillPresent() || loadedImageHash() != imageHash)
    return false;
  if (auto error = (*copy)->commit()) {
    consumeError(std::move(error));
    return false;
  }
  return true;
#else
  return false;
#endif
}

void RustTestLink::observeOutput(StringRef bytes) {
  if (!key.empty() && !bytes.contains(".rcgu.o") &&
      !bytes.contains("symbols.o") &&
      !bytes.contains(sys::path::filename(scratchName))) {
    char size[8];
    support::endian::write64le(size, bytes.size());
    outputHash = StringRef(size, sizeof(size)).str() + digest(bytes);
  }
}

void RustTestLink::publish(Ctx &ctx) {
#ifdef __linux__
  if (key.empty() || outputHash.empty() || ctx.e.warningCount ||
      !ctx.bitcodeFiles.empty() || !ctx.lazyBitcodeFiles.empty() ||
      !inputsStillPresent() || loadedImageHash() != imageHash)
    return;
  std::string path = output + ".lld-relink";
  std::string record = receiptMagic.str() + key + outputHash;
  record += digest(record);
  // Update only the specific open inode that we have recognized as our own.
  // If another writer replaces its pathname, its file stays untouched. A
  // reader observing a partial update rejects the checksum and links normally.
  int previous =
      open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (previous >= 0) {
    auto closeFile = llvm::scope_exit([&] { close(previous); });
    if (!readReceipt(previous).empty() && !flock(previous, LOCK_EX | LOCK_NB) &&
        !readReceipt(previous).empty())
      (void)pwrite(previous, record.data(), record.size(), 0);
    return;
  }
  if (errno != ENOENT)
    return;
  std::string tmp = path + ".XXXXXX";
  int fd = mkstemp(tmp.data());
  if (fd == -1)
    return;
  auto remove = llvm::scope_exit([&] { unlink(tmp.c_str()); });
  bool complete =
      write(fd, record.data(), record.size()) == ssize_t(record.size());
  if (!close(fd) && complete)
    (void)::link(tmp.c_str(), path.c_str());
#endif
}
