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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/usb.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/video_output.h"

#if PLAT_PSRAM
#include "psram.h"
#endif
#include "platform.h"
#include "tilegfx.h"
#if PLAT_USB_HID
#include "usb_hid.h"
#include "usblog_board.h"
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

/* ---- a hang monitor: with USB in host mode there is no console, so a hung core 0 is caught and reported by core 1 ----
 * Core 0 bumps a counter every time round its frame loop. If it stops for two seconds, core 1 records what the USB controller was
 * doing in the watchdog scratch registers (which survive the reset) and reboots; the next start shows them on the diagnostic screen. */
#define HANG_MAGIC 0x48414E47u   /* "HANG" */
#define PANIC_MAGIC 0x50414E49u  /* "PANI" */
static volatile uint32_t core0_beat;
static uint32_t hang_report[4];   /* what the previous run left behind, if it hung */
static bool hung_last_run, panicked_last_run;

#if PLAT_USB_HID
/* The linker sends every call of panic() here (-Wl,--wrap=panic). There is no console to print on, so the start of the message and
 * its first integer argument are kept across a reset. TinyUSB panics inside its interrupt handler on a data sequence error and on
 * anything it does not expect, which would otherwise leave the board stuck there. */
void __attribute__((noreturn)) __wrap_panic(const char *fmt, ...)
{
    uint32_t text[2] = {0, 0};
    for (int i = 0; fmt && i < 8 && fmt[i]; i++)
        ((char *)text)[i] = fmt[i];
    va_list ap;
    va_start(ap, fmt);
    const uint32_t arg = va_arg(ap, uint32_t);
    va_end(ap);
    usblog(USBLOG_EV_PANIC, 0, text[0], text[1], arg, 0);
    usblog_close_run(USBLOG_CLOSED_PANIC);
    watchdog_hw->scratch[0] = PANIC_MAGIC;
    watchdog_hw->scratch[1] = text[0];
    watchdog_hw->scratch[2] = text[1];
    watchdog_hw->scratch[3] = arg;
    watchdog_reboot(0, 0, 0);
    for (;;)
        tight_loop_contents();
}
#endif

static void __not_in_flash_func(hang_check)(void)
{
    static uint32_t seen, since_us;
    const uint32_t now = time_us_32();
    const uint32_t beat = core0_beat;
    if (beat == 0)
        return;   /* the game has not started its frame loop yet */
    if (beat != seen) {
        seen = beat;
        since_us = now;
        return;
    }
    if (now - since_us < 2000000)
        return;
    uint32_t nonzero = 0;   /* how often an interrupt is pending over a few microseconds: a storm reads nonzero every time */
    for (int i = 0; i < 1000; i++)
        nonzero += usb_hw->ints != 0;
#if PLAT_USB_HID
    usblog_usb_irq_state();
    usblog(USBLOG_EV_HANG, 0, usb_hw->sie_status, usb_hw->ints, usb_hw->buf_status, nonzero);
    usblog_close_run(USBLOG_CLOSED_HANG);
#endif
    watchdog_hw->scratch[0] = HANG_MAGIC;
    watchdog_hw->scratch[1] = usb_hw->sie_status;
    watchdog_hw->scratch[2] = usb_hw->ints;
    watchdog_hw->scratch[3] = nonzero << 16 | (usb_hw->buf_status & 0xFFFF);
    watchdog_reboot(0, 0, 0);
}

static tg_t live;              /* core 1's copy for the frame being drawn */
static tg_t pending;           /* the mailbox from core 0 */
static bool pending_valid;
static spin_lock_t *lock;

/* Text mode: the text screen goes in one of three buffers, which core 1 is handed by number so that it never copies one. One is
 * the live frame's, one the newest presented, and core 0 writes the third (all changed under the lock). */
