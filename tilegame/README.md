# tilegame

A platform layer (`include/platform.h`) for small console-style games built on [tilegfx](../tilegfx): a 60 Hz frame
loop, time, buttons and an audio callback. One header, one backend per target:

- `sdl/plat_sdl.c`: desktop (SDL2). Arrows, Z X A S, Return and Right Shift are the buttons.
- `board/plat_board.c`: the RP2350 board: HSTX HDMI through upstream pico_hdmi, a NES gamepad on GPIO, and HDMI audio.

`demo/game_demo.c` is a portable demo that uses only `platform.h`. `make check` runs the SDL backend's headless tests;
`make game_demo` builds the desktop demo. MIT licensed.

Board build: `cmake -S board -B build -DCPU_MHZ=504 [-DDEMO_AUTOPLAY=ON]` (needs `PICO_SDK_PATH`; expects `tilegfx` and
`hstx-free` next to this directory, override with `-DTILEGFX_DIR=` and `-DHSTX_FREE_DIR=`). The gamepad is a NES pad
on GPIO 20 (clock), 21 (latch), 26 (data), set by `PLAT_NES_*`. `DEMO_AUTOPLAY` lets the demo press its own buttons.
The board prints fps, render time, underruns and the audio queue level on USB serial once a second.
