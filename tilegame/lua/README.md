# Lua on the tilegame platform

A self-contained development environment: a command prompt on the 80 x 25 text screen around **Lua 5.4.9** (Lua.org, PUC-Rio, MIT; `../third_party/lua`, with 32-bit
integers and floats), with programs as text files on `tilefs` (the SD card on the board), edited with TDE and run from memory.

    ] LOAD hello       load hello.lua into memory (a name with no extension finds NAME.lua)
    ] LIST             show it, numbered (the numbers are for reading: the file has none)
    ] RUN              run it (Ctrl+C stops it)
    ] EDIT             start the editor (TDE) on it; back at the prompt with the file reloaded
    ] print(2 + 2)     anything else is Lua and runs at once
    ] 2 + 2            an expression is shown: 4
    HELP CLS NEW LIST [from [to]] RUN LOAD SAVE [file] FILES [folder] KILL CD EDIT VER BYE  (and STACK HEAP VIDEO USBLOG SCRATCH on the board)

`tl_env.[ch]` is the environment (keys, screen and clock through hooks, so a test can type at it); `tl_main.c` is the desktop program (`make tglua`, `./tglua [-2] [folder]`,
whose files are /sd; EDIT runs `./tde_ed` on the file); `make test_lua` is the test (typed keys, screen, files; run with AddressSanitizer and UBSan). On the board the
environment is part of the development kit (`../demo/devkit.c`), with TDE in the same program.

## The prompt

- A command is a word at the start of the line (in any case) followed by nothing, a file name or numbers. Anything else is Lua: `list = 7` and `run()` are Lua, `LIST` and `RUN` are
  commands.
- **Lines typed at the prompt share one Lua state**: `x = 5` on one line and `x * x` on the next gives 25. An expression on its own (or after `=`) has its values printed.
- A statement that is not finished on the line (a `for` without its `end`, a function, a long string) goes on at the next line, with the prompt `>>`; Ctrl+C there gives it up.
- **RUN starts a fresh state** and runs the program in it; what the program leaves (its global variables, its graphics) can be looked at from the prompt afterwards, until the
  next RUN or NEW. An error stops the program with its message and where it was (`walk.lua:12: ...`).
- Ctrl+C stops a running program, also one that is inside a `pcall`, a long computation or a frame loop. `os.exit()` ends a program as its end would.
- `io.read()` reads a line typed at the console (`io.read("n")` a number), `io.write(...)` writes without a new line, `print` writes with one (values separated by tabs, as Lua does).

## Lua as it is here

Lua 5.4 is unchanged except for `LUA_32BITS` (see `../third_party/lua/README-tilegame.md`):

- **Integers are 32 bits and floats single precision.** `math.maxinteger` is 2147483647; an integer that overflows wraps around. A float holds 24 bits, so whole numbers are exact up
  to 16 million: positions in pixels are fine. `/` always gives a float (`7 / 2` is 3.5), `//` rounds down (`7 // 2` is 3).
- **Only `nil` and `false` are false**: 0 is true. The game functions give `true` and `false` for what is yes or no (`btn`, `pressed`, `actorhit`, `animdone`, `wrap`).
- The libraries are `string`, `table`, `math`, `utf8` and `coroutine`, and the base functions. Lua's own `io`, `os`, `package` and `debug` libraries are not there; in their place:

| Function | |
|---|---|
| `io.open(name [, mode])` | a file on tilefs: mode `"r"`, `"w"` (empties it), `"a"` (writes at the end), each with `"+"` to read and write. `nil, message, code` if it cannot be opened |
| `f:read(...)`, `f:write(...)`, `f:lines(...)`, `f:seek([whence [, offset]])`, `f:close()`, `f:flush()` | as in Lua: read formats `"l"`, `"L"`, `"n"`, `"a"` or a count |
| `io.lines(name, ...)` | the lines of a file (closed at the end, or if the loop is left); `io.lines()` reads lines typed at the console |
| `io.read(...)`, `io.write(...)`, `io.type(x)` | the console; `"file"`, `"closed file"` or nil |
| `os.remove(name)`, `os.rename(from, to)` | a file (an empty folder too, for remove) |
| `os.dir([folder])` | what is in a folder: a list of `{name = ..., size = ..., dir = true or false}` |
| `os.clock()`, `os.time()` | seconds since the machine started (a float, and a whole number); there is no calendar |
| `os.exit()` | ends the program (not an error) |
| `require(name)`, `dofile(name)`, `loadfile(name)` | run another Lua file: `require("lib.maths")` runs `lib/maths.lua` once and keeps what it returns in `package.loaded` |

