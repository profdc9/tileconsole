#ifndef VIDEO_OUTPUT_H
#define VIDEO_OUTPUT_H

#include "pico_hdmi/hstx_packet.h"

#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// Video Output Configuration
// ============================================================================

// Video mode selection (define VIDEO_MODE_320x240 or VIDEO_MODE_1280x720 before including this header)
#ifdef VIDEO_MODE_320x240

// 1280x240 @ 60Hz - True 240p (Quad Clock Rate)
// Pixel Clock: 25.2 MHz (15kHz scan rate)
// H_TOTAL = 1600 pixels, V_TOTAL = 262 lines
// Active: 1280 (320x4), Front: 32 (8x4), Sync: 192 (48x4), Back: 96 (24x4)
#define MODE_H_FRONT_PORCH 32
#define MODE_H_SYNC_WIDTH 192
#define MODE_H_BACK_PORCH 96
#define MODE_H_ACTIVE_PIXELS 1280

#define MODE_V_FRONT_PORCH 4
#define MODE_V_SYNC_WIDTH 4
#define MODE_V_BACK_PORCH 14
#define MODE_V_ACTIVE_LINES 240
#define MODE_CEA_VIC 0
#define MODE_AVI_ASPECT_16_9 0
#define MODE_H_SYNC_POSITIVE 0
#define MODE_V_SYNC_POSITIVE 0

#ifndef MODE_HSTX_CLK_DIV
// HSTX clock divider: clk_sys / 1 = 126 MHz -> 25.2 MHz pixel clock (with CSR_CLKDIV=5).
#define MODE_HSTX_CLK_DIV 1
#endif
#define MODE_HSTX_CSR_CLKDIV 5

#elif defined(VIDEO_MODE_1280x720)

// Exact-clock 1280x720 CVT reduced blanking: 64 MHz pixel clock from a
// 320 MHz system clock, 1440x741 totals, 59.979 Hz. This is not a CTA mode,
// so its AVI InfoFrame uses VIC 0 while declaring a 16:9 picture.
#ifdef VIDEO_MODE_720P_RB
#define MODE_H_FRONT_PORCH 48
#define MODE_H_SYNC_WIDTH 32
#define MODE_H_BACK_PORCH 80
#define MODE_H_ACTIVE_PIXELS 1280

#define MODE_V_FRONT_PORCH 3
#define MODE_V_SYNC_WIDTH 5
#define MODE_V_BACK_PORCH 13
#define MODE_V_ACTIVE_LINES 720

#define MODE_CEA_VIC 0
#define MODE_AVI_ASPECT_16_9 1
#define MODE_H_SYNC_POSITIVE 1
#define MODE_V_SYNC_POSITIVE 0

#else

// 1280x720 CTA mode. Both 50 Hz and 60 Hz use a nominal 74.25 MHz pixel
// clock, positive sync, 750 total lines, and the same back porch. The 50 Hz
// variant extends the horizontal front porch to 440 pixels (1980 total).
// Using sys_clk=372 MHz gives 74.4 MHz, 0.2% above nominal.
#ifdef VIDEO_MODE_50HZ
#define MODE_H_FRONT_PORCH 440
#define MODE_CEA_VIC 19
#else
#define MODE_H_FRONT_PORCH 110
#define MODE_CEA_VIC 4
#endif
#define MODE_H_SYNC_WIDTH 40
#define MODE_H_BACK_PORCH 220
#define MODE_H_ACTIVE_PIXELS 1280

#define MODE_V_FRONT_PORCH 5
#define MODE_V_SYNC_WIDTH 5
#define MODE_V_BACK_PORCH 20
#define MODE_V_ACTIVE_LINES 720

#define MODE_AVI_ASPECT_16_9 1
#define MODE_H_SYNC_POSITIVE 1
#define MODE_V_SYNC_POSITIVE 1
#define MODE_SYNC_POSITIVE 1

#endif // VIDEO_MODE_720P_RB

#ifndef MODE_HSTX_CLK_DIV
// HSTX clock: sys_clk / 1; CSR_CLKDIV=5 produces the selected pixel clock.
#define MODE_HSTX_CLK_DIV 1
#endif
#define MODE_HSTX_CSR_CLKDIV 5

#else

// 640x480 @ 60Hz (VIC 1) - Default mode
// Pixel clock: 25.2 MHz
#define MODE_H_FRONT_PORCH 16
#define MODE_H_SYNC_WIDTH 96
#define MODE_H_BACK_PORCH 48
#define MODE_H_ACTIVE_PIXELS 640

