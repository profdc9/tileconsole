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
