/* SPDX-License-Identifier: MIT */
/*
 * The platform layer on the RP2350.
 *
 *  - Core 1: the HSTX DMA interrupt hands out pointers into a ring of pre-rendered lines (each of the 240 source lines
 *    serves two scanlines); the background task renders ahead of the scanout and keeps the HDMI audio queue full.
 *  - Core 0: the game. plat_present() gives core 1 a copy of the game's state through a spinlock-guarded mailbox, and
 *    core 1 takes the newest one at the start of a frame.
 *  - Input: a NES gamepad on three GPIO pins, read once per frame.
 *  - Audio: the game's callback is called from core 1 and its samples go out as HDMI audio packets.
 */
#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/video_output.h"

#include "platform.h"
#include "tilegfx.h"
#if PLAT_USB_HID
#include "usb_hid.h"
#endif

#ifndef PLAT_CPU_MHZ
#define PLAT_CPU_MHZ 504
#endif
#ifndef PLAT_NES_CLK
#define PLAT_NES_CLK 20
#define PLAT_NES_LATCH 21
#define PLAT_NES_DATA 26
#endif
#ifndef PLAT_STATS
#define PLAT_STATS 1
#endif
#ifndef PLAT_USB_HID
#define PLAT_USB_HID 0
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

static volatile uint32_t underruns, lines_done, render_us_total, render_us_max;
static volatile uint32_t audio_calls, audio_us_total, audio_us_max;   /* time spent in the game's audio callback */

static uint32_t frame_no, seen_gen;
static plat_buttons_t held[PLAT_MAX_PLAYERS], pressed[PLAT_MAX_PLAYERS], released[PLAT_MAX_PLAYERS];
static plat_mouse_t mouse_frame;

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
    if (rendered - (int32_t)s <= 0) {   /* the renderer is late: repeat the previous line rather than show stale data */
        underruns++;
        if (s)
            s--;
    }
    return ring[s % RING];
}

/* ---- audio: keep the HDMI audio packet queue full from the game's callback ---- */

#define AUDIO_CHUNK 64
#define AUDIO_QUEUE_TARGET 200
#define AUDIO_PACKETS_PER_CALL 8   /* bounded, so the renderer is never held up for long */

static volatile plat_audio_fn audio_fn;
static void *volatile audio_user;
static int16_t audio_buf[AUDIO_CHUNK * 2];
static int audio_pos = AUDIO_CHUNK;
static int audio_cs_counter;

static void __not_in_flash_func(audio_pump)(void)
{
    plat_audio_fn fn = audio_fn;
    if (!fn)
        return;
    for (int n = 0; n < AUDIO_PACKETS_PER_CALL && hstx_di_queue_get_level() < AUDIO_QUEUE_TARGET; n++) {
        audio_sample_t samples[4];
        for (int i = 0; i < 4; i++) {
            if (audio_pos >= AUDIO_CHUNK) {
                const uint32_t t0 = time_us_32();
                fn(audio_buf, AUDIO_CHUNK, audio_user);
                const uint32_t dt = time_us_32() - t0;
                audio_calls++;
                audio_us_total += dt;
                if (dt > audio_us_max)
                    audio_us_max = dt;
                audio_pos = 0;
            }
            samples[i].left = audio_buf[2 * audio_pos];
            samples[i].right = audio_buf[2 * audio_pos + 1];
            audio_pos++;
        }
        hstx_packet_t packet;
        audio_cs_counter =
            hstx_packet_set_audio_samples_cs_rate(&packet, samples, 4, audio_cs_counter, PLAT_AUDIO_RATE);
        hstx_data_island_t island;
        hstx_encode_data_island(&island, &packet, false, DI_HSYNC_ACTIVE);
        hstx_di_queue_push(&island);
    }
}

/* ---- video: core 1's background task renders ahead of the scanout ---- */

static void __not_in_flash_func(render_step)(void)
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

static void __not_in_flash_func(background)(void)
{
    render_step();
    audio_pump();
}

/* ---- input: a NES gamepad (a 4021 shift register) on GPIO ---- */

static void nes_init(void)
{
    gpio_init(PLAT_NES_CLK);
    gpio_set_dir(PLAT_NES_CLK, GPIO_OUT);
    gpio_put(PLAT_NES_CLK, 0);
    gpio_init(PLAT_NES_LATCH);
    gpio_set_dir(PLAT_NES_LATCH, GPIO_OUT);
    gpio_put(PLAT_NES_LATCH, 0);
    gpio_init(PLAT_NES_DATA);
    gpio_set_dir(PLAT_NES_DATA, GPIO_IN);
    gpio_pull_up(PLAT_NES_DATA);   /* nothing connected reads as "no buttons" */
}

