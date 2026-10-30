/* SPDX-License-Identifier: MIT */
/*
 * A small portable "game": it uses only platform.h and tilegfx, so the same file runs on the desktop and on the board.
 * D-pad moves the player sprite, A beeps, B turns the wave effect on and off, Start puts the player back.
 */
#include "platform.h"
#include "scene.h"

#define BEEP_HZ 440
#define BEEP_FRAMES (PLAT_AUDIO_RATE / 8)

/* The game loop writes beep_serial; the audio callback, which runs elsewhere, only reads it. */
static volatile uint32_t beep_serial;

static void audio_cb(int16_t *lr, int frames, void *user)
{
    (void)user;
    static uint32_t seen, left, phase;
    if (beep_serial != seen) {
        seen = beep_serial;
        left = BEEP_FRAMES;
        phase = 0;
    }
    for (int i = 0; i < frames; i++) {
        int16_t s = 0;
        if (left) {
            s = (phase & 0x8000) ? 4000 : -4000;
            phase += (uint32_t)BEEP_HZ * 65536 / PLAT_AUDIO_RATE;
            left--;
        }
        lr[2 * i] = lr[2 * i + 1] = s;
    }
}

int main(void)
{
    if (!plat_init())
        return 1;
    plat_audio_start(audio_cb, NULL);

    static tg_t g;
    scene_setup(&g);
    const int player = tg_sprite_alloc(&g);
    tg_sprite_set_priority(&g, player, 0);   /* in front of the scene's own sprites */
    int px = 150, py = 110, scroll = 0;
    bool wave = false;

    while (!plat_quit_requested()) {
        const uint32_t frame = plat_wait_frame();
        plat_buttons_t b = plat_buttons(0), hit = plat_buttons_pressed(0);
#ifdef DEMO_AUTOPLAY   /* press buttons by itself, to try the demo without an input device */
        static const plat_buttons_t walk[4] = {PLAT_BTN_RIGHT, PLAT_BTN_DOWN, PLAT_BTN_LEFT, PLAT_BTN_UP};
        b |= walk[(frame / 45) % 4];
        if (frame % 120 == 0)
            hit |= PLAT_BTN_A, b |= PLAT_BTN_A;
        if (frame % 600 == 0)
            hit |= PLAT_BTN_B;
#endif

        px += ((b & PLAT_BTN_RIGHT) ? 3 : 0) - ((b & PLAT_BTN_LEFT) ? 3 : 0);
        py += ((b & PLAT_BTN_DOWN) ? 3 : 0) - ((b & PLAT_BTN_UP) ? 3 : 0);
        if (hit & PLAT_BTN_START)
            px = 150, py = 110;
        if (hit & PLAT_BTN_B)
            wave = !wave;
        if (hit & PLAT_BTN_A)
            beep_serial++;
        scroll++;

        scene_animate(&g, (int)frame, scroll, wave);
        g.sprites[player] = (tg_sprite_t){.x = (int16_t)px, .y = (int16_t)py, .tile = (b & PLAT_BTN_A) ? 0 : 3,
                                          .flags = TG_SPRITE_ON, .above = 2};
        plat_present(&g);
    }
    plat_shutdown();
    return 0;
}
