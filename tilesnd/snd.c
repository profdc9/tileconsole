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
       OP_KEY_OFF, OP_STOP, OP_STOP_ALL, OP_SET_MASTER, OP_MUSIC_PLAY, OP_MUSIC_STOP, OP_MUSIC_VOL };

SND_HOT uint32_t snd_midi_hz(int note)
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
    s->music_vol = 255;
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
    if (op != OP_STOP_ALL && op != OP_SET_MASTER && op < OP_MUSIC_PLAY && (voice < 0 || voice >= SND_MAX_VOICES))
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

int snd_room(const snd_t *s)
{
    return SND_QUEUE_SIZE - (int)(s->q_tail - __atomic_load_n(&s->q_head, __ATOMIC_ACQUIRE));
}

uint32_t snd_mark(const snd_t *s) { return s->q_tail; }

bool snd_reached(const snd_t *s, uint32_t mark)
{
    return (int32_t)(__atomic_load_n(&s->done, __ATOMIC_ACQUIRE) - mark) >= 0;
}

uint32_t snd_sounding(const snd_t *s) { return __atomic_load_n(&s->sounding, __ATOMIC_ACQUIRE); }

bool snd_music_play(snd_t *s, const snd_song_t *song, int first_voice)
{
    if (!song || first_voice < 0 || first_voice >= SND_MAX_VOICES)
        return false;
    return cmd(s, OP_MUSIC_PLAY, 0, (uint8_t)first_voice, 0, 0, 0, song);
}

bool snd_music_stop(snd_t *s) { return cmd(s, OP_MUSIC_STOP, 0, 0, 0, 0, 0, NULL); }
bool snd_music_volume(snd_t *s, uint8_t vol) { return cmd(s, OP_MUSIC_VOL, 0, vol, 0, 0, 0, NULL); }
bool snd_music_playing(const snd_t *s) { return __atomic_load_n(&s->music_on, __ATOMIC_ACQUIRE) != 0; }

void snd_instrument_default(snd_instrument_t *in)
{
    memset(in, 0, sizeof *in);
    in->kind = SND_PULSE;
    in->duty = 128;
    in->vol = 255;
    in->sustain = 255;
    in->release_ms = 5;
    in->root = 60;
}

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
        if (v->sustain == 0)   /* nothing left to hold: the note is over */
            v->kind = SND_OFF;
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
        if (v->sustain == 0)   /* decayed to nothing: the note is over, and the voice is free */
            v->kind = SND_OFF;
        break;
    default:   /* ST_RELEASE: the note is over */
        v->env = 0;
        v->env_step = 0;
        v->kind = SND_OFF;
        break;
    }
}

/* ---- applying commands (audio thread) ---- */

SND_HOT static uint32_t tone_inc(const snd_t *s, uint32_t hz_q8)
{
    const uint64_t inc = ((uint64_t)hz_q8 << 24) / s->rate;
    return inc > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)inc;
}

SND_HOT static void pcm_set_pitch(const snd_t *s, snd_voice_t *v, uint32_t pitch_q16)
{
    const uint64_t inc = (uint64_t)v->sample->rate * pitch_q16 / s->rate;   /* 16.16 samples per output frame */
    v->inc_int = (uint32_t)(inc >> 16);
    v->inc_frac = (uint32_t)(inc & 0xFFFF);
}

/* Starting notes and setting envelopes: for the commands from the game thread and for the music (in the hot section too: the music starts notes
 * in the middle of a block, on the thread that may also feed a display). */
SND_HOT static void voice_pcm(const snd_t *s, snd_voice_t *v, const snd_sample_t *sm, uint32_t pitch_q16, uint8_t vol, int8_t pan)
{
    if (!sm || !sm->data || sm->length == 0 || (sm->bits != 8 && sm->bits != 16))
        return;
    v->kind = SND_PCM;
    v->sample = sm;
    v->pos = v->frac = 0;
    pcm_set_pitch(s, v, pitch_q16);
    v->vol = vol;
    v->pan = pan;
    env_start(v);
}

SND_HOT static void voice_tone(const snd_t *s, snd_voice_t *v, unsigned kind, uint32_t hz_q8, uint8_t vol, int8_t pan, uint8_t duty, const int8_t *wave)
{
    if (kind < SND_PULSE || kind > SND_WAVE || (kind == SND_WAVE && !wave))
        return;
    v->kind = (uint8_t)kind;
    v->phase = 0;
    v->lfsr = 1;
    v->inc = tone_inc(s, hz_q8);
    v->duty = duty;
    v->vol = vol;
    v->pan = pan;
    v->wave = wave;
    env_start(v);
}