static tg_text_t text_buf[3];
static int text_pending = -1, text_live = -1;
static volatile uint32_t scan_shift = 1;       /* picture line N is display line N >> scan_shift (tile mode doubles lines) */
static volatile int32_t frame_lines = TG_HEIGHT;   /* display lines in the live frame: 240 in tile mode, 480 in text mode */

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
    uint32_t s = active_line >> scan_shift;
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
        video_diag_phase[1] = 5;
        /* Not spin_lock_blocking: that switches this core's interrupts off while it waits for core 0 to let go of the lock (and while it copies), which held the
         * scanline interrupt back by up to 65 us in the video log, a whole line, enough to leave the DMA chain running a stale command list. The scanline
         * interrupt does not take this lock, so it can run in between. */
        spin_lock_unsafe_blocking(lock);
        if (pending_valid) {
            live = pending;
            if (live.mode == TG_MODE_TEXT)
                text_live = text_pending;
            pending_valid = false;
        }
        spin_unlock_unsafe(lock);
        video_diag_phase[1] = 3;
        live.frame = my_gen;   /* what the text mode cursor blinks by: it goes on blinking while the program does nothing */
        frame_lines = tg_display_lines(&live);
        scan_shift = (uint32_t)tg_display_line_shift(&live);
    }
    /* Source lines before cons-1 are no longer being read by the scanout, so their slots are free. */
    while (next < frame_lines && next <= cons - 2 + RING) {
        const uint32_t t0 = time_us_32();
        tg_render_display_line(&live, next, ring[next % RING]);
        const uint32_t dt = time_us_32() - t0;
        render_us_total += dt;
        lines_done++;
        if (dt > render_us_max)
            render_us_max = dt;
        video_diag_phase[1] = 6;
        const uint32_t save = save_and_disable_interrupts();
        const bool same_frame = frame_gen == my_gen;
        if (same_frame)
            rendered = next + 1;
        restore_interrupts(save);
        video_diag_phase[1] = 3;
        if (!same_frame)
            return;   /* a new frame has begun; start again from the top */
        next++;
    }
}

/* ---- a video watch: finds out when the picture is lost, restarts the output, and keeps what it saw ----
 * The loss looks like this: the TV shows "no signal" while the program goes on, and the frames still count (they count faster: a desynchronised
 * command stream no longer holds the DMA back). Core 1 counts frames and scanline interrupts every quarter of a second; a rate that is not 60 Hz twice
 * in a row is a loss. It then writes down the state of the interrupt, the DMA channels and the HSTX, whether the command lists changed, and the
 * last three seconds of samples, and restarts the output (video_output_force_resync). plat_video_report gives all of it to core 0 (VIDEO). */
#define VW_SAMPLE_US 250000
#define VW_HISTORY 32            /* samples kept: 8 seconds */
#define VW_RECENT 12             /* samples copied into an event: 3 seconds */
#define VW_EVENTS 8              /* events kept (the latest) */

typedef struct {
    uint32_t ms, frames, isrs, gap_us, isr_us, late, activity;
} vw_sample_t;

typedef struct {
    uint32_t ms, activity, number;
    int recovered;               /* 0: not known yet, 1: the rate was right again afterwards, -1: it was not */
    const char *why;
    video_diag_snapshot_t snap;
    vw_sample_t recent[VW_RECENT];
} vw_event_t;

static vw_sample_t vw_history[VW_HISTORY];
static uint32_t vw_samples;      /* samples taken */
static vw_event_t vw_events[VW_EVENTS];
static volatile uint32_t vw_event_count;
static volatile uint32_t plat_activity_now;

void plat_activity(unsigned what)
{
    plat_activity_now = what;
    video_diag_activity = (uint8_t)what;
}

