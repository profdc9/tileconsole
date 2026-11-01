# tilefs

A small file system layer for console-style programs: one POSIX-flavoured API (`fs_open`, `fs_read`, `fs_write`, `fs_seek`,
directories, `fs_rename`, ...) over interchangeable backends mounted under names. Plain C, no malloc in the core, no
dependencies. See `tilefs.h`, whose header comment lists exactly what the layer promises.

Backends:

- `fs_posix.c`: a directory of the host, for desktop builds and tests.
- `fs_ram.c`: a RAM disk in a block of memory you give it (fixed-size blocks and entries, so it never fragments), for
  boards without storage.
- `fs_fat.c`: FAT12/16/32 and exFAT with long names, through ChaN's FatFs R0.16 (unmodified, in `third_party/fatfs`, with
  its own BSD-style licence; the configuration is that directory's `ffconf.h`). It runs on any *block device*
  (`fs_blockdev_t`: 512-byte sectors): `fs_blk_file.c` is a disk image in a host file, for debugging the whole stack on a PC,
  `fs_blk_ram.c` a block of RAM, and an SD card on SPI is the third (below).

The same program then runs on all of them. To make that true the layer is stricter than any one of them: portable FAT-style
names, no holes in files (seeking outside the file is an error), `rename` never replaces, a file that is open cannot be
unlinked, renamed or truncated, and a file open for writing cannot be opened again. FatFs's own checks and locking are off,
because the core does these.

## The SD card (EmuBoard2350)

`sd_spi.c` drives an SD / SDHC / SDXC card in SPI mode as a block device (`sd_spi_dev`): card set-up (CMD0, CMD8, ACMD41, OCR,
CSD for the size), byte or block addressing, single and multiple block transfers with CRC checking, retries, busy handling. It
reaches the card only through a four-function transport (`sd_spi_io_t`: chip select, byte transfer, bus speed, millisecond
clock), so the protocol is tested on a PC against a software model of a card (`test_sd.c`) and runs on the board through
`pico/sd_spi_pico.c`.

The socket is wired for SPI0 (from the schematic): MISO = GPIO4, CS = GPIO5, SCK = GPIO6, MOSI = GPIO7, with 10k pull-ups to
3.3 V on MISO, CS and MOSI and no card-detect line. The transport sets **MISO to INPUT_PULLUP**, so an empty socket reads 0xFF and
the driver reports `FS_ENOMEDIA` (a card with no file system is `FS_ENODEV`).

    # CMakeLists.txt of a pico-sdk project:   include(.../tilefs/tilefs.cmake)   tilefs_add(<target>)
    # then, in C:
    sd_spi_pico_init(&port, &sd, spi0, SD_SPI_PICO_EMUBOARD2350, 12500000);
    fs_fat_init(&fat, 0, &sd_spi_dev, &sd);        // the card is set up here
    fs_mount("sd", &fs_fat_ops, &fat);

The driver was written from the SD specification and then compared with ChaN's SPI-mode drivers from the FatFs sample projects
(`ffsample.zip`: the STM32, AVR and LPC1768 ones, which share one structure). It already did the same set-up (slow clock, 80 clocks
with the card not selected, CMD0, CMD8 with its echo check, ACMD41 with HCS, OCR for the addressing, CMD16, the CMD12 stuff byte, the
stop token, busy waits). Adopted from them: 10 ms of settling before the first clocks, a chip select and a clock byte around every
command (including between CMD55 and the ACMD), and ACMD23 before multiple block writes. Changed because of them: CRC checking. ChaN
ignores CRCs; this driver asks for CRC checking with CMD59 and checks read CRCs only if the card agreed, since a card with checking
off need not send a valid one (set `sd.no_crc = 1` to leave it off). Not taken: MMC support (CMD1), card detect and write protect
switches (the socket has none wired).

The bus runs at 400 kHz until the card is set up and then at the speed given (25 MHz is the most a standard card takes; 12.5 MHz is a
safe first try on a new board). The FatFs code needs more than the pico-sdk's default 2 KB of stack: build with
`PICO_STACK_SIZE=0x1000`.

## Tests

`make check` builds with AddressSanitizer and UBSan and runs:

- a conformance suite against every backend (RAM, host, and FAT16, FAT32 and exFAT images): open modes, seeking, block
  boundaries, directories, rename, the busy rules, path handling, handle limits, space accounting;
- the mount namespace, and a recording backend that shows what the core actually passes down;
- the RAM disk's limits and the FAT file systems filling up (short writes, `FS_ENOSPC`, a full FAT16 root directory);
- FAT timestamps (dates checked against known values, leap years, 2100, the 1980 and 2106 limits), the clock hook, a flaky
  block device (read/write/init errors reach the caller as `FS_EIO`), an unformatted device (`FS_ENODEV`), long and odd
  names, two FAT drives at once;
- **other people's tools**: images written by tilefs are checked by `fsck.vfat` / `fsck.exfat` (with and without a partition
  table) and read back with mtools; images made by `mkfs.vfat` / `mkfs.exfat` and mtools are read and written by tilefs and
  checked again (skipped if the tools are not installed);
- **the SD driver** (`test_sd.c`, a card model that insists on what a real card insists on: 10 ms and 74+ clocks before CMD0, a slow bus
  until the card is set up, command and data CRCs, byte or block addresses, busy, eight clocks after deselect, a stop token to end
  a multiple block write; also cards that do not know CMD59 and cards that send junk CRCs when checking is off): version 1, version 2 and SDHC cards, sizes from the CSD, block I/O against a shadow copy, transient and
  lasting read and write faults, no card, a stuck busy line, and the whole stack (FatFs on the driver on the model, checked by fsck);
- a random differential test of each backend against the host's file system (the host stands in as the reference;
  `./test_tilefs OPS SEED` runs more of it).

Mutation testing (deliberate breaks of the core and the backends) is caught except changes where another layer gives the same
answer anyway. MIT licensed (FatFs under its own licence).