SND_HOT static void voice_adsr(const snd_t *s, snd_voice_t *v, uint32_t attack_ms, uint32_t decay_ms, uint8_t sustain, uint32_t release_ms)
{
    v->attack_samples = attack_ms * s->rate / 1000;
    v->decay_samples = decay_ms * s->rate / 1000;
    v->sustain = sustain == 255 ? ENV_ONE : (uint32_t)sustain * 257;
    v->release_samples = release_ms * s->rate / 1000 ? release_ms * s->rate / 1000 : 1;
}

/* ---- music (audio thread) ---- */

/* A speed of 2^(d/12) as 16.16: the pitch of a sample played d semitones from its root note. */
SND_HOT static uint32_t semitones_q16(int d)
{
    static const uint32_t ratio[12] = {65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715};
    d = d < -96 ? -96 : d > 60 ? 60 : d;
    const int octave = d >= 0 ? d / 12 : -((-d + 11) / 12);
    const uint32_t r = ratio[d - 12 * octave];
    return octave >= 0 ? r << octave : r >> -octave;
}

SND_HOT static uint32_t music_spt(const snd_t *s, uint32_t bpm)
{
    return (uint32_t)(((uint64_t)s->rate * 60u << 16) / ((uint64_t)bpm * SND_TICKS_PER_QUARTER));
}

/* Every voice the song's tracks play on falls silent. */
SND_HOT static void music_silence(snd_t *s)
{
    const snd_song_t *song = s->song;
    if (!song)
        return;
    for (int i = 0; i < song->tracks && s->music_first + i < SND_MAX_VOICES; i++)
        s->voices[s->music_first + i].kind = SND_OFF;
}

SND_HOT static void music_note(snd_t *s, int i, const snd_event_t *ev)
{
    snd_track_state_t *st = &s->track[i];
    const snd_instrument_t *in = &s->song->instrument[st->inst % SND_MAX_INSTRUMENTS];
    snd_voice_t *v = &s->voices[s->music_first + i];
    const uint8_t vol = (uint8_t)(((uint32_t)st->vol * in->vol / 255u) * s->music_vol / 255u);
    const int8_t pan = st->pan == SND_PAN_INSTRUMENT ? in->pan : (int8_t)st->pan;
    const int note = ev->a;
    if ((ev->flags & SND_EV_SLUR) && v->kind != SND_OFF && v->stage != ST_RELEASE) {   /* no new attack: the pitch moves */
        if (v->kind == SND_PCM)
            pcm_set_pitch(s, v, semitones_q16(note - in->root));
        else
            v->inc = tone_inc(s, snd_midi_hz(note));
        v->vol = vol;
        v->pan = pan;
        return;
    }
    voice_adsr(s, v, in->attack_ms, in->decay_ms, in->sustain, in->release_ms);
    if (in->kind == SND_PCM)
        voice_pcm(s, v, in->sample, semitones_q16(note - in->root), vol, pan);
    else
        voice_tone(s, v, in->kind, snd_midi_hz(note), vol, pan, in->duty, in->wave);
}

/* Runs track i's events until one that takes time (or the track's end). */
SND_HOT static void music_run(snd_t *s, int i)
{
    const snd_track_t *tr = &s->song->track[i];
    snd_track_state_t *st = &s->track[i];
    for (int guard = 0; !st->ended && guard < 4096; guard++) {
        if (st->idx >= tr->count) {
            if (tr->loop < 0 || (uint32_t)tr->loop >= tr->count) {
                st->ended = 1;
                break;
            }
            st->idx = (uint32_t)tr->loop;
            st->depth = 0;   /* (L is outside every loop) */
        }
        const snd_event_t *ev = &tr->events[st->idx++];
        switch (ev->op) {
        case SND_EV_NOTE:
            music_note(s, i, ev);
            st->off = ev->gate;
            break;
        case SND_EV_INST: st->inst = ev->a; break;
        case SND_EV_VOL: st->vol = ev->a; break;
        case SND_EV_PAN: st->pan = (int8_t)ev->a; break;
        case SND_EV_TEMPO:
            if (ev->gate)
                s->music_spt = music_spt(s, ev->gate);
            break;
        case SND_EV_LOOP_START:
            if (st->depth < SND_LOOP_DEPTH)
                st->left[st->depth++] = ev->a ? (uint8_t)(ev->a - 1) : 0;
            break;
        case SND_EV_LOOP_END:
            if (st->depth && st->left[st->depth - 1]) {
                st->left[st->depth - 1]--;
                if (ev->gate <= st->idx)
                    st->idx -= ev->gate;
            } else if (st->depth) {
                st->depth--;
            }
            break;
        default:   /* SND_EV_REST */
            break;
        }
        if (ev->len) {
            st->wait = ev->len;
            break;
        }
    }
}

