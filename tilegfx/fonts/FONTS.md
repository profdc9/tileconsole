# Fonts

The C files in this directory are generated from **Unscii** by Viznut (https://viznut.fi/unscii/), a set of bitmapped
Unicode fonts for character-cell art.

- Source: https://github.com/viznut/unscii, commit `257b4dfeb50651510fe8b9cf4f151a148a4490e4` (2021-02-24, "clarify
  license"), the `fontfiles/*.hex` files.
- Licence, from that repository's README: *"You can consider it Public Domain (or CC-0) except for the files derived from
  or containing parts of Roman Czyborra's Unifont project (unifont.hex, hex2bdf.pl, unscii-16-full.*) which fall under
  GPL."* The generated files here are tagged `CC0-1.0`.
- **`unscii-16-full` is deliberately not included**: it contains Unifont glyphs and is GPL. Nothing derived from
  `unifont.hex` is used.

## What is included

| C object | Source | Cell | Glyphs |
|---|---|---|---|
| `tg_font_unscii8` | unscii-8.hex | 8x8 | 3191 |
| `tg_font_unscii8_alt` | unscii-8-alt.hex | 8x8 | 3191 |
| `tg_font_unscii8_thin` | unscii-8-thin.hex | 8x8 | 3191 |
| `tg_font_unscii8_fantasy` | unscii-8-fantasy.hex | 8x8 | 3191 |
| `tg_font_unscii8_mcr` | unscii-8-mcr.hex | 8x8 | 3191 |
| `tg_font_unscii8_tall` | unscii-8-tall.hex | 8x16 | 3191 |
| `tg_font_unscii16` | unscii-16.hex | 8x16 | 2997 |

Codepoints run from U+0000 to U+1FBF9: Latin, Greek, Cyrillic, box drawing, block elements, Braille, and the legacy
computing symbols. Each glyph is one byte per row, most significant bit leftmost.

Two things differ from the source files:
- `unscii-16.hex` has 243 double-width (16x16) glyphs. They do not fit the 8-pixel cell used here and were left out.
- In the 8x8 files, U+0000 is stored as 16 zero bytes. It is kept as 8 zero bytes (the generator checks it is all zeros).

## Regenerating

    python3 -I tools/gen_fonts.py <path to unscii/fontfiles> fonts

The source files this was generated from have these SHA-256 sums:

    03094f7fbab7085cf6a6b624cee61e47e71ce5d0c2f308c2f4436afdc17f776c  unscii-8.hex
    516c05e4b53b974bf4f53578fd0c8c7644d18bd99f737c86ff67fc74fb2d774e  unscii-8-alt.hex
    9b915268284415ead64ca77eedc30107c44c3a65c08df201e29c9eaed5d6f156  unscii-8-thin.hex
    7fbda7ebec089ac10ba9b8888b8ca1cad97e0e84788cb7d2b4fbd5bff1d20cc4  unscii-8-fantasy.hex
    b116a5cb19eeca6ba01f938d86deb8e33f8e7ea260b3cc707382df29b9f8744d  unscii-8-mcr.hex
    1f3ca1c7b9ec03e2368e8e93562b49623a829ee99197b39ba7a1314f533527fd  unscii-8-tall.hex
    2642c8b748fa81f24d76772d70c55faa720d98fadfec8133daf89136c5c8bfb1  unscii-16.hex

`test_font.c` checks the generated tables against glyphs copied from the original `.hex` files.
