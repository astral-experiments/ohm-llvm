Reusing a Rust test executable in Ohm
=====================================

On Linux, Ohm's ELF linker can reuse a Cargo executable when rustc submits
the same link again after an edit. It applies to x86-64 PIE links with retained
``.rlink-objects`` and no input rlib from the executable's own package.
LLD first resolves libraries and reads linker scripts in the usual search order.
It checks the command, selected inputs and loaded toolchain against a private
``.lld-relink`` file beside the executable. Linker maps, thin archives, LTO,
side-effecting scripts and links which emitted warnings use the ordinary path.
Explicit dynamic-loader invocation and interposition through ``LD_PRELOAD``,
``LD_AUDIT`` or a system preload file also select the ordinary path.
The linker verifies the files actually mapped into its process before accepting
or recording a result. The supported runtime consists of the linker, its LLVM
library and system libraries used by the Ohm Linux distribution. Additional
providers and libraries changed since the process started use the ordinary path.
Developer-owned providers require a read lease. LLD can instead hash the bytes
of small installed system libraries the developer cannot write. A provider
larger than 32 MiB without a stable generation selects the ordinary path.

Native objects, small or recently changed rlibs, scripts and shared libraries
are identified by their bytes. Older large rlibs on ext, XFS, Btrfs and tmpfs
can be identified by their open file's device, inode, size, mode and nanosecond
change time within a single Linux boot. A short read lease excludes open writers
and shared writable mappings when selecting a file generation. Restoring or
moving a build tree can require a fresh link. Rust's session-specific object
names are ignored only for the known retained-object and scratch locations. The
previous executable must omit those names. LLD verifies its contents with BLAKE3
before committing a replacement with the ordinary output permissions.
LLD creates the receipt only if its name is available, and updates an existing
one only after verifying its contents on the open file descriptor. Other files
at that path are preserved. Removing a damaged receipt allows a later link to
create a new one.
