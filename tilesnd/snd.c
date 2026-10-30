/* SPDX-License-Identifier: MIT */
#include "snd.h"

#include <string.h>

#include "snd_notes.h"

/* On a microcontroller, define SND_HOT_SECTION as a section name (a string) to keep the mixing code out of flash. */
#ifdef SND_HOT_SECTION
#define SND_HOT __attribute__((section(SND_HOT_SECTION)))
#else
#define SND_HOT
#endif

#define ENV_ONE 65536

enum { ST_ATTACK, ST_DECAY, ST_SUSTAIN, ST_RELEASE };
enum { OP_PLAY_PCM = 1, OP_PLAY_TONE, OP_SET_PITCH, OP_SET_FREQ, OP_SET_VOL, OP_SET_PAN, OP_SET_DUTY, OP_SET_ADSR,
       OP_KEY_OFF, OP_STOP, OP_STOP_ALL, OP_SET_MASTER };

uint32_t snd_midi_hz(int note)
{
    return snd_midi_hz_q8[note < 0 ? 0 : (note > 127 ? 127 : note)];
}

/* 0..255 -> 0..256, so that the top value is exactly unity. */
static inline uint32_t unity(uint32_t v) { return v + (v >> 7); }

void snd_init(snd_t *s, uint32_t rate)
{
    memset(s, 0, sizeof *s);
    s->rate = rate;
    s->master = 256;
    for (int i = 0; i < SND_MAX_VOICES; i++) {
        snd_voice_t *v = &s->voices[i];
        v->sustain = ENV_ONE;
        v->release_samples = rate * 5 / 1000 ? rate * 5 / 1000 : 1;
        v->vol = 255;
        v->duty = 128;
    }
}

/* ---- the command queue: one producer (the game thread), one consumer (the audio thread) ---- */

static bool push(snd_t *s, snd_cmd_t c)
{
    const uint32_t tail = s->q_tail;
    if (tail - __atomic_load_n(&s->q_head, __ATOMIC_ACQUIRE) >= SND_QUEUE_SIZE)
        return false;
    s->queue[tail % SND_QUEUE_SIZE] = c;
    __atomic_store_n(&s->q_tail, tail + 1, __ATOMIC_RELEASE);
    return true;
}

static bool cmd(snd_t *s, int op, int voice, uint8_t a, uint8_t b, int32_t x, int32_t y, const void *p)
{
    if (op != OP_STOP_ALL && op != OP_SET_MASTER && (voice < 0 || voice >= SND_MAX_VOICES))
        return false;
    return push(s, (snd_cmd_t){.op = (uint8_t)op, .voice = (uint8_t)voice, .a = a, .b = b, .x = x, .y = y, .p = p});
}

bool snd_play_pcm(snd_t *s, int voice, const snd_sample_t *sample, uint32_t pitch_q16, uint8_t vol, int8_t pan)
{
    return cmd(s, OP_PLAY_PCM, voice, vol, (uint8_t)pan, (int32_t)pitch_q16, 0, sample);
}

bool snd_play_tone(snd_t *s, int voice, snd_kind_t kind, uint32_t hz_q8, uint8_t vol, int8_t pan, uint8_t duty,
                   const int8_t *wave)
{
    /* a holds the kind and b the duty; volume and pan are packed into y. */
    if (voice < 0 || voice >= SND_MAX_VOICES)
        return false;
    return push(s, (snd_cmd_t){.op = OP_PLAY_TONE, .voice = (uint8_t)voice, .a = (uint8_t)kind, .b = duty,
                               .x = (int32_t)hz_q8, .y = (int32_t)(vol | ((uint32_t)(uint8_t)pan << 8)), .p = wave});
}

