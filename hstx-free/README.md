# hstx-free

A 640x480@60 HDMI test pattern for the RP2350 HSTX, built on the upstream
[pico_hdmi](https://github.com/fliperama86/pico_hdmi) library (Unlicense). Nothing here is taken from
frank-nes, frank-msx or frank-386, so the whole tree is free of GPL code. It is a base for HSTX projects.

- `pico_hdmi/` is an unmodified copy of upstream (commit in `pico_hdmi/UPSTREAM_COMMIT`).
- `main.c` is original to this project: colour bars, a bouncing box, a status line on USB serial. It never waits for a terminal.
- The EmuBoard2350 works with the library's default pinout. `-DSWAPPED_PAIRS=ON` selects the opposite polarity through upstream's `video_output_set_hstx_pinout()`, for boards wired that way.

Build: `export PICO_SDK_PATH=...; cmake -S . -B build -DCPU_MHZ=126 -DPICOTOOL_FORCE_FETCH_FROM_GIT=1; cmake --build build`.
`CPU_MHZ` may be 126, 252, 378 or 504.

## Licence

`pico_hdmi/` is Unlicense (public domain). `main.c` and the CMake files are also released under the Unlicense.
`pico_sdk_import.cmake` is from the Pico SDK (BSD-3-Clause).
