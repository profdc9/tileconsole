# tilegfx

A scanline compositor for a retro-console style game system: scrolling tile layers, sprites and an optional
320x240 bitmap layer. Plain C, no dependencies, so the same code runs in a desktop program and on a microcontroller.

- 320x240, 8-bit palette indices (index 0 transparent), 256-entry RGB565 palette.
- Up to 4 layers drawn back to front; each is a tile layer or a bitmap. Sprites sit between layers (`above`); sprites come from `tg_sprite_alloc` and are drawn in priority order (`tg_sprite_set_priority`, lower is in front, ties by slot), so the per-line search covers only allocated sprites, front-most first.
- Tile layers: 8x8 or 16x16 tiles, 4 or 8 bits per pixel, a power-of-two tile array that wraps, pixel scroll
  position (the fine offset is derived from it), optional per-line x offset, h/v flip per tile.
- `tg_render_line()` outputs one HDMI line: 320 words, two identical RGB565 pixels each.
- A line callback runs before each line for raster effects.

Any tile size and depth combination is supported; 16x16 at 8 bpp is 256 bytes a tile, so keep an eye on SRAM.

`make check` runs the headless tests (scrolling and wrap-around in all four tile modes, flips, sub-palettes, sprite
clipping and order, layer order, per-line scroll). `make demo_sdl` builds the SDL2 demo; `./demo_sdl --bench`
times the compositor (desktop numbers only). MIT licensed (see `LICENSE`).

## Text

`tg_font.h` adds bitmap fonts and the functions that put text on a tile layer. The fonts are the public-domain
[Unscii](https://viznut.fi/unscii/) variants (not the GPL `unscii-16-full`; see `fonts/FONTS.md`).

A layer remembers how glyph numbers map to tiles (`tg_layer_t.glyphs`: the offset, the range of glyphs that were loaded,
and for tall fonts the distance to the lower half), so printing needs only the string.

**Built-in fonts, by number** (the offset is worked out and stored for you):

    tg_layer_load_font(&layer, TG_FONT_UNSCII8, 32, 95, 100, fg, bg);   /* glyphs 32..126 -> tiles 100..194; offset 68 */
    tg_layer_put_string(&layer, 2, 3, "Hello", 0);                      /* 'H' is 72, so tile 72 + 68 = 140 */

**Your own glyph array** (load by pointer, then set the offset with its own call):

    tg_layer_load_glyphs(&layer, my_glyphs, 8, 0, 64, 200, fg, bg);     /* 64 glyphs of 8 rows -> tiles 200..263 */
    tg_layer_set_glyph_offset(&layer, 200 - 32);                        /* glyph number 32 is the first of them */
    tg_layer_put_string(&layer, 2, 3, "Hello", 0);

Glyph numbers without a tile (outside the loaded range, or a tile number below 0 or above 1023) are skipped: the cell is left
as it was and the position still moves on, so a string that mixes scripts takes one load and one put per script.
`tg_layer_put_utf8` decodes UTF-8 first (so `"é"` or `"─"` can be written as they are), `tg_layer_put_glyphs` takes 16-bit
glyph numbers. An 8x16 font takes two tiles a glyph and fills two map rows. Colour index 0 is transparent (give
`bg` as 0 for text with a see-through background). The layer's `map` and `tiles` are written to, so they must be RAM.

`tg_tile_from_glyph` programs a single tile from an 8-byte glyph; `tg_font_load_range` and `tg_map_put_string` are the same
steps without a layer. Font numbers go through `tg_fontlist.c`, which refers to every font: define `TG_FONTS_ENABLED` (a
mask, bit n for font n) to keep unwanted ones out of a program, or use `tg_layer_load_font_ptr` with the one font object.

`make demo_text` renders a sample of every font to `fonts.ppm`.

## Text mode

`g.mode = TG_MODE_TEXT` shows an 80 x 25 text screen instead of the tile layers: 640 x 400 pixels in the middle of the 640 x 480 picture, with
8 x 16 character cells like the IBM PC. A cell is a character code and an attribute (foreground colour in the low nibble, background in the
high one, each one of 16). The 256 glyphs are kept in RAM, so they can be redefined (always 8 x 16), and so are the 16 palette entries, which
start as the standard PC colours. Everything is in a `tg_text_t` (8 KB: cells, font, palette); `tg_text.h` has the helpers
(`tg_text_put_string`, `tg_text_scroll_up`, `tg_text_set_glyph`, ...) and `tg_text_load_cp437` fills the glyphs with the PC character set
from the Unscii 8x16 font (four characters Unscii lacks are drawn in `tg_text_cp437.c`). The platform layer copies the whole screen in
`plat_present`, so it can be changed at once. `tg_display_lines`, `tg_display_line_shift` and `tg_render_display_line` give a backend the
picture in either mode, one line at a time. `make test_text` checks every line of random screens against a pixel-by-pixel reference.

The cursor is the PC's: a position, a shape (a range of the 16 scanlines of its cell, an underline by default, a block for 0 to 15), a visible
flag and a blink flag (`tg_text_set_cursor`, `tg_text_set_cursor_shape`, `tg_text_show_cursor`, `tg_text_set_cursor_blink`). It is drawn in the
foreground colour of its cell and is hidden until shown. It blinks by `tg_t.frame`, which the platform layer sets for every frame it draws, so
it goes on blinking while the program does nothing: on for 16 frames, off for 16.

## Console

`tg_console.h` writes to a text screen the way a terminal does: `tg_console_puts`, `tg_console_putc` and `tg_console_printf` move a position along,
with `\n`, `\r`, `\b`, `\t` acting as controls, wrapping at the right edge (put off until the next character, so an 80 character line followed by `\n`
leaves no blank line, or dropped with `TG_CONSOLE_NOWRAP`) and scrolling at the bottom. The text screen's cursor follows the position. The
`printf` work space is in the console (not on the stack), so one call writes at most 255 characters. `make test_console` compares random output against
an independent model of a terminal.

With `TG_CONSOLE_ANSI` set in `flags` the console also interprets ANSI/VT100 escape sequences, in `printf` output too: cursor moves and positioning,
erase in display and line, insert and delete line, delete characters, a scroll region (`ESC[t;br`), save and restore the cursor, index and reverse
index, and colours (`ESC[...m`: the ANSI colours are translated to the PC's, bold is the bright foreground, blink the bright background, plus 90-97
and 100-107, 39 and 49). `TG_CONSOLE_LF_ONLY` makes `\n` a plain line feed, as on a terminal. See `tg_console.h` for the full list. It is off by
default, so `printf` users see no change. `make test_ansi_oracle` compares it with Daniel Marks' `TNTSCAnsi` (the interpreter of PicoParanoia, zlib
licence, copied to a scratch directory for the test and not modified) on the sequences the two share: 1.3 million comparisons of the whole screen
and cursor, over 30000 random sequences. Where `TNTSCAnsi` differs on purpose (the cursor position after `ESC 8`, a zero as a count, the cursor
after `ESC[r`, a line feed with the cursor outside the scroll region, a character in the last column), the console follows the VT100 instead.
