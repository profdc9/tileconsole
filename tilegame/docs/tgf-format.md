# TGF: the tile game's graphics and map files

One file format for what the game layer holds (`lua/tl_game.c`): the palette, the tiles and sprites, the tiles' flags, the animation
tables, the character table (which tiles are which letters) and the maps of the layers. The sprite and tile editor writes it; `loadgfx` and `loadmap` read it (the Lua functions at the end).
A file can hold any part of that, or all of it: a sheet of tiles, one level's map, or a whole level with its graphics.

The code is `../src/tgf.c` (`tgf.h`): checking, reading and writing through I/O functions the caller gives, with no Lua and no file
system of its own, so a game in C can use it too. `../tests/test_tgf.c` tests it (round trips, the file cut short at every byte, every
length one off, every range one past its end, random damage under AddressSanitizer); `../tests/test_lua.c` tests the Lua functions.

## Layout

All numbers are little-endian; there is no padding. A file is a list of chunks, each of them:

| Bytes | |
|---|---|
| 4 | `TGAC`, which begins every chunk |
| 4 | the chunk's tag: four ASCII characters, such as `TILE` (a space where the name is shorter: `MAP `, `END `) |
| 4 | the length of the payload in bytes (u32) |
| length | the payload |

So a reader always finds `TGAC` right after a chunk's payload: if it does not, a length is wrong, and the file is broken.

The first chunk is the header, `TGF1`; the last is `END `, with no payload. In between, the others in any order; all of them are
optional, and `MAP ` and `ANIM` come once for each layer and table they hold.

The header of a file of 16-pixel tiles, and the end:

    54 47 41 43  54 47 46 31  04 00 00 00  10 00 00 00      TGAC TGF1, 4 bytes: tile size 16, three zero bytes
    ...
    54 47 41 43  45 4E 44 20  00 00 00 00                   TGAC END , 0 bytes

## Chunks

| Tag | Payload |
|---|---|
| `TGF1` | u8 tile size (8 or 16: what `gfx(size)` chose), 3 zero bytes |
| `PALT` | u16 first, u16 count, then count × 3 bytes: red, green, blue (0 to 255 each) of palette entries first, first + 1, ... |
| `TILE` | u16 first, u16 count, u8 depth (8), 3 zero bytes, then count tiles of size × size bytes each, row by row from the top, one byte a pixel: a palette entry, 0 is see-through |
| `SPRT` | the same as `TILE`, for the sprite sheet |
| `FLAG` | u16 first, u16 count, then count bytes: the flags of tiles first, first + 1, ... (1 solid, 2 one-way, 4 trigger; the other bits are the program's) |
| `ANIM` | u8 table (1 to 32), u8 mode (0 loop, 1 once, 2 pingpong), u8 frame count (0 to 32), 1 zero byte, then for each frame: u16 sprite, u8 ticks (frames it is shown), 1 zero byte |
| `CHAR` | u16 count (0 to 512), 2 zero bytes, then count entries of 8 bytes: u32 character (a Unicode codepoint, 0 to 0x10FFFF), u16 its tile (0 to 255), u16 the tile below it for a character two tiles tall (0 to 255; 0xFFFF for one tile tall). In rising order of character, each once |
| `MAP ` | u8 layer (1 to 4), u8 flags (bit 0: the map wraps), 2 zero bytes, u32 width, u32 height, then width × height u16 entries, row by row from the top: bits 0 to 9 the tile, 10 flipped across, 11 flipped up and down, 12 to 15 zero |
| `END ` | nothing |

The ranges (`first`, `count`) let a file fill part of a set, so that files add up: a font in tiles 200 to 255 from one file, a
level's own tiles below them from another.

Sizes: 256 tiles of 8 × 8 pixels are 16 KB, of 16 × 16 pixels 64 KB; a 64 × 64 map is 8 KB.

## Reading

- The file begins with `TGAC` `TGF1`, or it is not one of these files.
- After each payload comes `TGAC` (or, after `END `, the end of the file). Anything else: the file is broken. A file that stops before
  its `END ` chunk was cut short.
- A payload shorter than the fields of its chunk: broken. Bytes after the fields are skipped, so that any chunk, the header too, can
  have fields added at its end; a reader of today reads the fields it knows.
- A chunk whose tag the reader does not know is skipped (its length says how far).
- A character table out of order, or with a character twice, is broken.
- What does not fit is an error, not something cut to fit: `first + count` over 256, a table outside 1 to 32 or with more than 32
  frames, a layer outside 1 to 4, a map bigger than the game allows (2^20 tiles), a tile or sprite number over 255 (the game layer's
  sets have 256), a character table of more than 512 or a character past 0x10FFFF, a depth other than 8 (4 is kept for later), a tile size other than the game's.
- Nothing is changed until the whole file has been read and checked, so a broken file leaves the game as it was.

## Writing

The header first, `END ` last. A set is written from its first tile to its last one that is not empty (all zero), so a sheet of 20
tiles stays small; the palette all 256 entries; a table only if it has frames; the character table if it has any characters (those in
working tiles, which files do not hold, left out); a map for each layer that has one.

## Lua

| Function | |
|---|---|
| `loadgfx(file)` | the palette, tiles, sprites, flags, animation tables and character table of the file (its maps are left alone; the file's characters join those the game has, replacing the same ones). With no graphics yet, `gfx(size)` with the file's tile size first; graphics of another size: an error |
| `loadmap(file [, layer])` | the maps of the file, each into its layer, made at its size (as `newmap`); with a layer, the file's one map goes into that layer |
| `savegfx(file)` | the palette, tiles, sprites, flags, tables and character table now |
| `savemap(file [, layer])` | the maps of all the layers that have one, or of the one layer |

`savegfx` and `savemap` write separate files; a file with graphics and maps together (which the C functions can write) is read by both
loaders, each taking its own chunks. Saving goes into a new file that then takes the old one's place, so a failure leaves the old file as
it was. A file in a zip archive (a cartridge) loads like any other.