A file that is open cannot be renamed or deleted (tilefs refuses): close it first. Files a program leaves open are closed at the next RUN or NEW.

## Game functions: tiles, a scrolling map, actors, buttons

`tl_game.c` adds functions for making games, on top of tilegfx's tile map (`tg_world`) and sprites (`tg_actor`; the design is in `tilegfx/WORLD.md`). Nothing is shown until the first
`waitframe()`; the picture is the tile screen, and the text screen comes back when the program ends. A program is a loop:

    gfx(8)
    ... define graphics, make the map and the actors ...
    while not btn(0, "b") do
      ... read the buttons, move things ...
      waitframe()
    end

`examples/walk.lua` is a small one: a ball that walks around a walled room, with the camera following it.

**The frame.** Everything between two `waitframe()`s is the next picture: it all appears together, so a door of four tiles is never half open. `waitframe()` shows it, waits for the next
frame (60 a second), moves the actors by their velocities and their animations on, and returns how many frames went by since the last call (1 if the program kept up; at most 4). A
program that is slow only looks choppier: velocities are per frame that went by, so the speed stays the same. `frames()` is the frame counter, `millis()` the clock. `textmode()` puts the
text screen up at once, in the middle of a program, for a `print` or an `io.read`; the graphics are kept, and the next `waitframe()` shows the picture again.

**A routine for each frame.** Instead of a `while ... waitframe() ... end` loop, a program can give the system a function and let it run the loop:

    function update(k)              -- k is the number of frames that went by, as waitframe() returns it
      actormove(p, 2, 0)
      return btn(0, "b")            -- a result that is not nil or false ends the loop
    end
    onframe(update)
    local r = run()

`onframe(f)` takes a function (or the name of one in a string, `"update"`); `onframe(nil)` forgets it. `run()` then loops: draw and wait for the frame as `waitframe()` does, move the
actors, call the function with the frames that went by. It ends when the function returns something that is not `nil` or `false` (`run()` returns it), at an error, or at Ctrl+C.
`waitframe()` inside the loop is an error, and so are `run()` and `onframe()`. `examples/walk2.lua` is `walk.lua` this way. A coroutine makes a game that reads as a straight line:
`local game = coroutine.wrap(function() ... coroutine.yield() ... end)` and `onframe(function(k) return game() end)`.

**Graphics** (everything is 8 bits a pixel: a colour is an entry 0 to 255 in the palette, and entry 0 is see-through)

