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

## Gamepads and mice

- `include/hid_pad.h`, `src/hid_pad.c`: a USB HID report descriptor parser and report decoder for gamepads (buttons, an 8- or
  4-way hat, the left stick, D-pad usages, report IDs, composite devices) with a default button layout and a Sony table;
  `src/hid_mouse.c`: boot-protocol mouse reports; `src/mouse_acc.c`: movement, wheel, button edges and a clamped pointer.
- `plat_mouse()` (platform.h) gives movement since the last frame, the buttons held, pressed and released, a wheel and a pointer
  position kept on the screen. Keyboards, the NES pad and the first gamepad are player 0; the second gamepad is player 1.
- SDL: controllers through SDL's game controller API. Board: `-DPLAT_USB_HID=ON` (the old name `PLAT_USB_KEYBOARD` still works)
  puts the USB port in host mode for keyboards, mice and gamepads, which takes the serial console. Xbox controllers are not HID
  and are not supported.

## USB hubs

A keyboard and a gamepad behind one hub work together. Two things made that work, both in `board/usb_hid.c`:

- TinyUSB must not be asked for anything from inside a mount callback. A control transfer started there collides with the
  enumeration in progress, which then never completes, and the hub's later devices are never looked at. The callbacks only record
  what an interface needs and `usb_hid_task` does it 200 ms after the last device appeared, one control transfer at a time.
- The RP2040/RP2350 host driver panics inside its interrupt handler on a data sequence error, which a low speed device behind some
  hubs produces. That interrupt is masked; the errors are counted on the diagnostic screen.

Not every hub works: with one bus-powered USB 3.0 hub a low-speed keyboard stopped after a few reports (it worked on a laptop and
behind a second hub). Full-speed devices (XInput pads, most HID gamepads) worked behind both. A stuck main loop or a panic resets the
board and the diagnostic screen (`padtest`) shows why.

## Keyboard text input and the console demo

`plat_getchar()` returns the next typed character (or -1): ASCII for printable keys with shift and caps lock applied, 10 for Enter, 8
Backspace, 9 Tab, 27 Escape, 127 Delete, 1..26 for Ctrl with a letter, and `PLAT_KEY_*` codes (256 and up) for the cursor keys, Home, End,
Page Up/Down, Insert and F1..F12. A held key repeats (after 500 ms, then every 33 ms). On the board `src/keymap.c` decodes the boot keyboard
reports (US layout, caps lock and num lock tracked in software, the LEDs are not driven); on the desktop the characters come from SDL's text
input, so the system layout applies and only ASCII is kept. `plat_getchar` is separate from `plat_buttons`: the same key presses count for both.

`demo/consdemo.c` is a console you can type into: a prompt with a line editor (cursor keys, Home/End, Backspace/Delete, Ctrl+A/E/K/U/L/C, history)
and commands (HELP, CLS, ECHO, COLOR, CURSOR, CHARS, TEST, UPTIME, KEYS). Its logic (`demo/consdemo_logic.c`) knows nothing of the platform, so
`tests/test_consdemo.c` types into it. The console layer it is built on is `tilegfx/tg_console.h`.
