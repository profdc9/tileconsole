/* SPDX-License-Identifier: MIT */
/*
 * tilegfx on the RP2350: the compositor runs ahead of the HSTX scanout on core 1; the game runs on core 0.
 *
 *  - Core 1 DMA interrupt: for each of the 480 active lines, hands the HSTX a pointer to a pre-rendered line.
 *    Each source line (240 of them) serves two scanlines, so no pixels are copied.
 *  - Core 1 background loop: renders source lines into a ring, a few lines ahead of the scanout.
 *  - Core 0: builds the next frame's state and publishes it; core 1 picks up the latest at the start of a frame.
 */
#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/systick.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/video_output.h"

#include "scene.h"
#include "tilegfx.h"

#ifndef CPU_MHZ
#define CPU_MHZ 504
#endif

#define RING 8
#define LINE_WORDS (MODE_H_ACTIVE_PIXELS / 2)   /* 320: two RGB565 pixels per word */

static uint32_t ring[RING][LINE_WORDS];

static tg_t live;              /* core 1's copy for the frame being drawn */
static tg_t pending;           /* the mailbox from core 0 */
static bool pending_valid;
static spin_lock_t *lock;

static volatile uint32_t frame_gen;   /* counts vsyncs (written by the vsync interrupt) */
static volatile int32_t cons;         /* highest source line the scanout has asked for this frame */
static volatile int32_t rendered;     /* source lines finished this frame */

static volatile uint32_t underruns, lead_min = 0xFFFF, lines_done;
static volatile uint32_t render_us_total, render_us_max;

/* Publish the game's state; core 1 uses the newest one at the start of the next frame. */
static void publish(const tg_t *g)
{
    uint32_t s = spin_lock_blocking(lock);
    pending = *g;
    pending_valid = true;
    spin_unlock(lock, s);
}

static void __scratch_x("") vsync(void)
{
    frame_gen++;
    cons = 0;
    rendered = 0;
}

static const uint32_t *__scratch_x("") scanline_ptr(uint32_t v_scanline, uint32_t active_line)
{
    (void)v_scanline;
    uint32_t s = active_line >> 1;
    cons = (int32_t)s;
    const int32_t lead = rendered - (int32_t)s;
    if (lead <= 0) {          /* the renderer is late: repeat the previous line rather than show stale data */
        underruns++;
        if (s)
            s--;
    } else if ((uint32_t)lead < lead_min) {
        lead_min = (uint32_t)lead;
    }
    return ring[s % RING];
}

/* Core 1's background task: stay ahead of the scanout, as far as the ring allows. */
static void __not_in_flash_func(render_task)(void)
{
    static uint32_t my_gen;
    static int32_t next;

    if (frame_gen != my_gen) {
        my_gen = frame_gen;
        next = 0;
        uint32_t s = spin_lock_blocking(lock);
        if (pending_valid) {
            live = pending;
            pending_valid = false;
        }
        spin_unlock(lock, s);
    }
    /* Source lines before cons-1 are no longer being read by the scanout, so their slots are free. */
    while (next < TG_HEIGHT && next <= cons - 2 + RING) {
        const uint32_t t0 = time_us_32();
        tg_render_line(&live, next, ring[next % RING]);
        const uint32_t dt = time_us_32() - t0;
        render_us_total += dt;
        lines_done++;
        if (dt > render_us_max)
            render_us_max = dt;
        const uint32_t save = save_and_disable_interrupts();
        const bool same_frame = frame_gen == my_gen;
        if (same_frame)
            rendered = next + 1;
        restore_interrupts(save);
        if (!same_frame)
            return;   /* a new frame has begun; start again from the top */
        next++;
    }
}


#ifdef BENCH_AT_BOOT
/*
 * Compositor cost in CPU cycles per line, measured before the video starts so the scanout interrupt cannot
 * disturb it. Results are printed every few seconds once a terminal is attached.
 */
#define BENCH_N 7
static const char *const bench_names[BENCH_N] = {"empty", "L0 16px 8bpp", "L1 8px 4bpp", "24 sprites", "all", "bitmap", "scan 128"};
static uint32_t bench_avg[BENCH_N], bench_max[BENCH_N];
static uint8_t bench_bitmap[TG_WIDTH * TG_HEIGHT];

static void bench_run(tg_t *g, uint32_t *avg, uint32_t *worst_out)
{
    static uint32_t out[LINE_WORDS];
    uint32_t total = 0, worst = 0;
    for (int pass = 0; pass < 2; pass++) {   /* the second pass is the one that counts */
        total = worst = 0;
        for (int line = 0; line < TG_HEIGHT; line++) {
            systick_hw->cvr = 0;
            tg_render_line(g, line, out);
            const uint32_t c = (0x00FFFFFFu - systick_hw->cvr) & 0x00FFFFFFu;
            total += c;
            if (c > worst)
                worst = c;
        }
    }
    *avg = total / TG_HEIGHT;
    *worst_out = worst;
}

