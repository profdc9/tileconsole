/*
 * hstx_free: 640x480 HDMI test pattern on the RP2350 HSTX, using the upstream
 * pico_hdmi library (Unlicense) with nothing from the frank-* forks.
 * Colour bars, a bouncing box, and a once-a-second status line on USB serial.
 * The program never waits for a terminal to be attached.
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/vreg.h"
#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_pins.h"
#include "pico_hdmi/video_output.h"

#ifndef CPU_MHZ
#define CPU_MHZ 126
#endif

#define BOX 32
#define BOX_COLOR 0xFFFF
static volatile int box_x = 50, box_y = 50;
static volatile uint32_t frames;

static const uint16_t bars[8] = {0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000};

/* Runs on core 1 in the DMA interrupt: keep it, and everything it reads, in RAM.
 * Two RGB565 pixels per 32-bit word, written a word at a time. */
static void __scratch_x("") scanline(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;
    const int by = box_y, bx = box_x;
    const int words_per_bar = MODE_H_ACTIVE_PIXELS / 2 / 8;
    for (int b = 0; b < 8; b++) {
        const uint32_t w = bars[b] | ((uint32_t)bars[b] << 16);
        uint32_t *d = dst + b * words_per_bar;
        for (int i = 0; i < words_per_bar; i++)
            d[i] = w;
    }
    if ((int)active_line >= by && (int)active_line < by + BOX) {
        const uint32_t w = BOX_COLOR | ((uint32_t)BOX_COLOR << 16);
        for (int i = bx / 2; i < (bx + BOX) / 2; i++)   /* bx is kept even */
            dst[i] = w;
    }
}

static void __scratch_x("") vsync(void) { frames++; }

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

#ifdef SWAPPED_PAIRS
    static const pico_hdmi_hstx_pinout_t pinout = {
        .clock = {.positive_gpio = 12, .negative_gpio = 13},
        .data = {{.positive_gpio = 14, .negative_gpio = 15},
                 {.positive_gpio = 16, .negative_gpio = 17},
                 {.positive_gpio = 18, .negative_gpio = 19}},
    };
    if (!video_output_set_hstx_pinout(&pinout))
        panic("bad HSTX pinout");
#endif

    hstx_di_queue_init();
    video_output_set_vsync_callback(vsync);
    video_output_init(640, 480);
    video_output_set_scanline_callback(scanline);
    multicore_launch_core1(video_output_core1_run);

    int dx = 2, dy = 1;   /* dx stays even so the box lands on a word boundary */
    uint32_t last_f = 0, last_us = time_us_32();
    for (;;) {
        sleep_ms(16);
        int x = box_x + dx, y = box_y + dy;
        if (x <= 0 || x + BOX >= 640) { dx = -dx; x = box_x + dx; }
        if (y <= 0 || y + BOX >= 480) { dy = -dy; y = box_y + dy; }
        box_x = x;
        box_y = y;
        uint32_t now = time_us_32();
        if (now - last_us >= 1000000) {
            uint32_t f = frames;
            printf("sys %lu kHz  hstx %lu kHz  %lu frames/s\n", (unsigned long)(clock_get_hz(clk_sys) / 1000),
                   (unsigned long)(clock_get_hz(clk_hstx) / 1000), (unsigned long)(f - last_f));
            last_f = f;
            last_us = now;
        }
    }
}
