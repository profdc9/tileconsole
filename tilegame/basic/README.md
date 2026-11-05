# BASIC on the tilegame platform

A self-contained development environment: a command prompt on the 80 x 25 text screen around **MY-BASIC** (Tony Wang, MIT; `my_basic/`, unmodified), with programs as free-form text
files on `tilefs` (the SD card on the board), edited with TDE and run from memory.

    ] LOAD hello       load hello.bas into memory (a name with no extension finds NAME.bas)
    ] LIST             show it, numbered (the numbers are for reading: the file has none)
    ] RUN              run it (Ctrl+C stops it)
    ] EDIT             start the editor (TDE) on it; back at the prompt with the file reloaded
    ] PRINT 2 + 2      anything else is BASIC and runs at once
    HELP CLS NEW LIST [from [to]] RUN LOAD SAVE [file] FILES [folder] KILL CD EDIT VER BYE

`basic_env.[ch]` is the environment (keys, screen and clock through three hooks, so a test can type at it); `basic.c` is the desktop program (`make tgbasic`, `./tgbasic [folder]`,
whose files are /sd; EDIT runs `./tde_ed` on the file); `make test_basic` is the test (typed keys, screen, files; run with AddressSanitizer and UBSan).

## MY-BASIC as it is, and how it is built

The source is MY-BASIC's own, not modified. Two of its settings are given on the command line (`BASICFLAGS` in the Makefile):

- `-Dmb_mem_tag_t=size_t`: it puts a size tag in front of every block it allocates, 16 bits by default, which leaves every block misaligned (undefined behaviour, and on the board's CPU
  some instructions fault); a pointer-sized tag keeps them aligned.
- Its default "compact mode" packs its structures, which GCC handles (members at odd addresses); `-Wno-address-of-packed-member` and the sanitizer's alignment check off say so.

## Things about MY-BASIC that are different from what you may expect

- `PRINT a, b` stays on the line; **a semicolon ends it**: `PRINT a;` prints a line. (`PRINT 2 + 2` at the prompt gets its line ended for you.)
- `:` puts several statements on one line. Strings: `left`, `mid`, `str`, `len`, ... (no `ucase`).
- **Each line typed at the prompt is a program of its own.** Its variables are gone afterwards (an undefined number is 0, an undefined string `(EMPTY)`), and so are a program's after RUN.
  MY-BASIC cannot run a line among the variables of an earlier one: a string variable can point into the text of the line it was assigned in, which is freed when the next line is loaded.
  (Found by running it under AddressSanitizer: `mb_reset` with the variables kept, or a forked interpreter, both end in a use after free.) Use a file for anything longer than a line.
- Printing a list or dictionary prints its type (`LIST`, `DICT`), not its contents.

## Game functions: tiles, a scrolling map, actors, buttons

`basic_game.c` adds functions for making games, on top of tilegfx's tile map (`tg_world`) and sprites (`tg_actor`; the design is in `tilegfx/WORLD.md`). Nothing is shown until the first
`WAITFRAME()`; the picture is the tile screen, and it goes away when the program ends, when the text screen is back and everything the
graphics used is freed. A program that does not use these functions never sees them. The program is a loop:

    gfx(8)
    ... define graphics, make the map and the actors ...
    while btn(0, "b") = 0
      ... read the buttons, move things ...
      waitframe()
    wend

`basic/examples/walk.bas` is a small one: a ball that walks around a walled room, with the camera following it.

**The frame.** Everything between two `WAITFRAME()`s is the next picture: it all appears together, so a door of four tiles is never half open. `WAITFRAME()` shows it, waits for the next
frame (60 a second), moves the actors by their velocities and returns how many frames went by since the last call (1 if the program kept up; the first call returns 1; at most 4).
A program that is slow only looks choppier: velocities are per frame that went by, so the speed stays the same. `FRAMES()` is the frame counter, `MILLIS()` the clock. `TEXTMODE()` puts the text screen up at once, in the middle of a program, for a `PRINT` or an `INPUT`; the graphics are kept, and the next `WAITFRAME()` shows the picture again (when the program ends the text screen comes back by itself).

**Graphics** (everything is 8 bits a pixel: a colour is an entry 0 to 255 in the palette, and entry 0 is see-through)