| function | what it does |
|---|---|
| `gfx(size)` | the size of tiles and sprites, 8 or 16 pixels (8 if you do not say); starts the graphics afresh, so call it first |
| `palette(c, r, g, b)` | colour c from red, green and blue, each 0 to 255. c is one character, such as `"A"`, or a number 0 to 255 for the entry itself (entry 0 is the backdrop) |
| `palette(c, -1, -1, -1)` | makes the character c the see-through one in strings (it is `"."` at first) |
| `tiledef(n, "...")` | map tile n (0 to 255) from a string with a character for every pixel, row by row (64 for 8 pixel tiles, 256 for 16) |
| `tilerow(n, y, "...")` | one row of a tile: 8 or 16 characters. With y = 0 and a string for every pixel of the tile, the whole tile, like `tiledef` |
| `tilepix(n, x, y, c)` | one pixel, c is 0 to 255 |
| `sprdef(n, "...")`, `sprrow(n, y, "...")`, `sprpix(n, x, y, c)` | the same for sprite n (the sprite sheet is separate from the map's tiles) |

A colour is named by the character whose code is its palette entry, and the same character draws it in a string: `palette("A", 255, 0, 0)` makes `A` red, and a `tiledef` string with
`A`s in it is red there. `.` is see-through (so are the pixels of a tile you have not defined), unless `palette("@", -1, -1, -1)` made `@` the see-through one. At the start the PC's 16
text colours are on `0` to `9` and `A` to `F` (`0` black, `1` blue, `4` red, `F` white), entry 0 is a black backdrop, and every other colour is black. Lua joins strings with `..`, and
`string.rep` repeats one, so a big tile can be built: `sprdef(1, string.rep("4", 256))`.

**The map** (tiles in a grid, in front of the backdrop; up to four layers of them, see below)

| function | what it does |
|---|---|
| `newmap(w, h [, layer])` | a map of w by h tiles, all tile 0, up to a million tiles, for the layer (the current one if not given, layer 1 at first); its camera follows the main one. Returns the layer |
| `tile(x, y, n [, flip])` | the tile at x, y (tile coordinates) is n; flip 1 mirrors it across, 2 down, 3 both |
| `gettile(x, y)`, `tilefill(x, y, w, h, n)` | the tile number there, and a block of one tile |
| `tilestr(x, y, w, s [, offset])` | the bytes of the string, plus the offset (0 if not given), are tile numbers put in the map one after the other, w to a row from x, y and then the next row (`"\0"` is tile 0). The tile of every byte must be 0 to 255, or nothing is changed |
| `camera(x, y)` | the main camera: the pixel of the solid layer at the top left of the screen (any numbers, negative too; outside the map is empty). The other layers follow it (`parallax`) |
| `camx()`, `camy()` | where the camera is (see `wrap`) |
| `follow(a [, dead_w, dead_h [, smooth]])` | from now on the camera follows actor `a` each frame, keeping the middle of the actor in the middle of the screen. A dead zone (`dead_w` by `dead_h` pixels in the middle of the screen) lets the actor move inside it without the camera moving; `smooth` n makes it cover 1/(n+1) of the way each frame (0: all of it). A map that does not repeat keeps the camera inside it (a map smaller than the screen is centred), one that repeats goes the short way round the seam. `follow(nil)` stops it, as does freeing the actor |
| `followlimit(on)` | whether `follow` keeps the camera inside a map that does not repeat (true at first) |
| `wrap(on)` | the current layer's map repeats in every direction (`true`), so the scrolling can go on for ever, or not. On the solid layer, returns true if the actors wrap too (the map must be at least 192 by 160 pixels) |
| `settileflags(n, f)` | what a tile is, as bits: 1 solid, 2 one-way (solid from above only), 4 trigger (stops nothing, but `actormove` reports it); 8 to 128 are the program's own marks |
| `tileflag(x, y)` | the flags of the tile at the pixel x, y |
| `tilebox(x, y, w, h)` | the flags of all the tiles a box of pixels touches, ORed: one call for what would take a loop |

A number with a fraction where a whole one is wanted (a tile position, a sprite number) is rounded down, so `tile(x / 8, y / 8, 3)` puts the tile under the pixel x, y.

**Layers.** The picture has up to four tile layers, 1 at the back and 4 in front, each with a map of its own; a pixel of colour 0 (`.` in a graphics string)
lets the layers behind it show through, down to the backdrop. They all use the one set of 256 tiles and its flags. A layer costs only its map and two small
windows (8 KB of fast memory with 8 pixel tiles), so use as many as the picture needs: a far background, a near one, the level, a panel that stays put.

| function | what it does |
|---|---|
| `layer(n)` | the map functions (`tile`, `gettile`, `tilefill`, `tilestr`, `tileflag`, `tilebox`, `wrap`, and `newmap` without a layer) work on layer n from now on (1 at first). Returns the one before; `layer()` the one now |
| `parallax(n, fx [, fy [, ox, oy]])` | layer n's camera is the main camera times fx across and fy down (fy is fx if not given), plus ox, oy pixels: 1 moves with it (as at first), 0.5 at half its speed (far away), 0 not at all (a panel that stays on the screen, at ox, oy) |
| `solidlayer(n)` | the layer whose tiles stop the actors (`actormove`), whose camera `camx()` and the actors' positions go by, and whose map `follow` keeps the camera inside (1 at first). Give it a parallax of 1. Returns the one before; `solidlayer()` the one now |
| `actorlayer(a, n)`, `actorlayer(a)` | actor a is drawn in front of layers 1 to n and behind the others (0: behind them all; 4, in front of them all, at first) |

    newmap(64, 32, 1); parallax(1, 0.5)         -- far away: half speed
    newmap(80, 60, 2); solidlayer(2)            -- the level
    newmap(40, 4, 3); parallax(3, 0, 0)         -- a panel at the top of the screen
    layer(3); tilestr(0, 0, 40, "...")          -- (the map functions now draw on layer 3)
    actorlayer(p, 2)                            -- the player is behind the panel

**Actors** (sprites that have a position in the map and are drawn only while on screen; 128 at most, and the frame can show 32 on a line)

| function | what it does |
|---|---|
| `actor(sprite, x, y)` | a new actor at the pixel x, y; returns its number, or `nil` if all 128 are in use |
| `actorfree(a)` | forgets it (the number is used again) |
| `actorpos(a, x, y)`, `actorpos(a)`, `actorx(a)`, `actory(a)` | its position in pixels (fractions are kept); `actorpos(a)` gives `x, y`. A whole number of pixels comes back as an integer |
| `actorvel(a, vx, vy)`, `actorvel(a)` | pixels a frame that went by, added by `waitframe()` (0.25 is a slow crawl) |
| `actormove(a, dx, dy)` | moves it by dx and dy pixels, x first, stopping against solid tiles; returns the sides that stopped it: 1 left, 2 right, 4 up, 8 down (added), and 16 if its box touched a trigger tile (flag 4) at any point of the move, which does not stop it. Test a side with `h & 2 ~= 0`. A tile that is both solid and a trigger stops it like any solid tile, and since the actor stops against it without overlapping it, that is not reported as a touch. An actor that stands on a trigger keeps touching it: every `actormove` reports it, even a move of 0, 0 |
| `actorhit(a, b)` | true if the collision boxes of the two overlap (two actors; not a tile) |
| `hitx(a, side)`, `hity(a, side)`, `hittile(a, side)`, `hitflags(a, side)` | the tile that last stopped the actor on that side (1 or `"left"`, 2 `"right"`, 4 `"up"`, 8 `"down"`), or 16 or `"touch"` for the trigger tile it last touched, as it was at that moment: its tile coordinates, its tile number and its flags. A later `tile` that changes the map does not change them. Where several tiles are along the edge, it is the first (the top one for left and right, the left one for up and down). They are 0 until the first one of the kind and kept until the next, so use what `actormove` returns to know that this move had one |
| `lastx(a)`, `lasty(a)` | where the actor was before its position last changed (by `actormove`, `actorpos` or its velocity at `waitframe()`). Right after `actormove` says 16 for the first touch of a trigger, this is where it was before the move, clear of the trigger, so `actorpos(a, lastx(a), lasty(a))` puts it back as if the trigger had stopped it |
| `actortile(a, sprite)`, `actortile(a)`, `actorsize(a, cols, rows)` | which sprite; a grid of up to 4 by 4 sprites (taken row by row from the number) for a big character |
| `actorflip(a, f)`, `actorhide(a, on)` | mirror it (1 across, 2 down, 3 both); do not draw it |
| `actorbox(a, x, y, w, h)` | the part of it that collides, in pixels from its top left (the whole of it if w or h is 0) |
| `actorprio(a, p)`, `ysort(on)` | who is in front (0 in front, 255 behind); with `ysort(true)` the one lower on the screen is in front |
| `actoruser(a)`, `actoruser(a, n)` | a number (0 to 65535) the program keeps in an actor. (A Lua table keyed by the actor's number holds anything else: `enemies[a] = {hp = 3}`) |
| `animdef(n, mode, ticks, frames)` | table n (1 to 32) of sprites to show one after the other, `ticks` frames each (1 to 255): a string, `"2 1 3 1"`, where `"2:10"` gives one its own time, or a Lua table, `{2, 1, 3, 1}` or `{{2, 10}, 1}`; 32 frames at most. The mode is `"loop"` (or 0), `"once"` (1: stops on the last frame) or `"pingpong"` (2). Returns the number of frames; a table made again replaces the old one, also for actors that play it |
| `actoranim(a, n [, restart])`, `actoranim(a)` | the actor plays table n from now on (0: none; its sprite stays as it is). `waitframe()` and `run()` move every playing animation on by the frames that went by. Asking for the table it is already playing does nothing, so a program can say `actoranim(p, 2)` every frame while the key is down; with `restart` true it starts over. Without n it returns the table playing (0: none) |
| `animdone(a)`, `animframe(a)` | true once a `"once"` animation has shown its last frame for its whole time; the frame the animation is on, from 0 |

Actors are in front of the map (of every layer, unless `actorlayer` says otherwise). With `wrap(true)` on the solid layer an actor's position is kept within the map, and one near the seam is seen on both sides of it.

**Buttons.** `btn(player, button)` is true while the button is held, `pressed(player, button)` if it went down since the last frame; players are 0 and 1. The button is its name, `"up"`,
`"down"`, `"left"`, `"right"`, `"a"`, `"b"`, `"x"`, `"y"`, `"start"`, `"select"`, `"l"`, `"r"`, or a number (1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048 in that order; add them
for any of several).

A mistake (an actor that is not there, a number out of range, a map that was not made) stops the program with a line that says what, the function's name and the program line.

`examples/layers.lua` uses all four layers.

Not here yet: loading tiles and maps from files.

## Sound

`tl_sound.c` plays through tilesnd (`../../tilesnd`), which mixes 16 voices at 48000 samples a second on the machine's audio thread (on the board core 1, into the HDMI
audio). The voices are numbered 1 to 16; each plays one note at a time: a tone (a pulse wave, a triangle, two kinds of noise, or a 32-step wavetable) or a sample, through
its envelope, at its volume and its place between left and right. A program uses as many voices as it likes, for anything: one for the jump, one for the coins, three for
the music.

    envelope(1, 0, 200, 0, 20)     -- a blip: gone in 200 ms
    tone(1, "pulse", 72)           -- C5 (60 is middle C)
    tone(2, "triangle", "E3", 500) -- E3 for half a second
    slide(1, 84, 100)              -- up an octave in 100 ms

| function | what it does |
|---|---|
| `tone(v, kind, note [, ms])` | a tone on voice v. kind: `"pulse"` (a square wave, or thinner, see `duty`), `"triangle"`, `"noise"` (a hiss), `"buzz"` (a short-cycle noise: metallic), `"wave"` (the voice's wavetable, see `wave`). The note is a number (MIDI's: 60 is middle C, 69 the A of 440 Hz, 61 the C sharp above middle C; a fraction is between two notes) or a name: `"C4"`, `"F#3"`, `"Bb5"` (an octave of 4 if none is given). For the noises the note is how fast they are clocked: higher is brighter. The tone holds until `release(v)`, or is let go after ms |
| `play(v, n [, speed [, ms]])` | sample n on voice v. speed 1 is as it was recorded, 2 an octave up, 0.5 an octave down. It ends by itself at the end of the sample, unless the sample loops |
| `release(v)` | lets go of the note: it fades out over the envelope's release time |
| `silence([v])` | stops voice v at once, or every voice |
| `playing(v)` | true while voice v makes sound (also a note that was started and has not reached the audio thread yet) |
| `envelope(v [, attack, decay, sustain, release])` | how the voice's notes rise and fall: in `attack` ms to full, in `decay` ms down to `sustain` (a level, 0 to 255), held there until the note is let go, then in `release` ms to nothing. With sustain 0 a note ends by itself once it has decayed: a blip or a drum, with no `release` needed. `envelope(v)` is the default again: full at once, held, 5 ms to fade |
| `volume(v, level)`, `pan(v, p)`, `duty(v, d)` | the voice's volume (0 to 255; 255 at first), its place from left to right (-128 to 127; 0, the middle, at first), and the pulse's duty (0 to 255: 128, at first, is a square wave, 64 thinner, 32 thinner still). They change the note playing and the ones after it |
| `pitch(v, note)` | the tone's note now (a sample: its speed) |
| `slide(v, note, ms)` | from the note the voice has to this one, evenly in semitones, over ms (a sample: to the speed) |
| `wave(v, shape)` | the voice's wavetable, for `"wave"` tones: `"sine"` (at first), `"triangle"`, `"saw"`, `"square"`, a table of 32 numbers from -1 to 1, or a string of 32 bytes (as `sample` takes them). A note playing changes at once |
| `mastervolume(level)` | all of the sound, 0 to 255 |
| `note(name)`, `hz(f)` | the number of a note's name (`note("A4")` is 69), and the note of a frequency (`hz(440)` is 69, `hz(450)` about 69.39) |
| `sample(n, data, rate [, first, last])` | makes sample n (1 to 32) from a string, one byte a sample (0 to 127, then 128 to 255 for -128 to -1: `"\0\127\0\129"`), or from a table of numbers from -1 to 1, recorded at `rate` samples a second (100 to 200000). With first and last, which count from 1 as `string.sub` does, those samples repeat after the rest has played, until `release(v)`. Making a sample again stops the voices that play it. Returns its length |
| `loadsample(n, file [, first, last])` | the same from a WAV file: 8- or 16-bit PCM, mono or stereo (made mono), at any rate. Returns its length and its rate |

A voice's settings (volume, pan, duty, envelope, wavetable) stay until they are changed, or until RUN or NEW. Ctrl+C stops every voice, in a program or at the prompt, and
so do RUN, NEW, EDIT and BYE; RUN and NEW also put every voice back as it was at first and give the samples' memory back. A program that ends, even with an error, leaves
what it started playing: a note that holds sounds on at the prompt until `release`, `silence()` or Ctrl+C. Lines typed at the prompt can play too.

Slides and notes with a length move on whenever the environment looks at the keys: at each frame in `waitframe` and `run`, every 16 ms in a long computation, and at the
prompt; so they are smooth to within a frame. A note reaches the speakers a few ms after the call (on the board about 17 ms: core 1 keeps that much ahead of the HDMI
output). On a machine without sound (the desktop with no audio device) the functions do nothing and `playing` is false; samples are still made, so a program runs the
same. On the board the samples are in the PSRAM.

`examples/sound.lua` plays one sound for each button.

### Music

Music is written in MML (Music Macro Language): a string of notes for each track, as BASIC's `PLAY` had them, several tracks at once, each on a
voice of its own and each with its instruments. The audio thread plays it by itself (tilesnd's sequencer, `../../tilesnd/snd_mml.c`), to within
1.3 ms, whatever the program is doing: it goes on while a file loads, and at the prompt after the program has ended.

    instrument(1, "pulse", {duty = 64, decay = 300, sustain = 120})
    instrument(2, "triangle", {volume = 230})
    music("t140 L @1 o5 l8 e g >c< g e4 g4 | f a >c< a f4 a4",
          "L @2 o3 l2 c c | f f")

| function | what it does |
|---|---|
| `music([first voice,] track, ...)` | plays the tracks (1 to 16 strings of MML), in place of any music that was playing: the first track on `first voice` and the others on the voices after it; without it, on the last voices (four tracks on 13 to 16), which leaves the first ones to the program's sound effects. A mistake in the MML stops the program with the track and the place: `music: track 2, at 14: 'x' is not an MML command`. `music()` stops the music |
| `instrument(n, kind [, settings])` | instrument n (0 to 31) for the music's `@n`: kind as `tone`'s, or `"sample"`; settings is a table of any of `duty`, `attack`, `decay`, `sustain`, `release` (as `envelope`'s), `volume` (0 to 255, times the track's `v`), `pan`, `wave` (as `wave`'s), `sample` (its number) and `root` (the note at which the sample plays as it was recorded; `"C4"`, 60, if not given). Instrument 0, and any not made, is a square wave at full volume, held to the note's end. Music uses the instruments as they were when `music` was called |
| `musicplaying()` | true while the music plays; music that loops plays until it is stopped |
| `musicvolume(level)` | the music's volume, 0 to 255, from its next notes on (255 at first) |

The MML (commands in lower or upper case, except `l` and `L`; spaces, new lines and `|` bar lines are left out):

| MML | what it is |
|---|---|
| `c d e f g a b` | a note, then `+` or `#` (sharp) or `-` (flat), then its length: `4` a quarter note, `8` an eighth, `2` a half, `1` a whole ... (any number that divides 384, so `3`, `6`, `12` for triplets), or `%n` for n ticks (96 to a quarter note); then dots, each adding half of what the one before added (`4.` is a quarter and an eighth). No length: `l`'s |
| `r` | a rest, its length the same way |
| `n60,8` | a note by its MIDI number (60 is middle C); the length after a comma, or `l`'s |
| `o4`, `>`, `<` | the octave (0 to 9; `o4` has middle C, and is the octave at first), one octave up, one down |
| `l8` | the length of the notes and rests that have none (a quarter at first) |
| `^8` | adds to the length of the note or rest before it (`c4^16`) |
| `&` | between two notes: the same pitch makes one longer note; another pitch slurs to it, with no new attack |
| `t120` | the tempo, quarter notes a minute (20 to 600; 120 at first); it is for all the tracks, from where it is |
| `v15` | the track's volume, 0 to 15 (15 at first) |
| `q8` | how much of its length a note sounds before it is let go, in eighths (8, all of it, at first; `q4` half: staccato) |
| `@1` | the instrument (0 at first) |
| `p-64` | the track's pan, -128 (left) to 127 (right); until it has one, the instrument's |
| `k-12` | transposes the notes after it, -48 to 48 semitones |
| `[ ... ]3` | repeats what is inside 3 times (2 with no number); they nest, up to 8 deep. A repeat is kept as a loop in the compiled music, so it takes no more memory however many times it goes round (unless its passes come out different, as `[c >]3`'s do: then it is written out) |
| `L` | where the track starts again when it comes to its end: the music loops for ever. Without one the track ends |

Each track loops on its own, so tracks that loop should be as long as each other. A track's settings (octave, length, volume, instrument ...) go on from
one note to the next and through the repeats; `t` changes the tempo of every track. The music's voices are the program's voices too: a sound effect
played on one of them is cut off by the track's next note, so effects belong on the others. `silence()`, Ctrl+C, RUN and NEW stop the music;
making a sample again that an instrument of the music plays stops it too. `examples/music.lua` is eight bars on four tracks.

## Speed (board)

`examples/bench.lua` measures what things cost. Lua compiles a program to bytecode once and allocates nothing for arithmetic, calls or table fields (only for new strings), so on the
board a statement costs well under a microsecond: a 16.7 ms frame holds tens of thousands of them. Keep variables `local` (a global is a table lookup, several times slower).

How a running program is made to look at the keys: a timer (on the board an interrupt, on the desktop SDL's timer thread) calls `tl_env_tick()` every 16 ms, which sets a Lua hook
for one instruction; the hook looks at the keys (and, with the tile screen up, keeps the board's hang monitor and the USB host going), then takes itself away. A hook that stayed on
would make every instruction about twice as slow. Without such a timer (`async_ticks` 0, as in the tests) the environment counts instructions instead.

On the board Lua's small blocks (up to 512 bytes) are in SRAM, up to 64 KB; anything bigger, and anything beyond that, is in the PSRAM (`lua_alloc` in `../demo/devkit.c`). `HEAP` shows
how much is in SRAM. The share is a build setting, `cmake -DLUA_SRAM_KB=n` (0: all of Lua's memory in the PSRAM). Measured with `bench.lua`: with all of it in the PSRAM, ordinary
code and calls were only 2-6% slower (the 16 KB cache holds what a loop keeps using), but making strings was 2.7 times slower (every allocation goes through the PSRAM pool) and
`tile()` 2.9 times. Putting the game's own state (the actors) in SRAM as well made no measurable difference, so it stays in the PSRAM: SRAM is what the graphics need.

The allocators themselves, measured on the board on 2026-11-26 with a throwaway test (not in the tree): nanoseconds per free and malloc of a block of 16 to 72 bytes, the
test loop's own time taken off. "Live" is how many blocks stay allocated while one at random is freed and a new one made (256 is about 12 KB, which fits the cache; 2048 about
100 KB); "touched" also reads each block before freeing it and writes each new one, as Lua does with its objects.

| allocator | malloc, free at once | 256 live | 2048 live | 2048 live, touched |
|---|---|---|---|---|
| C heap in SRAM, through the SDK's malloc mutex (Lua's small blocks now) | 732 | 745 | 720 | 719 |
| C heap in SRAM, newlib's malloc called directly (no mutex) | 320 | 333 | 310 | 308 |
| dlmalloc pool on a block of SRAM | 260 | 300 | 284 | 283 |
| dlmalloc pool in the PSRAM (Lua's bigger blocks now) | 262 | 455 | 1447 | 3927 |
| size classes (a free list per 8 bytes, no headers) in SRAM | 25 | 25 | 25 | 24 |
| size classes in the PSRAM | 26 | 27 | 182 | 2512 |

The SDK's mutex costs more than malloc and free themselves; newlib's malloc and dlmalloc (the same algorithm) cost the same in SRAM; in the PSRAM dlmalloc is 5 times slower once
the blocks no longer fit the cache, and reading and writing the objects there costs about 2.5 µs each whatever the allocator. Allocation is about 1.4 µs of the 8.9 µs a string
concatenation takes, and game code seldom allocates every frame, so it was left as it is. If it ever matters, the choices are:

- Call newlib's malloc directly from `lua_alloc` (310 ns). Nothing is reserved, but it is only safe while core 0's main loop is the only thing that allocates (today core 1
  runs only the HDMI loop, and the interrupts do not allocate).
- Take the 64 KB share from the heap at start-up and run a dlmalloc pool on it (284 ns, no mutex). The 64 KB is then the share's even when Lua does not use it (now the share is
  only a limit, and what Lua does not use is free for the rest).
- Size classes (25 ns): not chosen. Lua's blocks come in dozens of sizes (strings of every length, the compiler's arrays, a function's code), each size in use holds at least
  one page, and a page comes back only when every block in it is free, so the 64 KB would fill with half-empty pages.

## Losing the picture (board)

The board watches its own picture: core 1 counts frames every quarter second, and a rate that is not 60 Hz twice in a row means the HDMI output has lost
sync (the TV shows "no signal" while the program goes on). It writes down what it can see, restarts the output, and the picture comes back. `VIDEO` at the
prompt shows how many times that has happened and saves the details in `VIDEO.LOG` on the card (the newest event first). Send that file with a report of a lost picture.

## Limits

A program is at most `TL_PROGRAM_MAX` (64 KB) of text (a program can `require` more from other files). The prompt line is as long as the screen line allows, and a statement typed
over several lines at most `TL_CHUNK_MAX` (4000) characters. There is one environment at a time (Lua's hook carries no pointer of its own).