static void bench_all(const tg_t *game)
{
    static tg_t v;
    systick_hw->rvr = 0x00FFFFFF;
    systick_hw->csr = 5;   /* enable, count processor cycles */
    for (int i = 0; i < TG_WIDTH * TG_HEIGHT; i++)
        bench_bitmap[i] = (uint8_t)((i * 7 + (i >> 8)) & 255);
    for (int n = 0; n < BENCH_N; n++) {
            v = *game;
            if (n == 0 || n == 3 || n == 5 || n == 6)
                memset(v.layers, 0, sizeof v.layers);
            if (n == 1)
                v.layers[1].kind = TG_LAYER_OFF;
            if (n == 2)
                v.layers[0].kind = TG_LAYER_OFF;
            if (n == 5)
                v.layers[0] = (tg_layer_t){.kind = TG_LAYER_BITMAP, .bitmap = bench_bitmap};
            if (n != 3 && n != 4)
                memset(v.sprites, 0, sizeof v.sprites);
            v.sprite_count = (n == 3 || n == 4) ? game->sprite_count : (n == 6 ? TG_MAX_SPRITES : 0);
        bench_run(&v, &bench_avg[n], &bench_max[n]);
    }
}

static void bench_print(void)
{
    printf("bench, cycles per line (avg/max), %d MHz:", CPU_MHZ);
    for (int n = 0; n < BENCH_N; n++)
        printf("  %s %lu/%lu", bench_names[n], (unsigned long)bench_avg[n], (unsigned long)bench_max[n]);
    printf("  -> all = %lu us/line\n", (unsigned long)(bench_avg[4] / CPU_MHZ));
}
#endif

/* Flash must keep working once the system clock goes up: slow its clock to stay at or under about 66 MHz. */
static void __no_inline_not_in_flash_func(set_flash_timing)(uint32_t cpu_mhz)
{
    if (cpu_mhz <= 133)
        return;
    uint32_t div = (cpu_mhz + 65) / 66;
    uint32_t rx = div;
    if (cpu_mhz / div > 100)
        rx++;
    uint32_t t = qmi_hw->m[0].timing;
    t &= ~(QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS);
    t |= div << QMI_M0_TIMING_CLKDIV_LSB | rx << QMI_M0_TIMING_RXDELAY_LSB;
    qmi_hw->m[0].timing = t;
}

int main(void)
{
    if (CPU_MHZ > 252) {
        vreg_disable_voltage_limit();
        vreg_set_voltage(VREG_VOLTAGE_1_65);
        sleep_ms(10);
    }
    set_flash_timing(CPU_MHZ);
    set_sys_clock_khz(CPU_MHZ * 1000, true);
    stdio_init_all();

    lock = spin_lock_init(spin_lock_claim_unused(true));
    static tg_t game;
    scene_setup(&game);
    scene_animate(&game, 0, 0, false);
    live = game;
#ifdef BENCH_AT_BOOT
    bench_all(&game);
#endif

    hstx_di_queue_init();
    video_output_set_vsync_callback(vsync);
    video_output_init(640, 480);
    video_output_set_scanline_pointer_callback(scanline_ptr);
    video_output_set_background_task(render_task);
    multicore_launch_core1(video_output_core1_run);

    int frame = 0, scroll = 0;
    uint32_t seen = frame_gen, last_us = time_us_32();
    uint32_t last_lines = 0, last_total = 0, last_gen = frame_gen;
    for (;;) {
        while (frame_gen == seen)
            tight_loop_contents();
        seen = frame_gen;
        scene_animate(&game, frame, scroll, (frame / 300) & 1);   /* the wave switches on and off every 5 s */
        publish(&game);
        frame++;
        scroll++;

        const uint32_t now = time_us_32();
        if (now - last_us >= 1000000) {
            const uint32_t lines = lines_done, total = render_us_total, gen = frame_gen;
            const uint32_t dl = lines - last_lines;
            printf("sys %lu kHz  hstx %lu kHz  %lu fps  render %lu.%02lu us/line avg, %lu us max  "
                   "underruns %lu  lead >= %lu lines this second\n",
                   (unsigned long)(clock_get_hz(clk_sys) / 1000), (unsigned long)(clock_get_hz(clk_hstx) / 1000),
                   (unsigned long)(gen - last_gen), (unsigned long)(dl ? (total - last_total) / dl : 0),
                   (unsigned long)(dl ? (total - last_total) * 100 / dl % 100 : 0), (unsigned long)render_us_max,
                   (unsigned long)underruns, (unsigned long)lead_min);
            lead_min = 0xFFFF;
#ifdef BENCH_AT_BOOT
            if ((frame / 60) % 3 == 0)
                bench_print();
#endif
            last_lines = lines; last_total = total; last_gen = gen; last_us = now;
            render_us_max = 0;
        }
    }
}