#define MODE_V_FRONT_PORCH 10
#define MODE_V_SYNC_WIDTH 2
#define MODE_V_BACK_PORCH 33
#define MODE_V_ACTIVE_LINES 480
#define MODE_CEA_VIC 1
#define MODE_AVI_ASPECT_16_9 0
#define MODE_H_SYNC_POSITIVE 0
#define MODE_V_SYNC_POSITIVE 0

#ifndef MODE_HSTX_CLK_DIV
// HSTX clock divider: 1 (stock 126 MHz sys_clk -> 25.2 MHz pixel clock).
#define MODE_HSTX_CLK_DIV 1
#endif
#define MODE_HSTX_CSR_CLKDIV 5

#endif

#define MODE_H_TOTAL_PIXELS (MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH + MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS)
#define MODE_V_TOTAL_LINES (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH + MODE_V_ACTIVE_LINES)

// Data Island placement: true if the DI fits inside the hsync pulse (480p, 240p);
// false if it must be placed in the back porch instead (720p60).
// Callers of hstx_encode_data_island pass this as the hsync_active parameter.
#if MODE_H_SYNC_WIDTH >= (W_PREAMBLE + W_DATA_ISLAND)
#define DI_IN_HSYNC 1
#define DI_HSYNC_ACTIVE true
#else
#define DI_IN_HSYNC 0
#define DI_HSYNC_ACTIVE false
#endif

// Frame dimensions (set via video_output_init)
extern uint16_t frame_width;
extern uint16_t frame_height;

// ============================================================================
// Global State
// ============================================================================

extern volatile uint32_t video_frame_count;

/**
 * True while the current vertical position is outside the active picture.
 * Background work that performs RAM-heavy preparation can use this to avoid
 * competing with active-video DMA fetches.
 */
bool video_output_in_vertical_blanking(void);

// ============================================================================
// Public Interface
// ============================================================================

typedef void (*video_output_task_fn)(void);
typedef void (*video_output_vsync_cb_t)(void);

/**
 * Scanline Callback:
 * Called by the video output system when it needs pixel data for a scanline.
 *
 * @param v_scanline The current vertical scanline (0 to MODE_V_TOTAL_LINES - 1)
 * @param active_line The current active video line (0 to MODE_V_ACTIVE_LINES - 1),
 *                    only valid if active_video is true.
 * @param line_buffer Buffer to fill with MODE_H_ACTIVE_PIXELS RGB565 pixels (packed as uint32_t pairs).
 *                    The buffer MUST be filled with (MODE_H_ACTIVE_PIXELS / 2) uint32_t words.
 *                    - 640x480 mode: 640 pixels = 320 uint32_t words
 *                    - 320x240 mode: 1280 pixels = 640 uint32_t words (4x pixel repetition)
 */
typedef void (*video_output_scanline_cb_t)(uint32_t v_scanline, uint32_t active_line, uint32_t *line_buffer);

/**
 * Scanline Pointer Callback:
 * Called by the video output system when it needs the address of already
 * prepared RGB565 pixel data for a scanline.
 *
 * @param v_scanline The current vertical scanline (0 to MODE_V_TOTAL_LINES - 1)
 * @param active_line The current active video line (0 to MODE_V_ACTIVE_LINES - 1)
 * @return Pointer to (MODE_H_ACTIVE_PIXELS / 2) uint32_t words, or NULL to use
 *         the internal line buffer. In native pixel mode the pointer refers to
 *         MODE_H_ACTIVE_PIXELS / 2 RGB565 halfwords.
 */
typedef const uint32_t *(*video_output_scanline_ptr_cb_t)(uint32_t v_scanline, uint32_t active_line);

/**
 * Initialize HSTX and DMA for video output.
 * @param width Framebuffer width in pixels (e.g., 320)
 * @param height Framebuffer height in pixels (e.g., 240)
 */
void video_output_init(uint16_t width, uint16_t height);

/**
 * Register the scanline callback.
 */
void video_output_set_scanline_callback(video_output_scanline_cb_t cb);

/**
 * Register a prepared scanline pointer callback.
 * When non-NULL, this takes precedence over the fill-buffer callback.
 */
void video_output_set_scanline_pointer_callback(video_output_scanline_ptr_cb_t cb);

/**
 * Register a VSYNC callback, called once per frame at the start of vertical sync.
 */
void video_output_set_vsync_callback(video_output_vsync_cb_t cb);

