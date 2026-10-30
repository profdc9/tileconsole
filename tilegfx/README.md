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

`tg_font.h` and `tg_fonts.h` add bitmap fonts and the helpers that put them on a tile layer. The fonts are the public-domain
[Unscii](https://viznut.fi/unscii/) variants (not the GPL `unscii-16-full`; see `fonts/FONTS.md`), one source file each so
that only the fonts you name are linked:

- `tg_tile_from_glyph(tile, tile_log2, depth, glyph, fg, bg)` programs a tile from an 8-byte glyph (one byte a row, MSB
  leftmost): set pixels get colour index `fg`, the others `bg` (0 is transparent). Works for 8x8 and 16x16 tiles (a
  16x16 tile doubles every pixel), at 4 or 8 bpp. Any array in that format works, not only these fonts.
- `tg_font_glyph(font, codepoint)` finds a glyph by Unicode codepoint.

Two ways to get from a string to tiles on the map:

1. **Explicit, with a glyph offset** (the low-level pair):

       tg_font_load_range(font, tiles, 3, 4, 0, 32, 95, 0, fg, bg);   /* ASCII 32..126 become tiles 0..94 */
       tg_map_put_string(map, 5, 5, x, y, "Hello", -32, pal);         /* tile = glyph number + offset */

   `tg_font_load_range` moves consecutive glyphs into consecutive tiles (`half` picks the top or bottom 8 rows of a
   16-row glyph, so a tall font is two ranges printed on two map rows). `tg_map_put_string` writes the glyph numbers
   of a string's bytes as tile numbers plus the offset; `tg_map_put_glyphs` does the same for a list of 16-bit numbers
   (above 255, or including 0). The offset is tile number minus glyph number, so a set that starts at space just uses
   -32: the codes 0..31 come out below tile 0 and are skipped (the cell is left alone and the position moves on).
2. **Charset-based**: `tg_font_load` returns a `tg_charset_t` and `tg_text_put` takes UTF-8 text, looks each character
   up in that charset and skips the ones it does not have. Use it when you want accented letters or box drawing from a
   string without working out numbers.

`make demo_text` renders a sample of every font to `fonts.ppm`.