bool snd_set_pitch(snd_t *s, int voice, uint32_t pitch_q16) { return cmd(s, OP_SET_PITCH, voice, 0, 0, (int32_t)pitch_q16, 0, NULL); }
bool snd_set_freq(snd_t *s, int voice, uint32_t hz_q8) { return cmd(s, OP_SET_FREQ, voice, 0, 0, (int32_t)hz_q8, 0, NULL); }
bool snd_set_volume(snd_t *s, int voice, uint8_t vol) { return cmd(s, OP_SET_VOL, voice, vol, 0, 0, 0, NULL); }
bool snd_set_pan(snd_t *s, int voice, int8_t pan) { return cmd(s, OP_SET_PAN, voice, (uint8_t)pan, 0, 0, 0, NULL); }
bool snd_set_duty(snd_t *s, int voice, uint8_t duty) { return cmd(s, OP_SET_DUTY, voice, duty, 0, 0, 0, NULL); }
bool snd_key_off(snd_t *s, int voice) { return cmd(s, OP_KEY_OFF, voice, 0, 0, 0, 0, NULL); }
bool snd_stop(snd_t *s, int voice) { return cmd(s, OP_STOP, voice, 0, 0, 0, 0, NULL); }
bool snd_stop_all(snd_t *s) { return cmd(s, OP_STOP_ALL, 0, 0, 0, 0, 0, NULL); }
bool snd_set_master(snd_t *s, uint8_t vol) { return cmd(s, OP_SET_MASTER, 0, vol, 0, 0, 0, NULL); }

bool snd_set_adsr(snd_t *s, int voice, uint16_t attack_ms, uint16_t decay_ms, uint8_t sustain, uint16_t release_ms)
{
    return cmd(s, OP_SET_ADSR, voice, sustain, 0, (int32_t)(attack_ms | ((uint32_t)decay_ms << 16)), release_ms, NULL);
}

/* ---- envelope ---- */

SND_HOT static void env_enter_decay(snd_voice_t *v)
{
    if (v->decay_samples == 0 || v->sustain >= ENV_ONE) {
        v->env = (int32_t)v->sustain;
        v->stage = ST_SUSTAIN;
        v->env_step = 0;
        return;
    }
    int32_t step = (int32_t)((ENV_ONE - v->sustain) / v->decay_samples);
    v->stage = ST_DECAY;
    v->env_step = -(step ? step : 1);
    v->env_target = (int32_t)v->sustain;
}

SND_HOT static void env_start(snd_voice_t *v)
{
    if (v->attack_samples == 0) {
        v->env = ENV_ONE;
        env_enter_decay(v);
        return;
    }
    int32_t step = (int32_t)(ENV_ONE / v->attack_samples);
    v->env = 0;
    v->stage = ST_ATTACK;
    v->env_step = step ? step : 1;
    v->env_target = ENV_ONE;
}

SND_HOT static void env_release(snd_voice_t *v)
{
    int32_t step = v->env / (int32_t)v->release_samples;
    v->stage = ST_RELEASE;
    v->env_step = -(step ? step : 1);
    v->env_target = 0;
}

/* The envelope has reached its target. */
SND_HOT static void env_advance(snd_voice_t *v)
{
    switch (v->stage) {
    case ST_ATTACK:
        v->env = ENV_ONE;
        env_enter_decay(v);
        break;
    case ST_DECAY:
        v->env = (int32_t)v->sustain;
        v->stage = ST_SUSTAIN;
        v->env_step = 0;
        break;
    default:   /* ST_RELEASE: the note is over */
        v->env = 0;
        v->env_step = 0;
        v->kind = SND_OFF;
        break;
    }
}

/* ---- applying commands (audio thread) ---- */

static uint32_t tone_inc(const snd_t *s, uint32_t hz_q8)
{
    const uint64_t inc = ((uint64_t)hz_q8 << 24) / s->rate;
    return inc > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)inc;
}

static void pcm_set_pitch(const snd_t *s, snd_voice_t *v, uint32_t pitch_q16)
{
    const uint64_t inc = (uint64_t)v->sample->rate * pitch_q16 / s->rate;   /* 16.16 samples per output frame */
    v->inc_int = (uint32_t)(inc >> 16);
    v->inc_frac = (uint32_t)(inc & 0xFFFF);
}