/**
 * Register a background task to run in the Core 1 loop.
 * This is typically used for audio processing.
 */
void video_output_set_background_task(video_output_task_fn task);

/**
 * Get DVI mode status.
 * @return true if DVI mode (no HDMI audio), false if HDMI mode
 */
bool video_output_get_dvi_mode(void);

/**
 * Set DVI mode.
 * When enabled, disables all HDMI Data Islands (no audio output).
 * Some monitors have trouble syncing with HDMI Data Islands.
 * Default: false (HDMI mode with audio).
 * @param enabled true for DVI mode, false for HDMI mode with audio
 */
void video_output_set_dvi_mode(bool enabled);

#include "pico_hdmi/video_output_precomposed.h"

/**
 * Native pixel mode (requires PICO_HDMI_PRECOMPOSED_ACTIVE_LINES): the
 * scanline pointer callback returns the native half-width line (e.g. 320
 * RGB565 pixels for 640-wide output); the active-data DMA runs 16-bit
 * transfers and the bus/HSTX expander double each pixel in hardware.
 */
void video_output_set_native_pixel_mode(bool enabled);

/**
 * Core 1 entry point for video output.
 * This function does not return.
 */
void video_output_core1_run(void);

/**
 * Reconfigure HDMI audio for a different sample rate.
 * Updates ACR, Audio InfoFrame, and packet timing.
 * Can be called after video_output_init() to override the default 48kHz.
 * @param sample_rate Audio sample rate in Hz (e.g. 32000, 44100, 48000)
 */
void pico_hdmi_set_audio_sample_rate(uint32_t sample_rate);

/**
 * Diagnostics, for finding out why the picture was lost (the sink shows "no signal" while the interrupts go on): a snapshot of the scanline
 * interrupt's state and of the DMA channels and the HSTX, taken from core 1 (never from the interrupt). max_gap_us and max_isr_us are the longest
 * time between two scanline interrupts and the longest one took since video_output_diag_reset_max; the template fields say whether one of the
 * command lists that never change (9 words each for the two blanking lines and the DVI active line) differs from its copy taken at start-up:
 * template_bad_index is the first word that does (-1: none), with the value it has and had.
 */
typedef struct {
    uint32_t isr_count, frame_count, max_gap_us, max_isr_us;
    uint32_t v_scanline;
    uint32_t flags;                          // 1: an active line's command list is posted, 2: the next interrupt is the pong channel's, 4: DVI mode
    struct {
        uint32_t ctrl, read_addr, transfer_count;   // the ping channel (0) and the pong channel (1)
    } ch[2];
    uint32_t dma_intr, hstx_fifo_stat, hstx_csr;
    int32_t template_bad_index;
    uint32_t template_bad_value, template_bad_was;
} video_diag_snapshot_t;

extern volatile uint32_t video_diag_isr_count, video_diag_max_gap_us, video_diag_max_isr_us;

/**
 * The slow-gap log: every time the scanline interrupt starts more than 45 us after the one before (a line is about 34 us), or finds that the other DMA
 * channel has already finished (it was a whole line late, so the chain restarted with a stale command list), it writes a record here. The phases are
 * for the program to set: what each core was doing (video_diag_phase[0] core 0, [1] core 1) and a coarse activity code, so that the record says where the delay
 * came from. video_diag_slow_count counts all of them; the table keeps the latest VIDEO_DIAG_SLOW.
 */
#define VIDEO_DIAG_SLOW 16
typedef struct {
    uint32_t isr_count;      // which interrupt it was
    uint32_t t_us;           // the timer (microseconds, 32 bits) when it started
    uint32_t gap_us;         // time since the interrupt before
    uint16_t v_scanline;
    uint8_t flags;           // 1: the other channel had finished already, 2: the pong channel's interrupt, 4: an active line's command list was posted
    uint8_t phase0, phase1, activity;
} video_diag_slow_t;
extern volatile video_diag_slow_t video_diag_slow[VIDEO_DIAG_SLOW];
extern volatile uint32_t video_diag_slow_count, video_diag_both_idle_count;
extern volatile uint8_t video_diag_phase[2], video_diag_activity;
void video_output_diag_snapshot(video_diag_snapshot_t *s);
void video_output_diag_reset_max(void);

/** Restarts the HSTX and the DMA chain from the top of a frame (core 1, thread context). */
void video_output_force_resync(void);
extern volatile uint32_t video_output_resync_count;

#endif // VIDEO_OUTPUT_H
