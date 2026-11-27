/* SPDX-License-Identifier: MIT */
/*
 * Files for Lua programs, on tilefs (the SD card on the board): Lua's io and os functions as far as they make sense here (io.open, io.lines, io.read and
 * io.write on the console, io.type; os.remove, os.rename, os.clock, os.time, os.exit, and os.dir to list a folder), and loadfile, dofile and require
 * reading .lua files from it. Used in place of Lua's own io, os and package libraries, which would go to the host's files.
 */
#ifndef TL_IO_H
#define TL_IO_H

#include <stddef.h>

struct lua_State;
struct tl_env;

/* Registers io, os, loadfile, dofile, require, package.loaded, mount and unmount (tl_env does, for every new state). */
void tl_io_open(struct lua_State *L);

/* ---- zip archives as folders (tilefs's fs_zip): MOUNT and UNMOUNT at the prompt, mount() and unmount() in Lua, RUN game.zip ----
 * An archive stays mounted until it is unmounted: it belongs to the file system, not to a program, so NEW and RUN leave it. Its memory (the
 * archive's directory, about 50 to 100 bytes an entry) comes from the environment's alloc hook. */

#define TL_ARCHIVE_DEFAULT "ar"   /* where an archive goes if no name is given, and where RUN game.zip puts it */

/* Mounts the archive at /name (in place of an archive that is there already; the same archive again changes nothing). 0, or a negative
 * FS_E* code with what went wrong in `why` (n bytes). */
int tl_io_mount(struct tl_env *e, const char *archive, const char *name, char *why, size_t n);
/* 0, or FS_ENOENT if no archive is mounted there (another kind of mount is not unmounted), FS_EBUSY if a file in it is open (a file that
 * nothing refers to any more is closed first: Lua collects its garbage). */
int tl_io_unmount(struct tl_env *e, const char *name);
/* The i-th archive mounted (from 0): its mount's name, and the archive's path; NULL past the last. */
const char *tl_io_archive(int i, const char **path);
/* Forgets the archives without touching the file system, giving their memory back: for tl_env_init, which follows a fs_reset (or a fresh
 * start), after which nothing of theirs is mounted any more. */
void tl_io_forget_archives(struct tl_env *e);

/* Is the value at idx what os.exit raises to end a program (not an error)? */
int tl_io_is_exit(struct lua_State *L, int idx);

#endif