SND_HOT static void apply(snd_t *s, const snd_cmd_t *c)
{
    snd_voice_t *v = &s->voices[c->voice % SND_MAX_VOICES];
    switch (c->op) {
    case OP_PLAY_PCM: {
        const snd_sample_t *sm = c->p;
        if (!sm || !sm->data || sm->length == 0 || (sm->bits != 8 && sm->bits != 16))
            return;
        v->kind = SND_PCM;
        v->sample = sm;
        v->pos = v->frac = 0;
        pcm_set_pitch(s, v, (uint32_t)c->x);
        v->vol = c->a;
        v->pan = (int8_t)c->b;
        env_start(v);
        break;
    }
    case OP_PLAY_TONE: {
        const unsigned kind = c->a;
        if (kind < SND_PULSE || kind > SND_WAVE || (kind == SND_WAVE && !c->p))
            return;
        v->kind = (uint8_t)kind;
        v->phase = 0;
        v->lfsr = 1;
        v->inc = tone_inc(s, (uint32_t)c->x);
        v->duty = c->b;
        v->vol = (uint8_t)(c->y & 0xFF);
        v->pan = (int8_t)((c->y >> 8) & 0xFF);
        v->wave = c->p;
        env_start(v);
        break;
    }
    case OP_SET_PITCH:
        if (v->kind == SND_PCM)
            pcm_set_pitch(s, v, (uint32_t)c->x);
        break;
    case OP_SET_FREQ:
        if (v->kind >= SND_PULSE)
            v->inc = tone_inc(s, (uint32_t)c->x);
        break;
    case OP_SET_VOL: v->vol = c->a; break;
    case OP_SET_PAN: v->pan = (int8_t)c->a; break;
    case OP_SET_DUTY: v->duty = c->a; break;
    case OP_SET_ADSR: {
        const uint32_t attack_ms = (uint32_t)c->x & 0xFFFF, decay_ms = (uint32_t)c->x >> 16, release_ms = (uint32_t)c->y;
        v->attack_samples = attack_ms * s->rate / 1000;
        v->decay_samples = decay_ms * s->rate / 1000;
        v->sustain = c->a == 255 ? ENV_ONE : (uint32_t)c->a * 257;
        v->release_samples = release_ms * s->rate / 1000 ? release_ms * s->rate / 1000 : 1;
        break;
    }
    case OP_KEY_OFF:
        if (v->kind != SND_OFF && v->stage != ST_RELEASE)
            env_release(v);
        break;
    case OP_STOP:
        v->kind = SND_OFF;
        break;
    case OP_STOP_ALL:
        for (int i = 0; i < SND_MAX_VOICES; i++)
            s->voices[i].kind = SND_OFF;
        break;
    case OP_SET_MASTER:
        s->master = unity(c->a);
        break;
    }
}

/* ---- generators (audio thread) ---- */

/* The voice's gain for one sample is raw * env * volume * pan; this takes the envelope level and advances it. */
#define STEP_ENV()                                                                       \
    const int32_t amp = v->env >> 1; /* Q15 */                                           \
    if (v->env_step) {                                                                   \
        v->env += v->env_step;                                                           \
        if (v->env_step > 0 ? v->env >= v->env_target : v->env <= v->env_target) {       \
            env_advance(v);                                                              \
            ended = v->kind == SND_OFF;                                                  \
        }                                                                                \
    }

#define MIX(raw)                                                                         \
    do {                                                                                 \
        const int32_t m = ((raw) * amp) >> 15;                                           \
        accl[i] += (m * (int32_t)gl) >> 16;                                              \
        accr[i] += (m * (int32_t)gr) >> 16;                                              \
    } while (0)

static inline int32_t pcm_fetch(const snd_sample_t *sm, uint32_t i)
{
    return sm->bits == 8 ? (int32_t)((const int8_t *)sm->data)[i] * 256 : (int32_t)((const int16_t *)sm->data)[i];
}

