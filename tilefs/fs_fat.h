/* SPDX-License-Identifier: MIT */
/*
 * The FatFs (ChaN, R0.16) backend for tilefs: FAT12/16/32 and exFAT with long names, on any fs_blockdev_t. Each mount takes one of
 * FF_VOLUMES (2) FatFs drives. FatFs itself is the unmodified copy in third_party/fatfs; its options are in that directory's ffconf.h.
 * Names are the OEM code page 437; times come from the clock hook (or 2026-01-01 if there is none).
 */
#ifndef FS_FAT_H
#define FS_FAT_H

#include "ff.h"
#include "tilefs.h"

typedef struct {
    FIL f;
    uint8_t used;
} fs_fat_file_t;

typedef struct {
    DIR d;
    uint8_t used;
} fs_fat_dir_t;

typedef struct {
    FATFS fs;
    FILINFO info;   /* scratch, kept here to spare the stack */
    char path[FS_PATH_MAX + 8], path2[FS_PATH_MAX + 8];
    fs_fat_file_t file[FS_MAX_FILES];
    fs_fat_dir_t dir[FS_MAX_DIRS];
    uint8_t drive, mounted;
} fs_fat_t;

#define FS_FAT_ANY 0     /* the type that suits the size */
#define FS_FAT_FAT 1     /* FAT12 or FAT16 by size */
#define FS_FAT_FAT32 2
#define FS_FAT_EXFAT 3
#define FS_FAT_SFD 0x10   /* ORed in: no partition table, the file system starts at sector 0 (a "superfloppy") */

/* Makes a new, empty file system on the device, destroying what is there. Unless FS_FAT_SFD is given, a partition table with one
 * partition is written first, as a PC would for a card. Do this before fs_fat_init, with the drive unused. */
int fs_fat_format(unsigned drive, const fs_blockdev_t *dev, void *dev_ctx, int type);

/* Attaches the device to a drive and mounts it: FS_ENODEV if there is no recognisable file system, FS_EIO if the device fails. */
int fs_fat_init(fs_fat_t *f, unsigned drive, const fs_blockdev_t *dev, void *dev_ctx);
/* Flushes and detaches. Unmount it from tilefs first (fs_unmount refuses while anything is open). */
int fs_fat_done(fs_fat_t *f);

/* Where file times come from: seconds since 1970 (UTC, taken as is: FAT has no zone). NULL: a fixed time. */
void fs_fat_set_clock(uint32_t (*unix_time)(void));

/* Conversions between FAT date/time fields and seconds since 1970, for tests and tools. FAT times have 2 s steps and run 1980..2107; times after the 32-bit seconds count ends (2106) are given as 0xFFFFFFFF, and earlier ones as 1980-01-01. */
uint32_t fs_fat_to_unix(uint16_t fdate, uint16_t ftime);
uint32_t fs_fat_from_unix(uint32_t t);   /* date in the high half, time in the low, as FatFs's get_fattime */

extern const fs_ops_t fs_fat_ops;

#endif
