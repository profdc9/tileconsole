/* SPDX-License-Identifier: MIT */
/* Tests of the sound engine: render into buffers and check the samples. Exits non-zero on the first failure. */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snd.h"

static int checks;
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define RATE 48000
#define MAXF 60000

static int16_t out[2 * MAXF];

static void render(snd_t *s, int frames)
{
    snd_render(s, out, frames);
}

static int ch(int i, int c) { return out[2 * i + c]; }

static void fresh(snd_t *s) { snd_init(s, RATE); }

static int abs_i(int v) { return v < 0 ? -v : v; }

static void test_pulse(void)
{
    snd_t s;
    fresh(&s);
    CHECK(snd_play_tone(&s, 0, SND_PULSE, SND_HZ(480), 255, 0, 128, NULL));
    CHECK(!snd_voice_active(&s, 0));            /* nothing happens until the audio thread runs */
    render(&s, RATE);
    CHECK(snd_voice_active(&s, 0));

    int crossings = 0, hi = 0, lo = 0, positive = 0;
    for (int i = 0; i < RATE; i++) {
        const int v = ch(i, 0);
        CHECK(v == ch(i, 1));                    /* centred: both channels the same */
        CHECK(v == 32767 || v == -32767);        /* full volume is exactly unity */
        hi += v > 0;
        lo += v < 0;
        positive += v > 0;
        if (i && (v > 0) != (ch(i - 1, 0) > 0))
            crossings++;
    }
    CHECK(crossings >= 958 && crossings <= 962);    /* 480 Hz is 960 edges a second */
    CHECK(hi == lo || abs_i(hi - lo) <= 2);

    /* Duty cycle. */
    CHECK(snd_play_tone(&s, 0, SND_PULSE, SND_HZ(480), 255, 0, 64, NULL));
    render(&s, RATE);
    positive = 0;
    for (int i = 0; i < RATE; i++)
        positive += ch(i, 0) > 0;
    CHECK(positive > RATE * 24 / 100 && positive < RATE * 26 / 100);

    /* Changing the frequency keeps the phase running: the second half has twice the edges. */
    fresh(&s);
    snd_play_tone(&s, 0, SND_PULSE, SND_HZ(240), 255, 0, 128, NULL);
    render(&s, RATE / 2);
    int first = 0, second = 0;
    for (int i = 1; i < RATE / 2; i++)
        first += (ch(i, 0) > 0) != (ch(i - 1, 0) > 0);
    snd_set_freq(&s, 0, SND_HZ(480));
    render(&s, RATE / 2);
    for (int i = 1; i < RATE / 2; i++)
        second += (ch(i, 0) > 0) != (ch(i - 1, 0) > 0);
    CHECK(first >= 238 && first <= 242);
    CHECK(second >= 478 && second <= 482);
}

static void test_triangle_and_wave(void)
{
    snd_t s;
    fresh(&s);
    snd_play_tone(&s, 0, SND_TRIANGLE, SND_HZ(480), 255, 0, 0, NULL);   /* 100 samples a period */
    render(&s, 1000);
    int mx = -40000, mn = 40000;
    for (int i = 0; i < 1000; i++) {
        mx = ch(i, 0) > mx ? ch(i, 0) : mx;
        mn = ch(i, 0) < mn ? ch(i, 0) : mn;
        if (i % 100 && i % 100 < 50)
            CHECK(ch(i, 0) > ch(i - 1, 0));       /* rising for the first half... */
        if (i % 100 > 50)
            CHECK(ch(i, 0) < ch(i - 1, 0));       /* ...falling for the second */
    }
    CHECK(mx > 31000 && mn < -31000);

    /* A wavetable plays its entries in order, one per 1/32 of a period. */
    static int8_t table[32];
    for (int i = 0; i < 32; i++)
        table[i] = (int8_t)(i * 7 - 100);
    fresh(&s);
    snd_play_tone(&s, 0, SND_WAVE, SND_HZ(1500), 255, 0, 0, table);   /* 48000/1500 = 32 samples a period */
    render(&s, 320);
    for (int i = 0; i < 320; i++)
        CHECK(ch(i, 0) == table[i % 32] * 256);
    snd_play_tone(&s, 1, SND_WAVE, SND_HZ(1500), 255, 0, 0, NULL);   /* no table: queued, but ignored when applied */
    render(&s, 10);
    CHECK(!snd_voice_active(&s, 1));
}

