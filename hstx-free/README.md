# hstx-free

A 640x480@60 HDMI test pattern for the RP2350 HSTX, built on the upstream
[pico_hdmi](https://github.com/fliperama86/pico_hdmi) library (Unlicense). Nothing here is taken from
frank-nes, frank-msx or frank-386, so the whole tree is free of GPL code. It is a base for HSTX projects.

- `pico_hdmi/` is a copy of upstream (commit in `pico_hdmi/UPSTREAM_COMMIT`) with two local changes, both for the tilegame board build:
  - `src/video_output.c`: diagnostics (the scanline interrupt's timing, a log of slow interrupts, a snapshot of the DMA and HSTX state, and `video_output_force_resync()`), used by tilegame's video watch.
  - `PICO_HDMI_RAM_AUDIO_PATH` (CMake option, off by default): the code that builds, encodes and queues an audio sample packet, and its tables, run from RAM instead of flash (`src/hstx_packet.c`, `src/hstx_data_island_queue.c`). Off, the library is as upstream has it.
- `main.c` is original to this project: colour bars, a bouncing box, a status line on USB serial. It never waits for a terminal.
- The EmuBoard2350 works with the library's default pinout. `-DSWAPPED_PAIRS=ON` selects the opposite polarity through upstream's `video_output_set_hstx_pinout()`, for boards wired that way.

Build: `export PICO_SDK_PATH=...; cmake -S . -B build -DCPU_MHZ=126 -DPICOTOOL_FORCE_FETCH_FROM_GIT=1; cmake --build build`.
`CPU_MHZ` may be 126, 252, 378 or 504.

## Licence

`pico_hdmi/` is Unlicense (public domain). `main.c` and the CMake files are also released under the Unlicense.
`pico_sdk_import.cmake` is from the Pico SDK (BSD-3-Clause).
