Reusing a Rust test executable in Ohm
=====================================

On Linux, Ohm's ELF linker can reuse a Cargo executable when rustc submits
the same link again after an edit. It applies to x86-64 PIE links with retained
``.rlink-objects`` and no input rlib from the executable's own package.
LLD first resolves libraries and reads linker scripts in the usual search order.
It checks the command, selected inputs and loaded toolchain against a private
``.lld-relink`` file beside the executable. Linker maps, thin archives, LTO,
side-effecting scripts and links which emitted warnings use the ordinary path.

Native objects, small or recently changed rlibs, scripts and shared libraries
are identified by their bytes. Older large rlibs on ext, XFS, Btrfs and tmpfs
can be identified by their open file's device, inode, size, mode and nanosecond
change time within a single Linux boot. A short read lease excludes open writers
and shared writable mappings when selecting a file generation. Restoring or moving a build tree can
require a fresh link. Rust's session-specific object names are ignored only for
the known retained-object and scratch locations, and the previous executable
must omit those temporary names. LLD verifies its complete contents with BLAKE3
before committing a replacement with the ordinary output permissions.
Deleting the small receipt or the executable causes a normal link.