/* A separate model of the shift register, for comparison. */
static void lfsr_model(int short_mode, int clocks, int *bits)
{
    unsigned l = 1;
    for (int k = 0; k < clocks; k++) {
        bits[k] = l & 1;
        const unsigned fb = (l ^ (l >> (short_mode ? 6 : 1))) & 1;
        l = (l >> 1) | (fb << 14);
    }
}

static void test_noise(void)
{
    static int model[40000];
    for (int mode = 0; mode < 2; mode++) {
        snd_t s;
        fresh(&s);
        snd_play_tone(&s, 0, mode ? SND_NOISE_SHORT : SND_NOISE, SND_HZ(RATE / 2), 255, 0, 0, NULL);
        render(&s, 40000);
        lfsr_model(mode, 20000, model);
        for (int k = 0; k < 20000; k++) {
            const int v = ch(2 * k, 0), w = ch(2 * k + 1, 0);
            CHECK(v == w);                                   /* the register is clocked every other sample */
            CHECK(v == (model[k] ? -32767 : 32767));
        }
    }
    /* The long register has a 32767-step cycle and the short one is far shorter. */
    lfsr_model(0, 40000, model);
    int period = 0;
    for (int p = 1; p < 40000 - 100 && !period; p++) {
        int same = 1;
        for (int k = 0; k < 100 && same; k++)
            same = model[k] == model[k + p];
        period = same ? p : 0;
    }
    CHECK(period == 32767);
    lfsr_model(1, 40000, model);
    period = 0;
    for (int p = 1; p < 1000 && !period; p++) {
        int same = 1;
        for (int k = 0; k < 300 && same; k++)
            same = model[k] == model[k + p];
        period = same ? p : 0;
    }
    CHECK(period > 0 && period < 100);
}

static void test_pcm(void)
{
    static int16_t ramp16[200];
    static int8_t ramp8[200];
    for (int i = 0; i < 200; i++) {
        ramp16[i] = (int16_t)(i * 150 - 15000);
        ramp8[i] = (int8_t)(i - 100);
    }
    snd_t s;
    fresh(&s);
    snd_sample_t sm16 = {.data = ramp16, .length = 200, .rate = RATE, .bits = 16};
    snd_sample_t sm8 = {.data = ramp8, .length = 200, .rate = RATE, .bits = 8};

    /* At the recorded rate the sample comes out exactly, and the voice ends with it. */
    snd_play_pcm(&s, 0, &sm16, 65536, 255, 0);
    render(&s, 300);
    for (int i = 0; i < 200; i++)
        CHECK(ch(i, 0) == ramp16[i] && ch(i, 1) == ramp16[i]);
    for (int i = 201; i < 300; i++)
        CHECK(ch(i, 0) == 0);
    CHECK(!snd_voice_active(&s, 0));

    snd_play_pcm(&s, 0, &sm8, 65536, 255, 0);
    render(&s, 200);
    for (int i = 0; i < 200; i++)
        CHECK(ch(i, 0) == ramp8[i] * 256);

    /* Half speed interpolates between the samples; double speed skips every other one. */
    snd_play_pcm(&s, 0, &sm16, 32768, 255, 0);
    render(&s, 380);
    for (int i = 0; i < 190; i++) {
        CHECK(ch(2 * i, 0) == ramp16[i]);
        CHECK(abs_i(ch(2 * i + 1, 0) - (ramp16[i] + ramp16[i + 1]) / 2) <= 1);
    }
    snd_play_pcm(&s, 0, &sm16, 131072, 255, 0);
    render(&s, 100);
    for (int i = 0; i < 100; i++)
        CHECK(ch(i, 0) == ramp16[2 * i]);

    /* A sample recorded at half the output rate plays at half speed by itself. */
    snd_sample_t slow = {.data = ramp16, .length = 200, .rate = RATE / 2, .bits = 16};
    snd_play_pcm(&s, 0, &slow, 65536, 255, 0);
    render(&s, 20);
    for (int i = 0; i < 10; i++)
        CHECK(ch(2 * i, 0) == ramp16[i]);

    /* Pitch can be changed while it plays. */
    snd_play_pcm(&s, 0, &sm16, 65536, 255, 0);
    render(&s, 10);
    snd_set_pitch(&s, 0, 131072);
    render(&s, 10);
    CHECK(ch(0, 0) == ramp16[10] && ch(1, 0) == ramp16[12] && ch(9, 0) == ramp16[28]);

    /* Loops: 0..7 once, then 4..7 for ever. */
    static int16_t lp[10] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    snd_sample_t loop = {.data = lp, .length = 10, .rate = RATE, .loop_start = 4, .loop_end = 8, .bits = 16};
    snd_play_pcm(&s, 0, &loop, 65536, 255, 0);
    render(&s, 40);
    for (int i = 0; i < 8; i++)
        CHECK(ch(i, 0) == lp[i]);
    for (int i = 8; i < 40; i++)
        CHECK(ch(i, 0) == lp[4 + (i - 8) % 4]);
    CHECK(snd_voice_active(&s, 0));
    /* A fast loop that jumps several samples at once still stays inside it. */
    snd_play_pcm(&s, 0, &loop, 65536 * 7, 255, 0);
    render(&s, 200);
    for (int i = 0; i < 200; i++)
        CHECK(ch(i, 0) >= 10 && ch(i, 0) <= 80);

    /* Bad samples are ignored. */
    snd_sample_t bad = {.data = ramp16, .length = 200, .rate = RATE, .bits = 12};
    snd_stop(&s, 3);
    snd_play_pcm(&s, 3, &bad, 65536, 255, 0);
    snd_play_pcm(&s, 4, NULL, 65536, 255, 0);
    render(&s, 10);
    CHECK(!snd_voice_active(&s, 3) && !snd_voice_active(&s, 4));
}

