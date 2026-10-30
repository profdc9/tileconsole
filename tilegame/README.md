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

## The platformer

`game/` is a small side-scrolling platformer built on all of the above. Run and jump to the flag at the end of the level, collect
coins, stomp the walking enemies and avoid the pits; three lives.

- `game/level.c`: the level (a 120 x 15 tile grid, built in code); `game/game.c`: the rules, in fixed point and free of any
  display or sound (jump buffering, coyote time, variable jump height, tile collision, enemies, lives); `game/view.c`: all the
  art, made in code at start-up, and the conversion of game state into tilegfx layers and sprites (a parallax background, the
  level, a text HUD; the level and HUD are double-buffered because the display reads their maps); `game/game_main.c`: the glue
  to sound effects, a looping tune and the platform layer.
- Controls: D-pad (arrows) move, A (Z) jumps (hold for a higher jump), B (X) runs, Start (Return) restarts after a game over or win.
- `make platformer` builds the desktop version; `make check` also runs the game tests, which include a bot that has to be able
  to finish the level. For the board: `cmake -S board ...` builds the `platformer` target (add `-DPLAT_USB_KEYBOARD=ON` for the
  keyboard, which takes the USB port and so the serial console).
