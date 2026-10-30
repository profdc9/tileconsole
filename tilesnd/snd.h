/* SPDX-License-Identifier: MIT */
/*
 * tilesnd: a small sound engine for console-style games. Sixteen voices, each a PCM sample player or a PSG-style
 * generator (pulse with duty cycle, triangle, noise, wavetable), with an ADSR envelope, volume and pan. Plain C with
 * no dependencies and no floating point, so the same code runs in a desktop test program and on a microcontroller.
 *
 * Two threads are involved. The game thread calls the snd_* functions below; they only queue a command. The audio
 * thread calls snd_render(), which applies the queued commands and then produces samples. The queue is lock-free for
 * one producer and one consumer, so the game needs no locks, but only ONE thread may call the snd_* functions.
 * snd_play_* and friends return false if the queue is full (nothing is lost silently).
 *
 * Volumes and levels run 0..255 (255 is unity), pans -128 (left) .. 0 (centre) .. +127 (right).
 * Frequencies are in Hz * 256, so SND_HZ(440) is concert A; snd_midi_hz() converts a MIDI note number.
 */
#ifndef SND_H
#define SND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SND_MAX_VOICES 16
#define SND_QUEUE_SIZE 64   /* commands; a power of two */
#define SND_BLOCK 64        /* frames mixed at a time */

#define SND_HZ(hz) ((uint32_t)((hz) * 256))
uint32_t snd_midi_hz(int note);   /* note 0..127; others are clamped */

typedef struct {
    const void *data;     /* 8-bit signed (int8_t) or 16-bit signed (int16_t, native endian) samples, mono */
    uint32_t length;      /* in samples */
    uint32_t rate;        /* the rate the sample was recorded at, in Hz */
    uint32_t loop_start;  /* loop_end > loop_start (and <= length) makes it loop; otherwise it plays once */
    uint32_t loop_end;
    uint8_t bits;         /* 8 or 16 */
} snd_sample_t;

typedef enum {
    SND_OFF = 0,
    SND_PCM,
    SND_PULSE,
    SND_TRIANGLE,
    SND_NOISE,         /* 15-bit LFSR, the long, hissy noise */
    SND_NOISE_SHORT,   /* the same with a 93-step cycle: a buzzy, metallic noise */
    SND_WAVE,          /* a 32-entry table of signed 8-bit samples */
} snd_kind_t;

/* ---- internal state, owned by the audio thread; read it only from there (or when no audio is running) ---- */

typedef struct {
    uint8_t kind, stage;
    uint8_t vol, duty;
    int8_t pan;
    uint16_t lfsr;
    uint32_t phase, inc;                 /* tones: 32-bit phase and its step per sample */
    uint32_t pos, frac, inc_int, inc_frac;   /* PCM: sample index and 16-bit fraction, and the step per sample */
    int32_t env, env_step, env_target;   /* envelope level, 0..65536 */
    uint32_t attack_samples, decay_samples, sustain, release_samples;   /* envelope settings; sustain is a level, 0..65536 */
    const snd_sample_t *sample;
    const int8_t *wave;
} snd_voice_t;

typedef struct {
    uint8_t op, voice, a, b;
    int32_t x, y;
    const void *p;
} snd_cmd_t;

typedef struct snd {
    uint32_t rate;
    uint32_t master;                     /* 0..256 */
    snd_voice_t voices[SND_MAX_VOICES];
    snd_cmd_t queue[SND_QUEUE_SIZE];
    uint32_t q_head, q_tail;             /* head: consumer (audio thread), tail: producer (game thread) */
    int32_t acc[2][SND_BLOCK];
} snd_t;

void snd_init(snd_t *s, uint32_t rate);

/* ---- game thread ---- */

/* Start a sample on a voice. pitch is the playback speed, 65536 = as recorded. Restarts the voice if it is playing. */
bool snd_play_pcm(snd_t *s, int voice, const snd_sample_t *sample, uint32_t pitch_q16, uint8_t vol, int8_t pan);

/* Start a tone. `duty` (SND_PULSE) is the high part of the wave, 128 = half. `wave` is for SND_WAVE only. For the noise
 * kinds the frequency is the rate the shift register is clocked at. */
bool snd_play_tone(snd_t *s, int voice, snd_kind_t kind, uint32_t hz_q8, uint8_t vol, int8_t pan, uint8_t duty,
                   const int8_t *wave);

/* Change a playing voice. snd_set_pitch is for PCM voices (65536 = as recorded), snd_set_freq for tones. */
bool snd_set_pitch(snd_t *s, int voice, uint32_t pitch_q16);
bool snd_set_freq(snd_t *s, int voice, uint32_t hz_q8);
bool snd_set_volume(snd_t *s, int voice, uint8_t vol);
bool snd_set_pan(snd_t *s, int voice, int8_t pan);
bool snd_set_duty(snd_t *s, int voice, uint8_t duty);

/*
 * The envelope used by the next note on the voice (and the current one): attack and decay and release times in
 * milliseconds, sustain as a level 0..255. The default is no attack, no decay, full sustain and a 5 ms release.
 * A sustain level of 0 makes a note end by itself once its decay is done, which is what percussive sounds want: no
 * snd_key_off is needed and the voice is free again. Any other sustain holds until snd_key_off or snd_stop.
 */
bool snd_set_adsr(snd_t *s, int voice, uint16_t attack_ms, uint16_t decay_ms, uint8_t sustain, uint16_t release_ms);

bool snd_key_off(snd_t *s, int voice);   /* start the release; the voice ends when it has faded out */
bool snd_stop(snd_t *s, int voice);      /* end it at once */
bool snd_stop_all(snd_t *s);
bool snd_set_master(snd_t *s, uint8_t vol);

/* ---- audio thread ---- */

/* Apply the queued commands and produce `frames` stereo frames: left, right, left, right, ... */
void snd_render(snd_t *s, int16_t *lr, int frames);

/* True if the voice is making sound (still playing, or fading out). Audio-thread state: for tests and diagnostics. */
bool snd_voice_active(const snd_t *s, int voice);

#endif
