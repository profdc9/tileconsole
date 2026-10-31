/* SPDX-License-Identifier: MIT */
/*
 * The platformer: the glue between the game rules (game.c), the picture (view.c), the sound engine and the platform layer.
 * D-pad moves, A jumps (hold it for a higher jump), B runs, Start restarts after a game over or a win.
 */
#include "game.h"
#include "platform.h"
#include "snd.h"
#include "view.h"

static game_t game;
static view_t view;
static snd_t snd;

static void audio_cb(int16_t *lr, int frames, void *user)
{
    (void)user;
    snd_render(&snd, lr, frames);
}

/* ---- sound effects: notes at given frames, and pitch sweeps ---- */

enum { V_JUMP, V_COIN, V_STOMP, V_STOMP2, V_JINGLE, V_LEAD, V_BASS };

typedef struct {
    uint16_t at;       /* frames from now */
    uint8_t voice, kind, midi, vol, duty;
    uint16_t decay_ms;
} note_t;

#define MAX_PENDING 16
static struct { uint32_t when; note_t n; bool used; } pending[MAX_PENDING];
static uint32_t now;   /* in frames */

static void queue_notes(const note_t *notes, int count)
{
    for (int i = 0; i < count; i++)
        for (int s = 0; s < MAX_PENDING; s++)
            if (!pending[s].used) {
                pending[s].used = true;
                pending[s].when = now + notes[i].at;
                pending[s].n = notes[i];
                break;
            }
}

static void play_note(const note_t *n)
{
    snd_set_adsr(&snd, n->voice, 0, n->decay_ms, 0, 20);   /* sustain 0: the note ends when it has decayed */
    snd_play_tone(&snd, n->voice, (snd_kind_t)n->kind, snd_midi_hz(n->midi), n->vol, 0, n->duty, NULL);
}

static struct { int frames; int32_t hz, step; int voice; } sweep[3];

static void start_sweep(int slot, int voice, snd_kind_t kind, uint32_t from_hz, uint32_t to_hz, int frames, uint8_t vol, uint8_t duty,
                        uint16_t decay_ms)
{
    snd_set_adsr(&snd, voice, 0, decay_ms, 0, 20);
    snd_play_tone(&snd, voice, kind, SND_HZ(from_hz), vol, 0, duty, NULL);
    sweep[slot].frames = frames;
    sweep[slot].hz = (int32_t)SND_HZ(from_hz);
    sweep[slot].step = ((int32_t)SND_HZ(to_hz) - (int32_t)SND_HZ(from_hz)) / frames;
    sweep[slot].voice = voice;
}

static void sfx_update(void)
{
    now++;
    for (int s = 0; s < MAX_PENDING; s++)
        if (pending[s].used && pending[s].when <= now) {
            play_note(&pending[s].n);
            pending[s].used = false;
        }
    for (int i = 0; i < 3; i++)
        if (sweep[i].frames > 0) {
            sweep[i].frames--;
            sweep[i].hz += sweep[i].step;
            snd_set_freq(&snd, sweep[i].voice, (uint32_t)sweep[i].hz);
        }
}

static void sfx_for_events(uint32_t ev)
{
    if (ev & EV_JUMP)
        start_sweep(0, V_JUMP, SND_PULSE, 300, 700, 8, 200, 64, 130);
    if (ev & EV_COIN) {
        static const note_t coin[] = {{0, V_COIN, SND_PULSE, 83, 180, 32, 70}, {4, V_COIN, SND_PULSE, 88, 180, 32, 220}};
        queue_notes(coin, 2);
    }
    if (ev & EV_STOMP) {
        snd_set_adsr(&snd, V_STOMP, 0, 90, 0, 10);
        snd_play_tone(&snd, V_STOMP, SND_NOISE_SHORT, SND_HZ(6000), 200, 0, 0, NULL);
        start_sweep(1, V_STOMP2, SND_PULSE, 400, 100, 8, 200, 128, 120);
    }
    if (ev & EV_DIE)
        start_sweep(2, V_JINGLE, SND_TRIANGLE, 600, 90, 40, 255, 0, 700);
    if (ev & EV_WIN) {
        static const note_t win[] = {{0, V_JINGLE, SND_PULSE, 72, 200, 64, 200},  {7, V_JINGLE, SND_PULSE, 76, 200, 64, 200},
                                     {14, V_JINGLE, SND_PULSE, 79, 200, 64, 200}, {21, V_JINGLE, SND_PULSE, 84, 200, 64, 200},
                                     {35, V_JINGLE, SND_PULSE, 88, 200, 64, 700}};
        queue_notes(win, 5);
    }
    if (ev & EV_GAMEOVER) {
        static const note_t over[] = {{0, V_JINGLE, SND_TRIANGLE, 60, 255, 0, 300}, {14, V_JINGLE, SND_TRIANGLE, 55, 255, 0, 300},
                                      {28, V_JINGLE, SND_TRIANGLE, 52, 255, 0, 300}, {42, V_JINGLE, SND_TRIANGLE, 48, 255, 0, 900}};
        queue_notes(over, 4);
    }
    if (ev & EV_RESPAWN) {
        static const note_t back[] = {{0, V_COIN, SND_PULSE, 72, 160, 64, 80}, {5, V_COIN, SND_PULSE, 79, 160, 64, 160}};
        queue_notes(back, 2);
    }
}

/* ---- a little tune while playing: a lead and a bass, one step every eight frames ---- */

static void music_step(uint32_t frame)
{
    static const uint8_t lead[32] = {76, 0, 76, 0, 72, 76, 0, 79, 0, 0, 67, 0, 0, 0, 0, 0,
                                     72, 0, 0, 67, 0, 0, 64, 0, 0, 69, 0, 71, 0, 70, 69, 0};
    static const uint8_t bass[8] = {48, 48, 43, 43, 45, 45, 41, 43};
    if (frame % 8 == 0) {
        const uint8_t n = lead[(frame / 8) % 32];
        if (n) {
            snd_set_adsr(&snd, V_LEAD, 0, 110, 0, 15);
            snd_play_tone(&snd, V_LEAD, SND_PULSE, snd_midi_hz(n), 70, 20, 64, NULL);
        }
    }
    if (frame % 32 == 0) {
        snd_set_adsr(&snd, V_BASS, 0, 380, 0, 30);
        snd_play_tone(&snd, V_BASS, SND_TRIANGLE, snd_midi_hz(bass[(frame / 32) % 8]), 120, -20, 0, NULL);
    }
}

int main(void)
{
    if (!plat_init())
        return 1;
    snd_init(&snd, PLAT_AUDIO_RATE);
    plat_audio_start(audio_cb, NULL);
    game_init(&game);
    view_init(&view);
    plat_present(&view.g);   /* something sensible to show while the first frame is being worked out */

    uint32_t music_frame = 0;
    while (!plat_quit_requested()) {
        plat_wait_frame();
        game_update(&game, plat_buttons(0), plat_buttons_pressed(0));
#ifdef GAME_DEBUG_HUD
        plat_debug_line(view.debug);
#endif
        view_draw(&view, &game);
        plat_present(&view.g);

        sfx_for_events(game.events);
        sfx_update();
        if (game.state == GS_PLAYING)
            music_step(music_frame++);
        else
            music_frame = 0;
        if (game.events & (EV_DIE | EV_WIN | EV_GAMEOVER))
            snd_stop(&snd, V_LEAD), snd_stop(&snd, V_BASS);
    }
    plat_shutdown();
    return 0;
}
