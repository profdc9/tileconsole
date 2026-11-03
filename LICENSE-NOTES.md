# Licences

The projects written for tileconsole (`tilegame`, `tilegfx`, `tilesnd`, `tilefs`, `tilegfx-board`) are MIT licensed: each folder has its own `LICENSE`. Parts that came from elsewhere keep their own licences:

| where | what | licence |
|---|---|---|
| `tilegame/tde/src/` | TDE, the Thomson-Davis Editor 5.1v (Frank Davis, Jason Hood), with the changes marked "tilegame" | public domain (see `tilegame/tde/src/readme.txt`) |
| `tilegame/basic/my_basic/` | MY-BASIC, unmodified | MIT (`LICENSE` in that folder) |
| `tilegame/third_party/dlmalloc/` | dlmalloc 2.8.6 by Doug Lea, unmodified (the pools in `tilegame/src/pool.c` configure it) | MIT-0 (the notice is in `LICENSE` in that folder) |
| `tilefs/third_party/fatfs/` | FatFs R0.16 by ChaN, unmodified | the FatFs licence (`LICENSE.txt` in that folder) |
| `tilegfx/fonts/` | glyphs generated from Unscii by Viznut | public domain / CC0 (see `tilegfx/fonts/FONTS.md`; the GPL `unscii-16-full` is not included) |
| `hstx-free/pico_hdmi/` | upstream pico_hdmi, unmodified | Unlicense (`LICENSE` in that folder) |
| `tilegame/editor/` | the text editor of PicoParanoia (Jumentum-SOC, Daniel Marks) | zlib (the licence is in the file header) |

The pico-sdk is not part of this repository.
