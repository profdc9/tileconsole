/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
/* Headless tests of the SDL backend, using SDL's dummy video and audio drivers. */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat_sdl.h"
#include "platform.h"

static int checks;
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static void push_key(SDL_Keycode key, bool down, int repeat)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.keysym.sym = key;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.repeat = (Uint8)repeat;
    SDL_PushEvent(&e);
}

static void test_timing(void)
{
    const uint32_t f0 = plat_wait_frame();
    CHECK(plat_frame() == f0);
    const uint32_t t0 = plat_millis();
    for (int i = 1; i <= 30; i++)
        CHECK(plat_wait_frame() == f0 + (uint32_t)i);
    const uint32_t dt = plat_millis() - t0;
    CHECK(dt >= 480 && dt <= 560);          /* 30 frames at 60 Hz is 500 ms */
    CHECK(plat_frame() == f0 + 30);
    const uint32_t a = plat_millis();
    SDL_Delay(5);
    CHECK(plat_millis() >= a);
}

static void test_input(void)
{
    plat_wait_frame();
    CHECK(plat_buttons(0) == 0 && plat_buttons_pressed(0) == 0);

    push_key(SDLK_z, true, 0);
    push_key(SDLK_RIGHT, true, 0);
    push_key(SDLK_x, true, 1);            /* an auto-repeat event for a key that was never pressed: ignored */
    plat_wait_frame();
    CHECK(plat_buttons(0) == (PLAT_BTN_A | PLAT_BTN_RIGHT));
    CHECK(plat_buttons_pressed(0) == (PLAT_BTN_A | PLAT_BTN_RIGHT));
    CHECK(plat_buttons_released(0) == 0);

    plat_wait_frame();                    /* still held, but no longer "just pressed" */
    CHECK(plat_buttons(0) == (PLAT_BTN_A | PLAT_BTN_RIGHT));
    CHECK(plat_buttons_pressed(0) == 0);

    push_key(SDLK_z, false, 0);
    push_key(SDLK_RETURN, true, 0);
    plat_wait_frame();
    CHECK(plat_buttons(0) == (PLAT_BTN_RIGHT | PLAT_BTN_START));
    CHECK(plat_buttons_released(0) == PLAT_BTN_A);
    CHECK(plat_buttons_pressed(0) == PLAT_BTN_START);

    /* A press and release that both happen within one frame is not lost to the edge detection of the next. */
    push_key(SDLK_RIGHT, false, 0);
    push_key(SDLK_RETURN, false, 0);
    push_key(SDLK_UP, true, 0);
    push_key(SDLK_UP, false, 0);
    plat_wait_frame();
    CHECK(plat_buttons(0) == 0);
    CHECK(plat_buttons_released(0) == (PLAT_BTN_RIGHT | PLAT_BTN_START));

    /* The sampled state is stable between frames. */
    push_key(SDLK_s, true, 0);
    CHECK(plat_buttons(0) == 0);
    plat_wait_frame();
    CHECK(plat_buttons(0) == PLAT_BTN_Y);
    push_key(SDLK_s, false, 0);
    CHECK(plat_buttons(0) == PLAT_BTN_Y);
    plat_wait_frame();

    /* No other players, and bad player numbers, give nothing. */
    CHECK(plat_buttons(1) == 0 && plat_buttons(-1) == 0 && plat_buttons(PLAT_MAX_PLAYERS) == 0);
    CHECK(plat_sdl_button_for_key(SDLK_q) == 0);
    CHECK(plat_sdl_button_for_key(SDLK_RSHIFT) == PLAT_BTN_SELECT);
}

static void test_present(void)
{
    static tg_t g;
    tg_init(&g);
    g.palette[0] = 0xF800;
    plat_present(&g);
    const uint16_t before = plat_sdl_framebuffer()[0];
    CHECK(before != 0xF800);              /* not shown until the next frame */
    plat_wait_frame();
    const uint16_t *fb = plat_sdl_framebuffer();
    for (int i = 0; i < TG_WIDTH * TG_HEIGHT; i++)
        CHECK(fb[i] == 0xF800);

    g.palette[0] = 0x07E0;                /* changing our copy does not change what was presented */
    plat_wait_frame();
    CHECK(plat_sdl_framebuffer()[0] == 0xF800);
    plat_present(&g);
    plat_wait_frame();
    CHECK(plat_sdl_framebuffer()[TG_WIDTH * TG_HEIGHT - 1] == 0x07E0);

    /* A tile layer reaches the screen with its scroll applied. */
    static uint8_t tile[64];
    memset(tile, 5, sizeof tile);
    static uint16_t map[4] = {0, 0, 0, 0};
    g.palette[5] = 0x001F;
    g.layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = 3, .depth = 8, .map_w_log2 = 1, .map_h_log2 = 1,
                               .map = map, .tiles = tile};
    plat_present(&g);
    plat_wait_frame();
    CHECK(plat_sdl_framebuffer()[100 * TG_WIDTH + 100] == 0x001F);
}

static volatile long audio_frames, audio_calls;
static volatile int audio_user_ok;
static int audio_marker;
static void audio_fn(int16_t *lr, int frames, void *user)
{
    audio_user_ok = user == &audio_marker;
    for (int i = 0; i < frames; i++) {
        lr[2 * i] = 100;
        lr[2 * i + 1] = -100;
    }
    audio_frames += frames;
    audio_calls++;
}

static void test_audio(void)
{
    if (!plat_audio_start(audio_fn, &audio_marker)) {
        printf("(no audio device: audio test skipped)\n");
        return;
    }
    const uint32_t t0 = plat_millis();
    const long f0 = audio_frames;
    for (int i = 0; i < 30; i++)
        plat_wait_frame();
    const uint32_t dt = plat_millis() - t0;
    const long got = audio_frames - f0;
    CHECK(audio_calls > 0 && audio_user_ok);
    /* The right number of frames for the time that passed, to within a couple of buffers. */
    const long want = (long)dt * PLAT_AUDIO_RATE / 1000;
    CHECK(got > want * 7 / 10 && got < want * 13 / 10);

    CHECK(plat_audio_start(NULL, NULL));  /* stop */
    SDL_Delay(50);
    const long stopped = audio_frames;
    for (int i = 0; i < 6; i++)
        plat_wait_frame();
    CHECK(audio_frames == stopped);
}

static void test_quit(void)
{
    CHECK(!plat_quit_requested());
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);
    plat_wait_frame();
    CHECK(plat_quit_requested());
}

int main(void)
{
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    CHECK(plat_init());
    test_timing();
    test_input();
    test_present();
    test_audio();
    test_quit();
    plat_shutdown();
    printf("all %d checks passed\n", checks);
    return 0;
}
