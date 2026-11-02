# tileconsole

A retro-console style, self-contained development system for the RP2350 (HDMI on the HSTX, USB keyboard and gamepads, an SD card), and the libraries it is built from. The same code runs on the desktop (SDL2).

The development kit (`tilegame/demo/devkit.c`) is BASIC ([MY-BASIC](https://github.com/paladin-t/my_basic), unmodified) and the TDE editor in one program: type BASIC at the prompt, `EDIT` a program in TDE, come back and `RUN` it. Programs are free-form text files on the SD card.

Six projects, each with its own history, in one repository (they sit next to each other, and the build files refer to each other as `../name`):

| folder | what it is |
|---|---|
| `tilegfx/` | the graphics compositor: tile layers, sprites, 80 x 25 text mode with the PC character set, a console |
| `tilesnd/` | the sound engine |
| `tilefs/` | a POSIX-flavoured file layer: RAM disk, host folder, FatFs on an SD card (SPI) |
| `tilegame/` | the platform layer (SDL and the board), demos, a platformer, the TDE port, the BASIC environment and the development kit |
| `hstx-free/` | a 640 x 480 HDMI test pattern for the HSTX on top of upstream pico_hdmi (a copy of it is in `pico_hdmi/`) |
| `tilegfx-board/` | the compositor on the RP2350 driving HDMI |

Every folder has its own `README.md`. Each project's history is its own lineage in this repository: `git log -- tilefs/` shows only tilefs.

## Build

Desktop (SDL2): `cd tilegame && make check` runs all the tests; `make devkit && ./devkit` is the development kit (run it from `tilegame/`).

Board (needs the pico-sdk): `cmake -S tilegame/board -B build -DCPU_MHZ=504 -DPLAT_USB_HID=ON`, then build the `devkit` target and copy `devkit.uf2` to the board in BOOTSEL mode. The other board programs (`sdtest`, `editsd`, `platformer`, ...) are targets of the same project.

## Licences

See [LICENSE-NOTES.md](LICENSE-NOTES.md).
