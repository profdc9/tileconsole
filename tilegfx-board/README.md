# tilegfx-board

The [tilegfx](../tilegfx) compositor on the RP2350, driving HDMI through the upstream pico_hdmi in
[hstx-free](../hstx-free) (the project assumes both sit next to this directory; override with `-DTILEGFX_DIR=` and `-DHSTX_FREE_DIR=`).

- Core 1: the HSTX DMA interrupt hands out pointers into a ring of pre-rendered lines (each of the 240 source
  lines serves two scanlines, so nothing is copied); the background loop renders ahead of the scanout.
- Core 0: the game. It publishes its `tg_t` state through a spinlock-guarded mailbox; core 1 takes the newest at the
  start of each frame, so the game can never change state mid-frame.
- The compositor is placed in RAM (`TG_HOT_SECTION`); `tg_render_line_indices` has no calls into flash.
- USB serial prints once a second: clocks, frames per second, render time per line, underruns (the renderer was
  late and the previous line was repeated), and the smallest lead over the scanout.

Build: `export PICO_SDK_PATH=...; cmake -S . -B build -DCPU_MHZ=504 -DPICOTOOL_FORCE_FETCH_FROM_GIT=1; cmake --build build`.
`CPU_MHZ` may be 126, 252, 378 or 504. MIT licensed; `pico_hdmi` is the upstream Unlicense copy.
