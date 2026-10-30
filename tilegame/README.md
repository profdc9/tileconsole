# tilegame

A platform layer (`include/platform.h`) for small console-style games built on [tilegfx](../tilegfx): a 60 Hz frame
loop, time, buttons and an audio callback. One header, one backend per target:

- `sdl/plat_sdl.c`: desktop (SDL2). Arrows, Z X A S, Return and Right Shift are the buttons.
- `board/plat_board.c`: the RP2350 board: HSTX HDMI through upstream pico_hdmi, a NES gamepad on GPIO, and HDMI audio.

`demo/game_demo.c` is a portable demo that uses only `platform.h`. `make check` runs the SDL backend's headless tests;
`make game_demo` builds the desktop demo. MIT licensed.