static void test_envelope(void)
{
    snd_t s;
    fresh(&s);
    /* A pulse that is high for 255/256 of a very slow period is a steady level: easy to read the envelope off. */
    snd_set_adsr(&s, 0, 10, 10, 128, 20);   /* 10 ms attack, 10 ms decay to level 128, 20 ms release */
    snd_play_tone(&s, 0, SND_PULSE, SND_HZ(1), 255, 0, 255, NULL);
    render(&s, 2400);
    CHECK(ch(0, 0) < 200);                                  /* starts from silence */
    for (int i = 1; i < 470; i++)
        CHECK(ch(i, 0) >= ch(i - 1, 0));                    /* rising through the attack (480 samples) */
    int peak = 0;
    for (int i = 470; i < 500; i++)
        peak = ch(i, 0) > peak ? ch(i, 0) : peak;
    CHECK(peak >= 32700);
    for (int i = 490; i < 950; i++)
        CHECK(ch(i, 0) <= ch(i - 1, 0));                    /* falling through the decay */
    const int sustain = 128 * 257 / 2;                      /* level 128 of 255 */
    CHECK(abs_i(ch(1200, 0) - sustain) < 300);
    CHECK(abs_i(ch(2399, 0) - ch(1200, 0)) <= 1);           /* and it holds */

    snd_key_off(&s, 0);
    render(&s, 900);                                        /* 20 ms = 960 samples to fade */
    CHECK(snd_voice_active(&s, 0));
    for (int i = 1; i < 900; i++)
        CHECK(ch(i, 0) <= ch(i - 1, 0));                    /* falling */
    CHECK(ch(899, 0) < 1500);
    render(&s, 200);
    CHECK(!snd_voice_active(&s, 0));                        /* gone after the release */
    CHECK(ch(199, 0) == 0);

    /* The default release is 5 ms, so even a plain stop of a note does not end with a step. */
    fresh(&s);
    snd_play_tone(&s, 0, SND_PULSE, SND_HZ(1), 255, 0, 255, NULL);
    render(&s, 100);
    snd_key_off(&s, 0);
    render(&s, 500);
    CHECK(!snd_voice_active(&s, 0));
    CHECK(ch(100, 0) > 0 && ch(100, 0) < 32767);

    /* A percussive note (sustain 0) frees its voice when the decay is done, with no key-off. */
    fresh(&s);
    snd_set_adsr(&s, 0, 0, 20, 0, 5);                      /* 20 ms = 960 samples of decay */
    snd_play_tone(&s, 0, SND_PULSE, SND_HZ(1), 255, 0, 255, NULL);
    render(&s, 900);
    CHECK(snd_voice_active(&s, 0));
    for (int i = 1; i < 900; i++)
        CHECK(ch(i, 0) <= ch(i - 1, 0));                    /* fading all the way */
    render(&s, 100);
    CHECK(!snd_voice_active(&s, 0));
    CHECK(ch(99, 0) == 0);
    /* With an attack in front of it, too; and with no decay time it is over at once. */
    snd_set_adsr(&s, 1, 5, 10, 0, 5);
    snd_play_tone(&s, 1, SND_TRIANGLE, SND_HZ(200), 255, 0, 0, NULL);
    render(&s, 200);
    CHECK(snd_voice_active(&s, 1));
    render(&s, 600);
    CHECK(!snd_voice_active(&s, 1));
    snd_set_adsr(&s, 2, 0, 0, 0, 5);
    snd_play_tone(&s, 2, SND_PULSE, SND_HZ(200), 255, 0, 128, NULL);
    render(&s, 10);
    CHECK(!snd_voice_active(&s, 2));
    /* A sustain above 0 still holds for ever. */
    snd_set_adsr(&s, 3, 0, 10, 1, 5);
    snd_play_tone(&s, 3, SND_PULSE, SND_HZ(200), 255, 0, 128, NULL);
    render(&s, 2000);
    CHECK(snd_voice_active(&s, 3));

    /* snd_stop is immediate. */
    fresh(&s);
    snd_play_tone(&s, 0, SND_PULSE, SND_HZ(1), 255, 0, 255, NULL);
    render(&s, 10);
    snd_stop(&s, 0);
    render(&s, 10);
    CHECK(!snd_voice_active(&s, 0) && ch(0, 0) == 0);
}

