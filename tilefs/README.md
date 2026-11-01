# tilefs

A small file system layer for console-style programs: one POSIX-flavoured API (`fs_open`, `fs_read`, `fs_write`, `fs_seek`,
directories, `fs_rename`, ...) over interchangeable backends mounted under names. Plain C, no malloc in the core, no
dependencies. See `tilefs.h`, whose header comment lists exactly what the layer promises.

Backends:

- `fs_posix.c`: a directory of the host, for desktop builds and tests.
- `fs_ram.c`: a RAM disk in a block of memory you give it (fixed-size blocks and entries, so it never fragments), for
  boards without storage.
- FatFs on an SD card (SPI) is planned as the third.

The same program then runs on all of them. To make that true the layer is stricter than any one of them: portable FAT-style
names, no holes in files (seeking outside the file is an error), `rename` never replaces, a file that is open cannot be
unlinked, renamed or truncated, and a file open for writing cannot be opened again.

`make check` builds with AddressSanitizer and UBSan and runs: a conformance suite against both backends (open modes, seeking,
block boundaries, directories, rename, the busy rules, path handling, handle limits, space accounting), tests of the mount
namespace, a recording backend that shows what the core actually passes down, the RAM disk's limits (full disk, short writes,
no leaked blocks or entries over 300 fill/empty cycles, running out of entries), and a random differential test of
the RAM disk against the host's file system (the host stands in as the reference; `./test_tilefs OPS SEED` runs more of it).
Mutation testing of the core and both backends (52 deliberate breaks) is caught except four equivalent changes where a
backend gives the same answer anyway. MIT licensed.