| function | what it does |
|---|---|
| `gfx(size)` | the size of tiles and sprites, 8 or 16 pixels (8 if you do not say); starts the graphics afresh, so call it first |
| `palette(c, r, g, b)` | colour c from red, green and blue, each 0 to 255. c is one character, such as `"A"`, or a number 0 to 255 for the entry itself (entry 0 is the backdrop) |
| `palette(c, -1, -1, -1)` | makes the character c the see-through one in strings (it is `"."` at first) |
| `tiledef(n, "...")` | map tile n (0 to 255) from a string of 64 characters, row by row (8 pixel tiles; MY-BASIC's strings are at most 128 characters) |
| `tilerow(n, y, "...")` | one row of a tile: 8 or 16 characters. With y = 0 and a string with a character for every pixel of the tile (64 or 256), it is the whole tile, row by row, like `tiledef` (the way to define a 16 pixel tile in one command: join strings with `+`) |
| `tilepix(n, x, y, c)` | one pixel, c is 0 to 255 |
| `sprdef(n, "...")`, `sprrow(n, y, "...")`, `sprpix(n, x, y, c)` | the same for sprite n (the sprite sheet is separate from the map's tiles) |

A colour is named by the character whose code is its palette entry, and the same character draws it in a string: `palette("A", 255, 0, 0)` makes `A` red, and a `tiledef` string with `A`s in it is red there. The characters you can type reach about 94 entries; `tilepix`, `sprpix` and `palette` with a number reach the rest. `.` is see-through (so are the pixels of a tile you have not defined), unless `palette("@", -1, -1, -1)` made `@` the see-through one, when `.` becomes an ordinary colour. At the start the PC's 16 text colours are on `0` to `9` and `A` to `F` (`0` black, `1` blue, `4` red, `F` white), entry 0 is a black backdrop, and every other colour is black. Define the graphics before the loop:
changing a tile while it is on screen can show for a frame as it was and as it is.

**The map** (tiles in a grid, in front of the backdrop)

| function | what it does |
|---|---|
| `newmap(w, h)` | a map of w by h tiles, all tile 0, up to a million tiles; the camera is where it was (0, 0 at first) |
| `tile(x, y, n [, flip])` | the tile at x, y (tile coordinates) is n; flip 1 mirrors it across, 2 down, 3 both |
| `gettile(x, y)`, `tilefill(x, y, w, h, n)` | the tile number there, and a block of one tile |
| `tilestr(x, y, w, s$ [, offset])` | the bytes of the string, plus the offset (0 if not given), are tile numbers put in the map one after the other, w to a row from x, y and then the next row. A string cannot hold a 0, so the offset is how to reach tile 0 (`-32` makes a space tile 0). The tile of every byte must be 0 to 255, or nothing is changed |
| `camera(x, y)` | the pixel at the top left of the screen (any numbers, negative too; outside the map is empty) |
| `camx()`, `camy()` | where the camera is (see `wrap`) |
| `wrap(1)` | the map repeats in every direction, so the scrolling can go on for ever; `wrap(0)` ends it. Returns 1 if the actors wrap too (the map must be at least 192 by 160 pixels) |
| `settileflags(n, f)` | what a tile is, as bits: 1 solid, 2 one-way (solid from above only); 4 trigger (stops nothing, but `actormove` reports it: see below); 8 to 128 are not used yet, so a program may use them for its own marks |
| `tileflag(x, y)` | the flags of the tile at the pixel x, y (floating point is fine) |
| `tilebox(x, y, w, h)` | the flags of all the tiles a box of pixels touches, ORed: one call for what would take a loop |

**Actors** (sprites that have a position in the map and are drawn only while on screen; 128 at most, and the frame can show 32 on a line)

| function | what it does |
|---|---|
| `actor(sprite, x, y)` | a new actor at the pixel x, y; returns its number, or -1 if all 128 are in use |
| `actorfree(a)` | forgets it (the number is used again) |
| `actorpos(a, x, y)`, `actorx(a)`, `actory(a)` | its position in pixels, which can be fractions |
| `actorvel(a, vx, vy)` | pixels a frame that went by, added by `waitframe()` (floating point: 0.25 is a slow crawl) |
| `actormove(a, dx, dy)` | moves it by dx and dy pixels, x first, stopping against solid tiles; returns the sides that stopped it: 1 left, 2 right, 4 up, 8 down (add them), and 16 if its box touched a trigger tile (flag 4) at any point of the move, which does not stop it. A solid tile that is also a trigger does both. An actor that stands on a trigger keeps touching it: every `actormove` reports it, even a move of 0, 0 |
| `actorhit(a, b)` | 1 if the collision boxes of the two overlap (two actors; not a tile) |
| `hitx(a, side)`, `hity(a, side)`, `hittile(a, side)`, `hitflags(a, side)` | the tile that last stopped the actor on that side (1 or `"left"`, 2 `"right"`, 4 `"up"`, 8 `"down"`), or 16 or `"touch"` for the trigger tile it last touched (the first of those the move found, in row order), as it was at that moment: its tile coordinates, its tile number and its flags. A later `tile` that changes the map does not change them, so a program can tell that it hit a door or a spike even if it has since turned it into floor. Where several tiles are along the edge, it is the first (the top one for left and right, the left one for up and down); off the edge of a map it is the border tile that stopped it. They are 0 until the first one of the kind and kept until the next, so use what `actormove` returns to know that this move had one |
| `lastx(a)`, `lasty(a)` | where the actor was before its position last changed (by `actormove`, `actorpos` or its velocity at `waitframe()`; a move to the same place does not count), in pixels. Right after `actormove` says 16 for the first touch of a trigger, this is where it was before the move, which is clear of the trigger, so `actorpos(a, lastx(a), lasty(a))` puts it back, as if the trigger had stopped it (set `actorvel(a, 0, 0)` too if it has a velocity). Once it stands on the trigger, the last position is already inside it, so keep your own copy from before the touch if you need it |
| `actortile(a, sprite)`, `actorsize(a, cols, rows)` | which sprite; a grid of up to 4 by 4 sprites (taken row by row from the number) for a big character |
| `actorflip(a, f)`, `actorhide(a, on)` | mirror it (1 across, 2 down, 3 both); do not draw it |
| `actorbox(a, x, y, w, h)` | the part of it that collides, in pixels from its top left (the whole of it if w or h is 0) |
| `actorprio(a, p)`, `ysort(on)` | who is in front (0 in front, 255 behind; the same: the lower number); with `ysort(1)` the one lower on the screen is in front |
| `actoruser(a)`, `actoruser(a, n)` | a number (0 to 65535) the program keeps in an actor: its kind, or its state |

Actors are in front of the map. With `wrap(1)` an actor's position is kept within the map, and one near the seam is seen on both sides of it.

**Buttons.** `btn(player, button)` is 1 while the button is held, `pressed(player, button)` if it went down since the last frame; players are 0 and 1. The button is its name, `"up"`, `"down"`,
`"left"`, `"right"`, `"a"`, `"b"`, `"x"`, `"y"`, `"start"`, `"select"`, `"l"`, `"r"`, or a number (1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048 in that order; add them for any of several).

A mistake (an actor that is not there, a number out of range, a map that was not made) stops the program with a line that says what, and the function's name.

Not here yet: following an actor with the camera (`camera(actorx(a) - 160, actory(a) - 120)` does it), animation tables, loading tiles and maps from files, more than one tile layer, sound, and a
routine that is called once a frame instead of a loop.

## MY-BASIC things that matter to games

- A function call needs its brackets, also when it is a statement: `waitframe()`, `tile(3, 4, 1)`.
- `for` and `next` (and `while` and `wend`, `if` and `endif`) go on lines of their own: `for i = 1 to 10 : next` on one line is an error.
- Two `print ... ;` on one line print a stray `NIL`: put each on its own line.
- A string in the program is at most 128 characters, hence `tilerow` for 16 pixel tiles (or join strings: `a$ + b$` can be as long as you like, and `tilerow(n, 0, ...)` takes a whole tile).
- Numbers are floating point (24 bits of fraction), so positions are fine up to a million pixels or so; a map that wraps keeps them small.

## Limits

A program is at most `BASIC_PROGRAM_MAX` (64 KB) of text. The prompt line is as long as the screen line allows. MY-BASIC's callbacks carry no pointer, so there is one environment at a time.