static void __not_in_flash_func(video_watch)(void)
{
    static uint32_t next_us, last_frames, last_isrs, last_late, bad, good_since;
    static bool primed;
    const uint32_t now = time_us_32();
    if ((int32_t)(now - next_us) < 0)
        return;
    next_us = now + VW_SAMPLE_US;

    const uint32_t frames = frame_gen, isrs = video_diag_isr_count, late = underruns;
    vw_sample_t s = {to_ms_since_boot(get_absolute_time()), frames - last_frames, isrs - last_isrs, video_diag_max_gap_us, video_diag_max_isr_us,
                     late - last_late, plat_activity_now};
    last_frames = frames;
    last_isrs = isrs;
    last_late = late;
    video_output_diag_reset_max();
    if (!primed) {   /* the first sample after a start or a restart is only a baseline */
        primed = true;
        return;
    }
    vw_history[vw_samples++ % VW_HISTORY] = s;
    const bool wrong = s.frames < 13 || s.frames > 17;   /* 15 are due in a quarter of a second */
    const bool gross = s.frames == 0 || s.frames >= 25;   /* clearly gone: no need to wait for a second sample */
    vw_event_t *last = vw_event_count ? &vw_events[(vw_event_count - 1) % VW_EVENTS] : NULL;
    if (last && last->recovered == 0) {   /* did the last restart work? */
        if (!wrong) {
            if (++good_since >= 2)
                last->recovered = 1;
        } else {
            good_since = 0;
            if (bad >= 1)
                last->recovered = -1;
        }
    }
    if (!wrong) {
        bad = 0;
        return;
    }
    /* two wrong samples in a row, or one that is clearly gone, or a restart that did not work (the last event is not yet known to have recovered) */
    if (++bad < 2 && !gross && !(last && last->recovered == 0))
        return;
    bad = 0;
    vw_event_t *e = &vw_events[vw_event_count % VW_EVENTS];
    memset(e, 0, sizeof *e);
    e->ms = s.ms;
    e->activity = s.activity;
    e->number = vw_event_count + 1;
    e->why = s.frames == 0 ? "frames stopped" : s.frames > 18 ? "frames too fast" : "frames too slow";
    video_output_diag_snapshot(&e->snap);
    const uint32_t n = vw_samples < VW_RECENT ? vw_samples : VW_RECENT;
    for (uint32_t i = 0; i < n; i++)
        e->recent[i] = vw_history[(vw_samples - n + i) % VW_HISTORY];
    vw_event_count++;
    good_since = 0;
    primed = false;
    video_output_force_resync();
}

static void __not_in_flash_func(background)(void)
{
    video_diag_phase[1] = 1;
    hang_check();
    video_diag_phase[1] = 2;
    video_watch();
    video_diag_phase[1] = 3;
    render_step();
    video_diag_phase[1] = 4;
    audio_pump();
    video_diag_phase[1] = 0;
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
#if PLAT_USB_HID
    usblog_boot();   /* (first: the previous run's USB log is in RAM that nothing has used yet) */
#endif
    if (PLAT_CPU_MHZ > 252) {
        vreg_disable_voltage_limit();
        vreg_set_voltage(VREG_VOLTAGE_1_65);
        sleep_ms(10);
    }
    set_flash_timing(PLAT_CPU_MHZ);
    set_sys_clock_khz(PLAT_CPU_MHZ * 1000, true);
#if PLAT_PSRAM
    psram_init(PSRAM_CS_PIN, 133);   /* (before core 1 starts: this stops flash reads for a moment) */
#endif
    stdio_init_all();
    if (watchdog_hw->scratch[0] == HANG_MAGIC || watchdog_hw->scratch[0] == PANIC_MAGIC) {
        hung_last_run = true;
        panicked_last_run = watchdog_hw->scratch[0] == PANIC_MAGIC;
        for (int i = 0; i < 4; i++)
            hang_report[i] = watchdog_hw->scratch[i];
        watchdog_hw->scratch[0] = 0;
    }
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
    video_diag_phase[0] = 1;
    int idx = -1;
    if (g->mode == TG_MODE_TEXT && g->text) {   /* the text screen is a snapshot: the caller may change it as soon as this returns */
        uint32_t s = spin_lock_blocking(lock);
        for (idx = 0; idx == text_pending || idx == text_live; idx++)
            ;   /* the lowest of the three buffers that is neither the newest presented nor the live one */
        spin_unlock(lock, s);
        memcpy(&text_buf[idx], g->text, sizeof text_buf[idx]);
    }
    uint32_t s = spin_lock_blocking(lock);
    pending = *g;
    if (idx >= 0) {
        pending.text = &text_buf[idx];
        text_pending = idx;
    }
    pending_valid = true;
    spin_unlock(lock, s);
    video_diag_phase[0] = 0;
}

/* ---- the video watch's report, for core 0 (the VIDEO command) ---- */

static const char *activity_name(uint32_t a)
{
    static const char *const names[] = {"none", "prompt", "run starting", "editor", "parsing", "running", "run ending", "gfx setup"};
    return a < sizeof names / sizeof *names ? names[a] : "other";
}

static const char *phase0_name(uint32_t p)
{
    static const char *const names[] = {"other", "plat_present", "present copy", "waiting for a frame", "USB task", "NES read"};
    return p < sizeof names / sizeof *names ? names[p] : "?";
}