SND_HOT static void render_voice(snd_t *s, snd_voice_t *v, int n)
{
    int32_t *accl = s->acc[0], *accr = s->acc[1];
    const uint32_t vol = unity(v->vol);
    const int pan = v->pan;
    const uint32_t panl = pan > 0 ? (uint32_t)(127 - pan) * 2 : 255, panr = pan < 0 ? (uint32_t)(128 + pan) * 2 : 255;
    const uint32_t gl = vol * unity(panl), gr = vol * unity(panr);   /* Q16, 65536 = unity */
    bool ended = false;

    switch (v->kind) {
    case SND_PULSE: {
        uint32_t ph = v->phase;
        const uint32_t inc = v->inc, th = (uint32_t)v->duty << 24;
        for (int i = 0; i < n && !ended; i++) {
            const int32_t raw = ph < th ? 32767 : -32767;
            ph += inc;
            STEP_ENV();
            MIX(raw);
        }
        v->phase = ph;
        break;
    }
    case SND_TRIANGLE: {
        uint32_t ph = v->phase;
        const uint32_t inc = v->inc;
        for (int i = 0; i < n && !ended; i++) {
            const uint32_t p = ph >> 16, t = p < 0x8000 ? p : 0xFFFF - p;
            const int32_t raw = (int32_t)(t * 2) - 32768;
            ph += inc;
            STEP_ENV();
            MIX(raw);
        }
        v->phase = ph;
        break;
    }
    case SND_NOISE:
    case SND_NOISE_SHORT: {
        uint32_t ph = v->phase;
        uint32_t l = v->lfsr;
        const uint32_t inc = v->inc, tap = v->kind == SND_NOISE ? 1 : 6;
        for (int i = 0; i < n && !ended; i++) {
            const int32_t raw = (l & 1) ? -32767 : 32767;
            const uint32_t next = ph + inc;
            if (next < ph)   /* the phase wrapped: clock the shift register */
                l = (l >> 1) | ((((l ^ (l >> tap)) & 1)) << 14);
            ph = next;
            STEP_ENV();
            MIX(raw);
        }
        v->phase = ph;
        v->lfsr = (uint16_t)l;
        break;
    }
    case SND_WAVE: {
        uint32_t ph = v->phase;
        const uint32_t inc = v->inc;
        const int8_t *w = v->wave;
        for (int i = 0; i < n && !ended; i++) {
            const int32_t raw = w[ph >> 27] * 256;
            ph += inc;
            STEP_ENV();
            MIX(raw);
        }
        v->phase = ph;
        break;
    }
    case SND_PCM: {
        const snd_sample_t *sm = v->sample;
        const uint32_t len = sm->length;
        const bool loops = sm->loop_end > sm->loop_start && sm->loop_end <= len;
        const uint32_t end = loops ? sm->loop_end : len;
        uint32_t pos = v->pos, frac = v->frac;
        for (int i = 0; i < n && !ended; i++) {
            if (pos >= end) {
                if (!loops) {
                    v->kind = SND_OFF;
                    break;
                }
                pos = sm->loop_start + (pos - end) % (sm->loop_end - sm->loop_start);
            }
            uint32_t next = pos + 1;
            if (next >= end)
                next = loops ? sm->loop_start : pos;   /* a one-shot holds its last sample */
            const int32_t s0 = pcm_fetch(sm, pos), s1 = pcm_fetch(sm, next);
            const int32_t raw = s0 + (((s1 - s0) * (int32_t)(frac >> 1)) >> 15);
            frac += v->inc_frac;
            pos += v->inc_int + (frac >> 16);
            frac &= 0xFFFF;
            STEP_ENV();
            MIX(raw);
        }
        v->pos = pos;
        v->frac = frac;
        break;
    }
    default:
        break;
    }
}

SND_HOT void snd_render(snd_t *s, int16_t *lr, int frames)
{
    uint32_t head = s->q_head;
    const uint32_t tail = __atomic_load_n(&s->q_tail, __ATOMIC_ACQUIRE);
    while (head != tail) {
        apply(s, &s->queue[head % SND_QUEUE_SIZE]);
        head++;
    }
    __atomic_store_n(&s->q_head, head, __ATOMIC_RELEASE);

    while (frames > 0) {
        const int n = frames < SND_BLOCK ? frames : SND_BLOCK;

        bool any = false;
        for (int i = 0; i < SND_MAX_VOICES; i++)
            any |= s->voices[i].kind != SND_OFF;
        if (!any) {   /* nothing sounding: silence, without mixing or clipping anything */
            for (int i = 0; i < 2 * n; i++) {
                lr[i] = 0;
                __asm__ volatile("" ::: "memory");
            }
            lr += 2 * n;
            frames -= n;
            continue;
        }

        /* Not memset: on a microcontroller that is a call into flash. The barrier stops the compiler turning this into one. */
        for (int i = 0; i < SND_BLOCK; i++) {
            s->acc[0][i] = 0;
            s->acc[1][i] = 0;
            __asm__ volatile("" ::: "memory");
        }
        for (int i = 0; i < SND_MAX_VOICES; i++)
            if (s->voices[i].kind != SND_OFF)
                render_voice(s, &s->voices[i], n);
        /* The sum of 16 voices is at most 16 * 32768 (20 bits) and the master is at most 256 (9 bits): 32 bits is enough. */
        const int32_t master = (int32_t)s->master;
        for (int i = 0; i < n; i++)
            for (int ch = 0; ch < 2; ch++) {
                int32_t v = (s->acc[ch][i] * master) >> 8;
                v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
                lr[2 * i + ch] = (int16_t)v;
            }
        lr += 2 * n;
        frames -= n;
    }
}

bool snd_voice_active(const snd_t *s, int voice)
{
    return voice >= 0 && voice < SND_MAX_VOICES && s->voices[voice].kind != SND_OFF;
}
