/* SPDX-License-Identifier: MIT */
/*
 * Files for Lua programs, on tilefs (the SD card on the board): Lua's io and os functions as far as they make sense here (io.open, io.lines, io.read and
 * io.write on the console, io.type; os.remove, os.rename, os.clock, os.time, os.exit, and os.dir to list a folder), and loadfile, dofile and require
 * reading .lua files from it. Used in place of Lua's own io, os and package libraries, which would go to the host's files.
 */
#ifndef TL_IO_H
#define TL_IO_H

struct lua_State;

/* Registers io, os, loadfile, dofile, require and package.loaded (tl_env does, for every new state). */
void tl_io_open(struct lua_State *L);

/* Is the value at idx what os.exit raises to end a program (not an error)? */
int tl_io_is_exit(struct lua_State *L, int idx);

#endif