static const char *phase1_name(uint32_t p)
{
    static const char *const names[] = {"between tasks", "hang check", "video watch", "render", "audio pump", "render: waiting for the lock", "render: interrupts off"};
    return p < sizeof names / sizeof *names ? names[p] : "?";
}

static size_t app(char *buf, size_t size, size_t at, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static size_t app(char *buf, size_t size, size_t at, const char *fmt, ...)
{
    if (at >= size)
        return at;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + at, size - at, fmt, ap);
    va_end(ap);
    return n < 0 ? at : at + (size_t)n;
}

size_t plat_video_report(char *buf, size_t size)
{
    size_t at = 0;
    const uint32_t total = vw_event_count, shown = total < VW_EVENTS ? total : VW_EVENTS;
    at = app(buf, size, at, "video: %lu restarts since power on (%lu kept), uptime %lu ms, resyncs by the driver %lu\n", (unsigned long)total, (unsigned long)shown,
             (unsigned long)to_ms_since_boot(get_absolute_time()), (unsigned long)video_output_resync_count);
    {
        const uint32_t sc = video_diag_slow_count, sn = sc < VIDEO_DIAG_SLOW ? sc : VIDEO_DIAG_SLOW;
        at = app(buf, size, at, "\nslow interrupts: %lu so far (a gap over 45 us, or the other DMA channel already finished), %lu times the other channel was already done\n",
                 (unsigned long)sc, (unsigned long)video_diag_both_idle_count);
        if (sn)
            at = app(buf, size, at, " newest first: interrupt number, time us, gap us, scanline, flags (1 other done, 2 pong, 4 posted), core 0 phase, core 1 phase, activity\n");
        for (uint32_t k = 0; k < sn; k++) {
            const video_diag_slow_t *r = (const video_diag_slow_t *)&video_diag_slow[(sc - 1 - k) % VIDEO_DIAG_SLOW];
            at = app(buf, size, at, "  %lu %lu %lu %u %u %s; %s; %s\n", (unsigned long)r->isr_count, (unsigned long)r->t_us, (unsigned long)r->gap_us, r->v_scanline, r->flags,
                     phase0_name(r->phase0), phase1_name(r->phase1), activity_name(r->activity));
        }
    }
    for (uint32_t k = 0; k < shown; k++) {
        const vw_event_t *e = &vw_events[(total - 1 - k) % VW_EVENTS];   /* the newest first */
        const video_diag_snapshot_t *d = &e->snap;
        at = app(buf, size, at, "\n#%lu at %lu ms, core 0 was in the %s: %s, %s\n", (unsigned long)e->number, (unsigned long)e->ms, activity_name(e->activity), e->why,
                 e->recovered > 0 ? "recovered" : e->recovered < 0 ? "NOT recovered" : "recovery not checked");
        at = app(buf, size, at, "  interrupts %lu, frames %lu, scanline %lu, flags %lx (1 posted, 2 pong next), longest gap %lu us, longest interrupt %lu us\n",
                 (unsigned long)d->isr_count, (unsigned long)d->frame_count, (unsigned long)d->v_scanline, (unsigned long)d->flags, (unsigned long)d->max_gap_us,
                 (unsigned long)d->max_isr_us);
        for (int c = 0; c < 2; c++)
            at = app(buf, size, at, "  dma %s ctrl %08lx read %08lx count %lu\n", c ? "pong" : "ping", (unsigned long)d->ch[c].ctrl, (unsigned long)d->ch[c].read_addr,
                     (unsigned long)d->ch[c].transfer_count);
        at = app(buf, size, at, "  dma intr %lx, hstx fifo stat %08lx, hstx csr %08lx\n", (unsigned long)d->dma_intr, (unsigned long)d->hstx_fifo_stat, (unsigned long)d->hstx_csr);
        if (d->template_bad_index < 0)
            at = app(buf, size, at, "  command lists: unchanged\n");
        else
            at = app(buf, size, at, "  command lists: CHANGED, word %ld is %08lx, was %08lx\n", (long)d->template_bad_index, (unsigned long)d->template_bad_value,
                     (unsigned long)d->template_bad_was);
        at = app(buf, size, at, "  the 3 seconds before (ms, frames, interrupts, longest gap us, longest interrupt us, late lines, activity):\n");
        for (int i = 0; i < VW_RECENT; i++) {
            const vw_sample_t *s = &e->recent[i];
            if (s->ms)
                at = app(buf, size, at, "   %lu %lu %lu %lu %lu %lu %s\n", (unsigned long)s->ms, (unsigned long)s->frames, (unsigned long)s->isrs, (unsigned long)s->gap_us,
                         (unsigned long)s->isr_us, (unsigned long)s->late, activity_name(s->activity));
        }
    }
    at = app(buf, size, at, "\nthe last samples (the same columns):\n");
    const uint32_t n = vw_samples < VW_HISTORY ? vw_samples : VW_HISTORY;
    for (uint32_t i = 0; i < n; i++) {
        const vw_sample_t *s = &vw_history[(vw_samples - n + i) % VW_HISTORY];
        at = app(buf, size, at, " %lu %lu %lu %lu %lu %lu %s\n", (unsigned long)s->ms, (unsigned long)s->frames, (unsigned long)s->isrs, (unsigned long)s->gap_us,
                 (unsigned long)s->isr_us, (unsigned long)s->late, activity_name(s->activity));
    }
    return at < size ? at : size - 1;
}