static void test_pan_volume_master(void)
{
    snd_t s;
    fresh(&s);
    for (int pan = -128; pan <= 127; pan += 1) {
        snd_play_tone(&s, 0, SND_PULSE, SND_HZ(1), 255, (int8_t)pan, 255, NULL);
        render(&s, 4);
        const int l = ch(3, 0), r = ch(3, 1);
        if (pan == -128) CHECK(r == 0 && l == 32767);
        if (pan == 127) CHECK(l == 0 && r == 32767);
        if (pan == 0) CHECK(l == 32767 && r == 32767);
        if (pan > 0) CHECK(l <= r);
        if (pan < 0) CHECK(l >= r);
        CHECK(l >= 0 && r >= 0 && l <= 32767 && r <= 32767);
    }
    snd_play_tone(&s, 0, SND_PULSE, SND_HZ(1), 255, 64, 255, NULL);
    render(&s, 4);
    CHECK(abs_i(ch(3, 0) * 2 - ch(3, 1)) < 700);            /* half way: the left side is about half */

    snd_play_tone(&s, 0, SND_PULSE, SND_HZ(1), 128, 0, 255, NULL);
    render(&s, 4);
    CHECK(abs_i(ch(3, 0) - 16500) < 200);
    snd_set_volume(&s, 0, 0);
    render(&s, 4);
    CHECK(ch(3, 0) == 0);
    snd_set_volume(&s, 0, 255);
    snd_set_master(&s, 128);
    render(&s, 4);
    CHECK(abs_i(ch(3, 0) - 16500) < 200);
    snd_set_master(&s, 255);
    snd_set_pan(&s, 0, -128);
    render(&s, 4);
    CHECK(ch(3, 1) == 0 && ch(3, 0) == 32767);
}

