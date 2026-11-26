# Lua 5.4.9, vendored with one change

The `src/` files of Lua 5.4.9 (Lua.org, PUC-Rio; MIT licence, see `LICENSE`), without the stand-alone interpreter and compiler
(`lua.c`, `luac.c`). Taken from the `lua-src` crate 551.0.2, which carries the release unchanged.

Change: `luaconf.h` sets `LUA_32BITS` to 1, so integers are 32 bits and floats are single precision, which the RP2350's FPU does in
hardware (doubles and 64-bit integers would go through software). An integer that overflows wraps around, as Lua's integers always do.

Not built (see `lua/tl_env.c`, which opens the libraries itself): `liolib.c`, `loslib.c`, `loadlib.c`, `ldblib.c` and `linit.c`. The files
of the card are reached through tilefs instead (`lua/tl_io.c`: `io.open`, `io.lines`, `os.remove`, `os.rename`, `require`, `dofile`,
`loadfile`), so they work the same on the board and in the desktop builds, and a program cannot run other programs or leave the
environment.