uint32_t plat_video_restarts(void)
{
    return vw_event_count;
}


uint32_t plat_wait_frame(void)
{
    core0_beat++;
#if PLAT_USB_HID
    video_diag_phase[0] = 4;
    usb_hid_task();   /* the USB host stack needs to run often; a late frame must not starve it */
#endif
    while (frame_gen == seen_gen) {
        core0_beat++;
#if PLAT_USB_HID
        video_diag_phase[0] = 4;
        usb_hid_task();
#endif
        video_diag_phase[0] = 3;
        tight_loop_contents();
    }
    seen_gen = frame_gen;

    video_diag_phase[0] = 5;
    const plat_buttons_t nes = nes_read();
    video_diag_phase[0] = 0;
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

int plat_getkey(plat_key_event_t *event)
{
#if PLAT_USB_HID
    return usb_hid_getkey(event);
#else
    (void)event;
    return 0;
#endif
}

int plat_getchar(void)
{
#if PLAT_USB_HID
    return usb_hid_getchar();
#else
    return -1;   /* the NES pad has no keyboard */
#endif
}

int plat_input_diag(char (*lines)[41], int max_lines)
{
#if PLAT_USB_HID
    int n = 0;
    if (panicked_last_run && max_lines >= 2) {
        char text[9] = {0};
        memcpy(text, &hang_report[1], 8);
        snprintf(lines[n++], 41, "RESET AFTER A PANIC:");
        snprintf(lines[n++], 41, "%.8s ARG %08lX", text, (unsigned long)hang_report[3]);
    } else if (hung_last_run && max_lines >= 2) {
        snprintf(lines[n++], 41, "RESET AFTER A HANG: SIE %08lX", (unsigned long)hang_report[1]);
        snprintf(lines[n++], 41, "INTS %08lX PENDING %lu BUF %04lX", (unsigned long)hang_report[2], (unsigned long)(hang_report[3] >> 16),
                 (unsigned long)(hang_report[3] & 0xFFFF));
    }
    return n + usb_hid_diag(lines + n, max_lines - n);
#else
    if (max_lines > 0)
        snprintf(lines[0], 41, "NO USB INPUT IN THIS BUILD");
    return max_lines > 0 ? 1 : 0;
#endif
}

void plat_debug_line(char line[41])
{
    static char text[41];
#if PLAT_USB_HID
    static uint32_t last_us, last_frame;
    const uint32_t now = time_us_32();
    if (now - last_us >= 1000000) {
        uint32_t max_us, total_us;
        usb_hid_task_stats(&max_us, &total_us);
        const uint32_t elapsed = now - last_us;
        snprintf(text, sizeof text, "FPS %lu USB %lu%% MAX %lu US", (unsigned long)((frame_no - last_frame) * 1000000ull / elapsed),
                 (unsigned long)(total_us * 100ull / elapsed), (unsigned long)max_us);
        last_us = now;
        last_frame = frame_no;
    }
#endif
    memcpy(line, text, sizeof text);
}

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