static void test_mixing(void)
{
    /* Two voices mixed together are exactly the sum of each on its own. */
    static int16_t samp[100], ra[4000], rb[4000], rab[4000];
    for (int i = 0; i < 100; i++)
        samp[i] = (int16_t)(i * 100 - 5000);
    snd_sample_t sm = {.data = samp, .length = 100, .rate = 24000, .loop_start = 10, .loop_end = 90, .bits = 16};
    snd_t s;
    fresh(&s);
    snd_play_tone(&s, 0, SND_TRIANGLE, SND_HZ(333), 100, -40, 0, NULL);
    snd_render(&s, ra, 2000);
    fresh(&s);
    snd_play_pcm(&s, 1, &sm, 70000, 60, 50);
    snd_render(&s, rb, 2000);
    fresh(&s);
    snd_play_tone(&s, 0, SND_TRIANGLE, SND_HZ(333), 100, -40, 0, NULL);
    snd_play_pcm(&s, 1, &sm, 70000, 60, 50);
    snd_render(&s, rab, 2000);
    for (int i = 0; i < 4000; i++)
        CHECK(rab[i] == ra[i] + rb[i]);

    /* Loud voices clip at full scale instead of wrapping round. */
    fresh(&s);
    for (int v = 0; v < SND_MAX_VOICES; v++)
        snd_play_tone(&s, v, SND_PULSE, SND_HZ(1), 255, 0, 255, NULL);
    render(&s, 100);
    for (int i = 5; i < 100; i++)
        CHECK(ch(i, 0) == 32767 && ch(i, 1) == 32767);
    snd_stop_all(&s);
    render(&s, 10);
    for (int v = 0; v < SND_MAX_VOICES; v++)
        CHECK(!snd_voice_active(&s, v));
    for (int v = 0; v < SND_MAX_VOICES; v++)
        snd_play_tone(&s, v, SND_PULSE, SND_HZ(1), 255, 0, 0, NULL);   /* duty 0: always low */
    render(&s, 100);
    for (int i = 5; i < 100; i++)
        CHECK(ch(i, 0) == -32768 && ch(i, 1) == -32768);
}

/* However the output is cut into calls, the samples are the same. */
static void test_block_independence(void)
{
    static int16_t ref[2 * 5000], got[2 * 5000];
    static int16_t samp[300];
    static int8_t wave[32];
    for (int i = 0; i < 300; i++)
        samp[i] = (int16_t)((i * 37) % 2000 - 1000);
    for (int i = 0; i < 32; i++)
        wave[i] = (int8_t)(i * 8 - 128);
    snd_sample_t sm = {.data = samp, .length = 300, .rate = 22050, .loop_start = 50, .loop_end = 250, .bits = 16};
    const int sizes[] = {1, 7, 63, 64, 65, 333, 1000, 5000};
    for (size_t k = 0; k < sizeof sizes / sizeof *sizes; k++) {
        snd_t s;
        fresh(&s);
        snd_set_adsr(&s, 2, 20, 30, 100, 40);
        snd_play_tone(&s, 0, SND_PULSE, SND_HZ(311), 200, -20, 90, NULL);
        snd_play_tone(&s, 1, SND_NOISE, SND_HZ(4000), 90, 30, 0, NULL);
        snd_play_tone(&s, 2, SND_WAVE, SND_HZ(220), 150, 0, 0, wave);
        snd_play_pcm(&s, 3, &sm, 30000, 120, 90);
        snd_play_tone(&s, 4, SND_TRIANGLE, SND_HZ(97), 255, 0, 0, NULL);
        snd_render(&s, k == 0 ? ref : got, 0);   /* applies the commands */
        int16_t *dst = k == 0 ? ref : got;
        int done = 0;
        while (done < 5000) {
            const int n = sizes[k] < 5000 - done ? sizes[k] : 5000 - done;
            snd_render(&s, dst + 2 * done, n);
            done += n;
        }
        if (k > 0)
            for (int i = 0; i < 2 * 5000; i++)
                CHECK(ref[i] == got[i]);
    }
}

/* With nothing sounding the output is silence, whatever the buffer held before. */
static void test_silence(void)
{
    snd_t s;
    fresh(&s);
    memset(out, 0x55, sizeof out);
    render(&s, 1000);
    for (int i = 0; i < 2000; i++)
        CHECK(out[i] == 0);
    CHECK(out[2000] == 0x5555);                           /* and nothing past the end was touched */

    /* A voice that ends part way through a block: sound, then silence, in the same call. */
    snd_play_tone(&s, 0, SND_PULSE, SND_HZ(1), 255, 0, 255, NULL);
    render(&s, 100);
    snd_key_off(&s, 0);
    memset(out, 0x55, sizeof out);
    render(&s, 1000);
    CHECK(!snd_voice_active(&s, 0));
    for (int i = 500; i < 2000; i++)
        CHECK(out[i] == 0);
}

