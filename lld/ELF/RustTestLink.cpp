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
#include "lld/Common/Version.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/BLAKE3.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/Path.h"
#ifdef __linux__
#include <fcntl.h>
#include <link.h>
#include <signal.h>
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

bool hasStableGeneration(int fd, const struct stat &st) {
  if (st.st_size < 64 * 1024 || st.st_ctim.tv_sec + 2 >= time(nullptr))
    return false;
  // Success excludes existing writable descriptors and shared writable
  // mappings, even when their owners closed the original descriptor. A later
  // writer's first modification must then update this older timestamp. Take
  // the lease only around fstat; a future link checks the generation again.
  if (fcntl(fd, F_SETSIG, SIGURG) < 0 || fcntl(fd, F_SETLEASE, F_RDLCK) < 0)
    return false;
  struct stat observed;
  bool stable = !fstat(fd, &observed) && generation(st) == generation(observed);
  return !fcntl(fd, F_SETLEASE, F_UNLCK) && stable;
}

// Name the images actually loaded by this process, even if a package upgrade
// has already replaced one of their filesystem paths.
int hashBuildId(dl_phdr_info *info, size_t, void *opaque) {
  auto &h = *static_cast<BLAKE3 *>(opaque);
  // The boot ID already identifies the kernel's optional vDSO.
  if (StringRef(info->dlpi_name) == "linux-vdso.so.1")
    return 0;
  for (const auto &p : ArrayRef(info->dlpi_phdr, info->dlpi_phnum)) {
    if (p.p_type != PT_NOTE || !p.p_memsz)
      continue;
    const char *data =
        reinterpret_cast<const char *>(info->dlpi_addr + p.p_vaddr);
    StringRef notes(data, p.p_memsz);
    while (notes.size() >= sizeof(ElfW(Nhdr))) {
      ElfW(Nhdr) note;
      memcpy(&note, notes.data(), sizeof(note));
      notes = notes.drop_front(sizeof(note));
      size_t names = alignTo(uint64_t(note.n_namesz), 4);
      size_t desc = alignTo(uint64_t(note.n_descsz), 4);
      if (names > notes.size() || desc > notes.size() - names)
        break;
      if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
          notes.take_front(4) == StringRef("GNU\0", 4) && note.n_descsz >= 8) {
        field(h, info->dlpi_name);
        field(h, notes.substr(names, note.n_descsz));
        return 0;
      }
      notes = notes.drop_front(names + desc);
    }
  }
  // Rust's distributed LLVM DSO can omit its build ID. Verify that its name
  // still identifies the inode that supplied the running mapping.
  StringRef path = *info->dlpi_name ? info->dlpi_name : "/proc/self/exe";
  if (!sys::path::is_absolute(path))
    return 1;
  struct stat st;
  if (stat(path.str().c_str(), &st) || !S_ISREG(st.st_mode) ||
      st.st_ctim.tv_sec + 2 >= time(nullptr))
    return 1;
  auto maps = MemoryBuffer::getFileAsStream("/proc/self/maps");
  if (!maps)
    return 1;
  SmallVector<StringRef> lines;
  (*maps)->getBuffer().split(lines, '\n');
  auto headers = ArrayRef(info->dlpi_phdr, info->dlpi_phnum);
  auto load = llvm::find_if(
      headers, [](const auto &p) { return p.p_type == PT_LOAD && p.p_memsz; });
  if (load == headers.end())
    return 1;
  auto address = info->dlpi_addr + load->p_vaddr;
  for (StringRef line : lines) {
    unsigned long start, end;
    unsigned devMajor, devMinor;
    unsigned long long ino;
    if (sscanf(line.str().c_str(), "%lx-%lx %*s %*s %x:%x %llu", &start, &end,
               &devMajor, &devMinor, &ino) == 5 &&
        start <= address && address < end && ino == st.st_ino &&
        devMajor == major(st.st_dev) && devMinor == minor(st.st_dev)) {
      field(h, path);
      auto stamp = generation(st);
      field(h, StringRef(reinterpret_cast<const char *>(stamp.data()),
                         sizeof(stamp)));
      return 0;
    }
  }
  return 1;
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

// A partial or redirected cache file can only cause an ordinary link.
std::string previousReceipt(const std::string &path) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return {};
  auto closeFile = llvm::scope_exit([&] { close(fd); });
  struct stat st;
  char data[104];
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & 0077) || st.st_size != sizeof(data) ||
      read(fd, data, sizeof(data)) != sizeof(data))
    return {};
  StringRef payload(data, 72);
  return digest(payload) == StringRef(data + 72, 32) ? payload.str() : "";
}
#endif
} // namespace

void RustTestLink::configure(Ctx &ctx, const opt::InputArgList &args) {
#ifdef __linux__
  const char *pkg = getenv("CARGO_PKG_NAME");
  StringRef path = ctx.arg.outputFile;
  StringRef tail = sys::path::filename(path).rsplit('-').second;
  if (!pkg || !*pkg || !getenv("CARGO_MANIFEST_DIR") ||
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
  BLAKE3 h;
  if (!boot || (*boot)->getBufferSize() != 37 ||
      dl_iterate_phdr(hashBuildId, &h)) {
    disable();
    return;
  }
  field(h, "Ohm Rust ELF test link v1");
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
  if (buffer && hasStableGeneration(fd, st)) {
    archives[path] = generation(st);
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
    if (stat(a.getKey().str().c_str(), &st) || generation(st) != a.second)
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
    auto it = archives.find(name);
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
      !inputsStillPresent())
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
      !inputsStillPresent())
    return;
  std::string path = output + ".lld-relink";
  std::string tmp = path + ".XXXXXX";
  int fd = mkstemp(tmp.data());
  if (fd == -1)
    return;
  auto remove = llvm::scope_exit([&] { unlink(tmp.c_str()); });
  std::string record = key + outputHash;
  record += digest(record);
  bool complete =
      write(fd, record.data(), record.size()) == ssize_t(record.size());
  if (!close(fd) && complete)
    rename(tmp.c_str(), path.c_str());
#endif
}
