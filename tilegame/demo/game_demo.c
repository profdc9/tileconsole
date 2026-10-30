/* SPDX-License-Identifier: MIT */
/*
 * A small portable "game": it uses only platform.h and tilegfx, so the same file runs on the desktop and on the board.
 * D-pad moves the player sprite, A plays a blip, B a drum (and turns the wave effect on and off), Start a chime and puts
 * the player back. A mouse draws a pointer: its buttons play the blip (left), drum (right) and chime (middle), and the wheel
 * changes the scrolling speed.
 */
#include "platform.h"
#include "scene.h"
#include "snd.h"

/* The sound engine: the game thread queues commands, the audio thread (platform.h) renders them. */
static snd_t snd;

static void audio_cb(int16_t *lr, int frames, void *user)
{
    (void)user;
    snd_render(&snd, lr, frames);
}

enum { V_BLIP, V_DRUM, V_CHIME, V_MUSIC_LEAD, V_MUSIC_BASS, V_MUSIC_HAT };

static int8_t drum[3000];   /* a generated thump: a triangle wave sliding down from 150 Hz to 40 Hz, fading out */
static snd_sample_t drum_sample = {.data = drum, .length = sizeof drum, .rate = 24000, .bits = 8};

static void make_drum(void)
{
    uint16_t phase = 0;                         /* one turn is 65536 */
    int step = 150 * 65536 / 24000;             /* phase step per sample: 150 Hz at 24 kHz */
    const int floor_step = 40 * 65536 / 24000;
    for (int i = 0; i < (int)sizeof drum; i++) {
        const int tri = (phase < 32768 ? phase : 65535 - phase) * 2 - 32768;   /* -32768..32766 */
        const int level = 127 * ((int)sizeof drum - i) / (int)sizeof drum;      /* fades out */
        drum[i] = (int8_t)(tri * level / 32768);
        phase = (uint16_t)(phase + step);
        if (step > floor_step)
            step--;
    }
}

static void play_blip(void)
{
    snd_set_adsr(&snd, V_BLIP, 0, 90, 0, 30);
    snd_play_tone(&snd, V_BLIP, SND_PULSE, snd_midi_hz(81), 200, -30, 64, NULL);
}

static void play_drum(void)
{
    snd_play_pcm(&snd, V_DRUM, &drum_sample, 65536, 255, 20);
}

static void play_chime(void)
{
    snd_set_adsr(&snd, V_CHIME, 0, 400, 0, 100);
    snd_play_tone(&snd, V_CHIME, SND_TRIANGLE, snd_midi_hz(72), 255, 0, 0, NULL);
}

#if defined(DEMO_AUTOPLAY) && !defined(DEMO_NO_MUSIC)   /* a looping jingle that keeps several voices busy, to hear and to time the engine */
static void music_step(uint32_t frame)
{
    static const uint8_t lead[16] = {72, 76, 79, 84, 79, 76, 72, 76, 74, 77, 81, 86, 81, 77, 74, 77};
    static const uint8_t bass[4] = {48, 48, 53, 55};
    if (frame % 8 == 0) {
        snd_set_adsr(&snd, V_MUSIC_LEAD, 0, 120, 40, 30);
        snd_play_tone(&snd, V_MUSIC_LEAD, SND_PULSE, snd_midi_hz(lead[(frame / 8) % 16]), 110, 25, 32 + (uint8_t)((frame / 64) % 3) * 32, NULL);
    }
    if (frame % 32 == 0) {
        snd_set_adsr(&snd, V_MUSIC_BASS, 0, 200, 100, 60);
        snd_play_tone(&snd, V_MUSIC_BASS, SND_TRIANGLE, snd_midi_hz(bass[(frame / 32) % 4]), 200, -25, 0, NULL);
    }
    if (frame % 8 == 4) {
        snd_set_adsr(&snd, V_MUSIC_HAT, 0, 25, 0, 10);
        snd_play_tone(&snd, V_MUSIC_HAT, SND_NOISE_SHORT, SND_HZ(12000), 60, 0, 0, NULL);
    }
}
#endif

int main(void)
{
    if (!plat_init())
        return 1;
    snd_init(&snd, PLAT_AUDIO_RATE);
    make_drum();
    plat_audio_start(audio_cb, NULL);

    static tg_t g;
    scene_setup(&g);
    const int player = tg_sprite_alloc(&g);
    tg_sprite_set_priority(&g, player, 0);   /* in front of the scene's own sprites */
    const int pointer = tg_sprite_alloc(&g);
    g.sprites[pointer].flags = 0;                  /* hidden until a mouse shows up */
    int px = 150, py = 110, scroll = 0, scroll_speed = 1;
    bool wave = false;

    while (!plat_quit_requested()) {
        const uint32_t frame = plat_wait_frame();
        plat_buttons_t b = plat_buttons(0), hit = plat_buttons_pressed(0);
#ifdef DEMO_AUTOPLAY   /* press buttons by itself, to try the demo without an input device */
        static const plat_buttons_t walk[4] = {PLAT_BTN_RIGHT, PLAT_BTN_DOWN, PLAT_BTN_LEFT, PLAT_BTN_UP};
        b |= walk[(frame / 45) % 4];
        if (frame % 120 == 0)
            hit |= PLAT_BTN_A, b |= PLAT_BTN_A;
        if (frame % 240 == 60)
            hit |= PLAT_BTN_B;
        if (frame % 960 == 0)
            hit |= PLAT_BTN_START;
#endif

        px += ((b & PLAT_BTN_RIGHT) ? 3 : 0) - ((b & PLAT_BTN_LEFT) ? 3 : 0);
        py += ((b & PLAT_BTN_DOWN) ? 3 : 0) - ((b & PLAT_BTN_UP) ? 3 : 0);
        if (hit & PLAT_BTN_START)
            px = 150, py = 110, play_chime();
        if (hit & PLAT_BTN_B)
            wave = !wave, play_drum();
        if (hit & PLAT_BTN_A)
            play_blip();
#if defined(DEMO_AUTOPLAY) && !defined(DEMO_NO_MUSIC)
        music_step(frame);
#endif
        const plat_mouse_t mouse = plat_mouse();
        if (mouse.pressed & PLAT_MOUSE_LEFT)
            play_blip();
        if (mouse.pressed & PLAT_MOUSE_RIGHT)
            play_drum();
        if (mouse.pressed & PLAT_MOUSE_MIDDLE)
            play_chime();
        scroll_speed += mouse.wheel;
        scroll_speed = scroll_speed < -8 ? -8 : (scroll_speed > 8 ? 8 : scroll_speed);
        scroll += scroll_speed;

        scene_animate(&g, (int)frame, scroll, wave);
        g.sprites[player] = (tg_sprite_t){.x = (int16_t)px, .y = (int16_t)py, .tile = (b & PLAT_BTN_A) ? 0 : 3,
                                          .flags = TG_SPRITE_ON, .above = 2};
        g.sprites[pointer] = (tg_sprite_t){.x = (int16_t)mouse.x, .y = (int16_t)mouse.y, .tile = 4,
                                           .flags = (uint8_t)(mouse.present ? TG_SPRITE_ON : 0), .above = 3};   /* in front of everything */
        plat_present(&g);
    }
    plat_shutdown();
    return 0;
}