static void test_queue_and_misc(void)
{
    snd_t s;
    fresh(&s);
    for (int i = 0; i < SND_QUEUE_SIZE; i++)
        CHECK(snd_set_volume(&s, 0, 10));
    CHECK(!snd_set_volume(&s, 0, 10));                    /* full: refused, not lost silently */
    render(&s, 1);
    CHECK(snd_set_volume(&s, 0, 10));                     /* drained */

    CHECK(!snd_play_tone(&s, -1, SND_PULSE, 1, 1, 0, 0, NULL));
    CHECK(!snd_play_tone(&s, SND_MAX_VOICES, SND_PULSE, 1, 1, 0, 0, NULL));
    CHECK(!snd_key_off(&s, SND_MAX_VOICES));
    CHECK(!snd_set_pan(&s, -1, 0));
    CHECK(!snd_voice_active(&s, -1) && !snd_voice_active(&s, SND_MAX_VOICES));

    /* Commands apply in the order they were sent. */
    fresh(&s);
    snd_play_tone(&s, 0, SND_PULSE, SND_HZ(1), 255, 0, 255, NULL);
    snd_set_volume(&s, 0, 0);
    snd_set_volume(&s, 0, 255);
    snd_set_volume(&s, 0, 128);
    render(&s, 4);
    CHECK(abs_i(ch(3, 0) - 16500) < 200);

    /* Playing again on a voice restarts it. */
    fresh(&s);
    snd_play_tone(&s, 0, SND_PULSE, SND_HZ(480), 255, 0, 128, NULL);
    render(&s, 30);
    snd_play_tone(&s, 0, SND_PULSE, SND_HZ(480), 255, 0, 128, NULL);
    render(&s, 3);
    CHECK(ch(0, 0) == 32767);

    CHECK(snd_midi_hz(69) == 440 * 256);
    CHECK(abs_i((int)snd_midi_hz(60) - 66976) <= 1);       /* middle C */
    CHECK(snd_midi_hz(81) == 880 * 256);
    CHECK(snd_midi_hz(-5) == snd_midi_hz(0) && snd_midi_hz(500) == snd_midi_hz(127));
    for (int n = 1; n < 128; n++)
        CHECK(snd_midi_hz(n) > snd_midi_hz(n - 1));
}

/* One thread plays the game, another the audio device; the queue must hold up. */
static snd_t shared;
static int producer_done;

static void *producer(void *arg)
{
    (void)arg;
    unsigned seed = 7;
    for (int i = 0; i < 200000; i++) {
        seed = seed * 1664525u + 1013904223u;
        const int v = (int)((seed >> 8) % SND_MAX_VOICES);
        bool ok;
        do {
            switch ((seed >> 20) % 5) {
            case 0: ok = snd_play_tone(&shared, v, SND_PULSE + (int)((seed >> 4) % 3), SND_HZ(100 + (seed >> 12) % 900), 200, 0, 128, NULL); break;
            case 1: ok = snd_set_freq(&shared, v, SND_HZ(100 + (seed >> 12) % 900)); break;
            case 2: ok = snd_set_volume(&shared, v, (uint8_t)(seed >> 3)); break;
            case 3: ok = snd_key_off(&shared, v); break;
            default: ok = snd_set_pan(&shared, v, (int8_t)(seed >> 5)); break;
            }
            if (!ok)
                sched_yield();
        } while (!ok);
    }
    while (!snd_stop_all(&shared))
        sched_yield();
    __atomic_store_n(&producer_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_threads(void)
{
    snd_init(&shared, RATE);
    __atomic_store_n(&producer_done, 0, __ATOMIC_RELEASE);
    pthread_t t;
    pthread_create(&t, NULL, producer, NULL);
    static int16_t buf[2 * 256];
    long frames = 0;
    while (!__atomic_load_n(&producer_done, __ATOMIC_ACQUIRE)) {
        snd_render(&shared, buf, 256);
        frames += 256;
    }
    pthread_join(t, NULL);
    snd_render(&shared, buf, 256);                          /* picks up the last commands, including the stop */
    for (int v = 0; v < SND_MAX_VOICES; v++)
        CHECK(!snd_voice_active(&shared, v));
    CHECK(frames > 0);
}

int main(void)
{
    test_pulse();
    test_triangle_and_wave();
    test_noise();
    test_pcm();
    test_envelope();
    test_pan_volume_master();
    test_mixing();
    test_block_independence();
    test_silence();
    test_queue_and_misc();
    test_threads();
    printf("all %d checks passed\n", checks);
    return 0;
}