SND_HOT static void music_start(snd_t *s, const snd_song_t *song, int first)
{
    music_silence(s);
    s->song = song;
    s->music_first = (uint8_t)first;
    s->music_spt = music_spt(s, 120);
    s->music_acc = 0;
    for (int i = 0; i < SND_MAX_VOICES; i++) {   /* (field by field: a compound literal is a call to memset, which is in flash on a microcontroller) */
        snd_track_state_t *st = &s->track[i];
        st->idx = st->wait = st->off = 0;
        st->inst = 0;
        st->vol = 255;
        st->ended = i >= song->tracks || first + i >= SND_MAX_VOICES;
        st->depth = 0;
        st->pan = SND_PAN_INSTRUMENT;
        for (int k = 0; k < SND_LOOP_DEPTH; k++) {
            st->left[k] = 0;
            __asm__ volatile("" ::: "memory");
        }
    }
    for (int i = 0; i < song->tracks && first + i < SND_MAX_VOICES; i++)
        music_run(s, i);
}

SND_HOT static void music_tick(snd_t *s)
{
    const snd_song_t *song = s->song;
    for (int i = 0; i < song->tracks && s->music_first + i < SND_MAX_VOICES; i++) {
        snd_track_state_t *st = &s->track[i];
        if (st->off && --st->off == 0) {
            snd_voice_t *v = &s->voices[s->music_first + i];
            if (v->kind != SND_OFF && v->stage != ST_RELEASE)
                env_release(v);
        }
        if (st->ended)
            continue;
        if (st->wait && --st->wait)
            continue;
        music_run(s, i);
    }
}

/* The music moves on by `frames` frames (before they are mixed). */
SND_HOT static void music_advance(snd_t *s, int frames)
{
    s->music_acc += (uint32_t)frames << 16;
    while (s->song && s->music_acc >= s->music_spt) {
        s->music_acc -= s->music_spt;
        music_tick(s);
    }
}

SND_HOT static void apply(snd_t *s, const snd_cmd_t *c)
{
    snd_voice_t *v = &s->voices[c->voice % SND_MAX_VOICES];
    switch (c->op) {
    case OP_PLAY_PCM:
        voice_pcm(s, v, c->p, (uint32_t)c->x, c->a, (int8_t)c->b);
        break;
    case OP_PLAY_TONE:
        voice_tone(s, v, c->a, (uint32_t)c->x, (uint8_t)(c->y & 0xFF), (int8_t)((c->y >> 8) & 0xFF), c->b, c->p);
        break;
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
    case OP_SET_ADSR:
        voice_adsr(s, v, (uint32_t)c->x & 0xFFFF, (uint32_t)c->x >> 16, c->a, (uint32_t)c->y);
        break;
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
    case OP_MUSIC_PLAY:
        music_start(s, c->p, c->a);
        break;
    case OP_MUSIC_STOP:
        music_silence(s);
        s->song = NULL;
        break;
    case OP_MUSIC_VOL:
        s->music_vol = c->a;
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
        if (s->song)
            music_advance(s, n);

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

    uint32_t sounding = 0;
    for (int i = 0; i < SND_MAX_VOICES; i++)
        if (s->voices[i].kind != SND_OFF)
            sounding |= 1u << i;
    uint32_t music_on = 0;
    if (s->song)
        for (int i = 0; i < s->song->tracks && !music_on; i++)
            music_on = !s->track[i].ended;
    __atomic_store_n(&s->sounding, sounding, __ATOMIC_RELEASE);
    __atomic_store_n(&s->music_on, music_on, __ATOMIC_RELEASE);
    __atomic_store_n(&s->done, head, __ATOMIC_RELEASE);
}

bool snd_voice_active(const snd_t *s, int voice)
{
    return voice >= 0 && voice < SND_MAX_VOICES && s->voices[voice].kind != SND_OFF;
}