static plat_buttons_t nes_read(void)
{
    gpio_put(PLAT_NES_LATCH, 1);
    busy_wait_us_32(12);
    gpio_put(PLAT_NES_LATCH, 0);
    busy_wait_us_32(6);
    uint32_t bits = 0;
    for (int i = 0; i < 8; i++) {   /* A, B, Select, Start, Up, Down, Left, Right; a low level is a pressed button */
        bits |= (uint32_t)!gpio_get(PLAT_NES_DATA) << i;
        gpio_put(PLAT_NES_CLK, 1);
        busy_wait_us_32(6);
        gpio_put(PLAT_NES_CLK, 0);
        busy_wait_us_32(6);
    }
    plat_buttons_t b = 0;
    if (bits & 0x01) b |= PLAT_BTN_A;
    if (bits & 0x02) b |= PLAT_BTN_B;
    if (bits & 0x04) b |= PLAT_BTN_SELECT;
    if (bits & 0x08) b |= PLAT_BTN_START;
    if (bits & 0x10) b |= PLAT_BTN_UP;
    if (bits & 0x20) b |= PLAT_BTN_DOWN;
    if (bits & 0x40) b |= PLAT_BTN_LEFT;
    if (bits & 0x80) b |= PLAT_BTN_RIGHT;
    return b;
}

/* ---- start-up ---- */

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

bool plat_init(void)
{
    if (PLAT_CPU_MHZ > 252) {
        vreg_disable_voltage_limit();
        vreg_set_voltage(VREG_VOLTAGE_1_65);
        sleep_ms(10);
    }
    set_flash_timing(PLAT_CPU_MHZ);
    set_sys_clock_khz(PLAT_CPU_MHZ * 1000, true);
    stdio_init_all();
#if PLAT_USB_HID
    usb_hid_init();
#endif

    lock = spin_lock_init(spin_lock_claim_unused(true));
    tg_init(&live);
    nes_init();

    hstx_di_queue_init();
    video_output_set_vsync_callback(vsync);
    video_output_init(640, 480);
    pico_hdmi_set_audio_sample_rate(PLAT_AUDIO_RATE);
    video_output_set_scanline_pointer_callback(scanline_ptr);
    video_output_set_background_task(background);
    multicore_launch_core1(video_output_core1_run);
    seen_gen = frame_gen;
    return true;
}

void plat_shutdown(void) {}

void plat_present(const tg_t *g)
{
    uint32_t s = spin_lock_blocking(lock);
    pending = *g;
    pending_valid = true;
    spin_unlock(lock, s);
}

uint32_t plat_wait_frame(void)
{
#if PLAT_USB_HID
    usb_hid_task();   /* the USB host stack needs to run often; a late frame must not starve it */
#endif
    while (frame_gen == seen_gen) {
#if PLAT_USB_HID
        usb_hid_task();
#endif
        tight_loop_contents();
    }
    seen_gen = frame_gen;

    const plat_buttons_t nes = nes_read();
    for (int p = 0; p < PLAT_MAX_PLAYERS; p++) {
        plat_buttons_t now = p == 0 ? nes : 0;
#if PLAT_USB_HID
        now |= usb_hid_buttons(p);   /* keyboards and the first gamepad are player 0, the second gamepad player 1 */
#endif
        pressed[p] = now & (plat_buttons_t)~held[p];
        released[p] = held[p] & (plat_buttons_t)~now;
        held[p] = now;
    }
#if PLAT_USB_HID
    mouse_frame = usb_hid_mouse_frame();
#endif

#if PLAT_STATS
    static uint32_t last_us, last_lines, last_total, last_frame;
    const uint32_t now_us = time_us_32();
    if (now_us - last_us >= 1000000) {
        const uint32_t lines = lines_done, total = render_us_total, dl = lines - last_lines;
        printf("%lu fps  render %lu us/line (%lu max)  underruns %lu  audio queue %lu, silence packets %lu  audio %lu us per %d frames (%lu max)\n",
               (unsigned long)(frame_no - last_frame), (unsigned long)(dl ? (total - last_total) / dl : 0),
               (unsigned long)render_us_max, (unsigned long)underruns, (unsigned long)hstx_di_queue_get_level(),
               (unsigned long)hstx_di_queue_silence_count, (unsigned long)(audio_calls ? audio_us_total / audio_calls : 0), AUDIO_CHUNK,
               (unsigned long)audio_us_max);
        audio_calls = audio_us_total = audio_us_max = 0;
        last_us = now_us;
        last_lines = lines;
        last_total = total;
        last_frame = frame_no;
        render_us_max = 0;
    }
#endif
    return ++frame_no;
}

bool plat_quit_requested(void) { return false; }

uint32_t plat_millis(void) { return (uint32_t)(time_us_64() / 1000); }
uint32_t plat_frame(void) { return frame_no; }

plat_mouse_t plat_mouse(void) { return mouse_frame; }

void plat_mouse_set_position(int x, int y)
{
#if PLAT_USB_HID
    usb_hid_mouse_set_position(x, y);
#else
    (void)x;
    (void)y;
#endif
}

plat_buttons_t plat_buttons(int p) { return p >= 0 && p < PLAT_MAX_PLAYERS ? held[p] : 0; }
plat_buttons_t plat_buttons_pressed(int p) { return p >= 0 && p < PLAT_MAX_PLAYERS ? pressed[p] : 0; }
plat_buttons_t plat_buttons_released(int p) { return p >= 0 && p < PLAT_MAX_PLAYERS ? released[p] : 0; }

bool plat_audio_start(plat_audio_fn fn, void *user)
{
    audio_fn = NULL;   /* stop the pump while the callback is replaced */
    audio_user = user;
    audio_pos = AUDIO_CHUNK;
    audio_fn = fn;
    return true;
}
