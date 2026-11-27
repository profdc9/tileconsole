/* SPDX-License-Identifier: MIT */
#include "tl_sound.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "snd.h"
#include "tilefs.h"

#define TL_VOICES SND_MAX_VOICES
#define TL_SAMPLES 32       /* samples, numbered from 1 */
#define WAVE_LEN 32         /* a wavetable's entries (tilesnd's SND_WAVE) */
#define WAIT_MS 500         /* the longest the game waits for the audio thread (room in the queue, or a stop to be applied) */
#define LENGTH_MAX (1u << 30)

/* What the game thread knows of a voice: what it was last given, and what tilesnd has to be told again with every note (volume, pan, duty and the
 * wavetable are arguments of a note there, settings of the voice here). */
typedef struct {
    int kind;               /* 0: nothing since the voice was last stopped; a snd_kind_t (SND_PCM for a sample) */
    int sample;             /* SND_PCM: the sample's number (from 1) */
    uint8_t vol, duty;
    int8_t pan;
    float pitch;            /* a tone's note (a MIDI number, 60 is middle C), or a sample's speed (1 is as recorded) */
    int sliding;            /* pitch goes from slide_from to slide_to over slide_ms from slide_t0 */
    float slide_from, slide_to;
    uint32_t slide_t0, slide_ms;
    int timed;              /* the note is let go (key off) at off_at */
    uint32_t off_at;
    uint32_t mark;          /* the queue's mark after the voice's last note: until the audio thread reaches it, the note counts as playing */
    int8_t wave[WAVE_LEN];  /* SND_WAVE's table: tilesnd reads it while the voice plays, so it stays here */
} voice_t;

/* A sample: tilesnd keeps a pointer to the snd_sample_t while a voice plays it, so it stays here too. */
typedef struct {
    snd_sample_t s;
    void *data;             /* NULL: not made */
} sample_t;

static struct {
    voice_t v[TL_VOICES];
    sample_t samples[TL_SAMPLES];
    uint32_t last_tick;
    snd_instrument_t inst[SND_MAX_INSTRUMENTS];   /* for music: copied into each song (with its sample's address) by music() */
    int inst_sample[SND_MAX_INSTRUMENTS];         /* a "sample" instrument's sample, from 1 */
    snd_song_t *song;                             /* the music music() last started (NULL: none); the audio thread reads it while it plays */
    uint32_t song_samples;                        /* the samples its instruments play, bit n for sample n + 1 */
    uint32_t music_mark;                          /* the queue's mark after it was started */
    int music_on;                                 /* started, and not stopped since (the audio thread says when it ends by itself) */
} S;

static tl_env_t *E;   /* the environment the functions belong to, kept by tl_sound_open (there is one at a time) */

/* ---- the engine and the audio thread ---- */

/* Waits (a little: the audio thread empties the queue every few ms) for room for n commands; 0 if it did not come. */
static int room(tl_env_t *e, int n)
{
    snd_t *s = e->hooks.snd;
    if (snd_room(s) >= n)
        return 1;
    const uint32_t t0 = e->hooks.millis();
    while (snd_room(s) < n) {
        if (e->hooks.poll)
            e->hooks.poll();
        if (e->hooks.millis() - t0 > WAIT_MS)
            return 0;
    }
    return 1;
}

/* Waits until the audio thread has applied everything queued so far; 1 if it has (nothing reads what a stopped voice played any more). */
static int settle(tl_env_t *e)
{
    snd_t *s = e->hooks.snd;
    if (!s)
        return 1;
    const uint32_t mark = snd_mark(s), t0 = e->hooks.millis();
    while (!snd_reached(s, mark)) {
        if (e->hooks.poll)
            e->hooks.poll();
        if (e->hooks.millis() - t0 > WAIT_MS)
            return 0;
    }
    return 1;
}

static void *mem(tl_env_t *e, size_t n)
{
    return e->hooks.alloc ? e->hooks.alloc(n, 0) : malloc(n);
}

static void unmem(tl_env_t *e, void *p)
{
    if (!p)
        return;
    if (e->hooks.release)
        e->hooks.release(p);
    else
        free(p);
}

/* A note (a MIDI number, fractions allowed) as tilesnd's Hz * 256, kept between 1/256 Hz and the output rate (the noises are clocked at most once a sample). */
static uint32_t note_hz(const snd_t *s, float note)
{
    const float q = 440.0f * 256.0f * exp2f((note - 69.0f) / 12.0f), top = (float)s->rate * 256.0f;
    return q >= top ? (uint32_t)top : q < 1.0f ? 1u : (uint32_t)(q + 0.5f);
}

/* A sample's speed (1 is as recorded) as tilesnd's 16.16 pitch. */
static uint32_t speed_q16(float speed)
{
    const float q = speed * 65536.0f;
    return q >= 64.0f * 65536.0f ? 64u * 65536u : q < 1.0f ? 1u : (uint32_t)(q + 0.5f);
}

/* Tells the engine the voice's pitch (pitch, slide). */
static void send_pitch(tl_env_t *e, int v)
{
    snd_t *s = e->hooks.snd;
    const voice_t *vo = &S.v[v];
    if (!s || !vo->kind || !room(e, 1))
        return;
    if (vo->kind == SND_PCM)
        snd_set_pitch(s, v, speed_q16(vo->pitch));
    else
        snd_set_freq(s, v, note_hz(s, vo->pitch));
}

static void forget_motion(voice_t *vo)
{
    vo->sliding = 0;
    vo->timed = 0;
}

static void set_wave(int8_t *w, const char *name)
{
    for (int i = 0; i < WAVE_LEN; i++) {
        float x;
        if (!strcmp(name, "saw"))
            x = -1.0f + 2.0f * (float)i / WAVE_LEN;
        else if (!strcmp(name, "square"))
            x = i < WAVE_LEN / 2 ? 1.0f : -1.0f;
        else if (!strcmp(name, "triangle"))
            x = i < WAVE_LEN / 2 ? -1.0f + 4.0f * (float)i / WAVE_LEN : 3.0f - 4.0f * (float)i / WAVE_LEN;
        else
            x = sinf(6.2831853f * (float)i / WAVE_LEN);
        w[i] = (int8_t)lrintf(x * 127.0f);
    }
}

static void voice_defaults(voice_t *vo)
{
    memset(vo, 0, sizeof *vo);
    vo->vol = 255;
    vo->duty = 128;
    vo->pitch = 60.0f;
    set_wave(vo->wave, "sine");
}

static void instrument_defaults(void)
{
    for (int i = 0; i < SND_MAX_INSTRUMENTS; i++) {
        snd_instrument_default(&S.inst[i]);
        set_wave(S.inst[i].wave, "sine");
        S.inst_sample[i] = 0;
    }
}

/* Gives the song's memory back (after `free_ok`: the audio thread has let go of it; otherwise it is lost rather than freed while read). */
static void drop_song(tl_env_t *e, int free_ok)
{
    if (free_ok)
        unmem(e, S.song);
    S.song = NULL;
    S.song_samples = 0;
    S.music_on = 0;
}

/* Stops what plays sample n (from 0), the music too if it plays it, and waits for the audio thread to let go of it; 1 if it has. */
static int let_go(tl_env_t *e, int n)
{
    snd_t *s = e->hooks.snd;
    const int in_song = S.song && ((S.song_samples >> n) & 1);
    int queued = 1;   /* (a stop that could not be queued lets go of nothing: the audio thread still plays it) */
    if (in_song && s)
        queued = room(e, 1) && snd_music_stop(s);
    for (int v = 0; v < TL_VOICES; v++) {
        voice_t *vo = &S.v[v];
        if (vo->kind == SND_PCM && vo->sample == n + 1) {
            if (s && !(room(e, 1) && snd_stop(s, v)))
                queued = 0;
            vo->kind = 0;
            vo->sample = 0;
            forget_motion(vo);
        }
    }
    const int settled = queued && settle(e);
    if (in_song)
        drop_song(e, settled);
    return settled;
}

/* Gives sample n's memory back (after `free_ok`: the audio thread has let go of it; otherwise it is kept, lost, rather than freed while read). */
static void drop_sample(tl_env_t *e, int n, int free_ok)
{
    sample_t *sm = &S.samples[n];
    if (free_ok)
        unmem(e, sm->data);
    memset(sm, 0, sizeof *sm);
}

void tl_sound_init(tl_env_t *e)
{
    (void)e;
    memset(&S, 0, sizeof S);
    for (int v = 0; v < TL_VOICES; v++)
        voice_defaults(&S.v[v]);
    instrument_defaults();
}

void tl_sound_silence(tl_env_t *e)
{
    snd_t *s = e->hooks.snd;
    if (s && room(e, 2)) {
        snd_music_stop(s);
        snd_stop_all(s);
    }
    S.music_on = 0;
    for (int v = 0; v < TL_VOICES; v++) {
        S.v[v].kind = 0;
        S.v[v].sample = 0;
        forget_motion(&S.v[v]);
    }
}

void tl_sound_reset(tl_env_t *e)
{
    snd_t *s = e->hooks.snd;
    const int queued = !s || room(e, 4 + TL_VOICES);
    if (s && queued) {
        snd_music_stop(s);
        snd_music_volume(s, 255);
        snd_stop_all(s);
        snd_set_master(s, 255);
        for (int v = 0; v < TL_VOICES; v++)
            snd_set_adsr(s, v, 0, 0, 255, 5);   /* (tilesnd's own default) */
    }
    const int settled = queued && settle(e);
    if (queued)   /* (otherwise the song still plays: it stays this module's, and the next music() stops it and gives it back) */
        drop_song(e, settled);
    for (int n = 0; n < TL_SAMPLES; n++)
        drop_sample(e, n, settled);
    for (int v = 0; v < TL_VOICES; v++)
        voice_defaults(&S.v[v]);
    instrument_defaults();
}

void tl_sound_tick(tl_env_t *e)
{
    snd_t *s = e->hooks.snd;
    if (!s)
        return;
    const uint32_t now = e->hooks.millis();
    if (now == S.last_tick)
        return;
    S.last_tick = now;
    for (int v = 0; v < TL_VOICES; v++) {
        voice_t *vo = &S.v[v];
        if (vo->timed && (int32_t)(now - vo->off_at) >= 0) {
            vo->timed = 0;
            if (room(e, 1))
                snd_key_off(s, v);
        }
        if (vo->sliding) {
            const uint32_t gone = now - vo->slide_t0;
            if (gone >= vo->slide_ms) {
                vo->sliding = 0;
                vo->pitch = vo->slide_to;
            } else {
                vo->pitch = vo->slide_from + (vo->slide_to - vo->slide_from) * ((float)gone / (float)vo->slide_ms);
            }
            send_pitch(e, v);
        }
    }
}

/* ---- arguments ---- */

static int whole_arg(lua_State *L, int i, const char *who, const char *what)
{
    int isnum;
    const lua_Integer n = lua_tointegerx(L, i, &isnum);
    if (!isnum)
        luaL_error(L, "%s: %s is a whole number", who, what);
    return (int)n;
}

/* A voice, 1 to 16: its index from 0. */
static int voice_arg(lua_State *L, int i, const char *who)
{
    const int v = whole_arg(L, i, who, "the voice");
    if (v < 1 || v > TL_VOICES)
        luaL_error(L, "%s: the voice is 1 to %d", who, TL_VOICES);
    return v - 1;
}

/* A sample's number, 1 to TL_SAMPLES: its index from 0. */
static int sample_arg(lua_State *L, int i, const char *who)
{
    const int n = whole_arg(L, i, who, "the sample");
    if (n < 1 || n > TL_SAMPLES)
        luaL_error(L, "%s: the sample is 1 to %d", who, TL_SAMPLES);
    return n - 1;
}

/* A number kept between lo and hi (a level, a pan, a time): what is beyond them is the nearest end. */
static float clamped(lua_State *L, int i, float lo, float hi)
{
    const float x = (float)luaL_checknumber(L, i);
    return !(x >= lo) ? lo : x > hi ? hi : x;   /* (NaN: lo) */
}

static int level_arg(lua_State *L, int i) { return (int)lrintf(clamped(L, i, 0.0f, 255.0f)); }

/* A length in ms, nil or 0 for none. */
static uint32_t ms_opt(lua_State *L, int i)
{
    return lua_isnoneornil(L, i) ? 0 : (uint32_t)lrintf(clamped(L, i, 0.0f, (float)LENGTH_MAX));
}

/* A note's name: a letter A to G, then # (sharp) or b (flat), any number of them, then the octave (4 if none; C4 is middle C, 60). 1 if it is one. */
static int parse_note(const char *t, float *out)
{
    static const int semis[7] = {9, 11, 0, 2, 4, 5, 7};   /* A B C D E F G */
    const int c = *t >= 'a' ? *t - 'a' : *t - 'A';
    if (c < 0 || c > 6)
        return 0;
    int n = semis[c];
    t++;
    for (; *t == '#' || *t == 'b'; t++)
        n += *t == '#' ? 1 : -1;
    int octave = 4;
    if (*t) {
        const int neg = *t == '-';
        t += neg;
        if (*t < '0' || *t > '9')
            return 0;
        octave = 0;
        while (*t >= '0' && *t <= '9' && octave < 100)
            octave = octave * 10 + (*t++ - '0');
        if (*t)
            return 0;
        octave = neg ? -octave : octave;
    }
    *out = (float)((octave + 1) * 12 + n);
    return 1;
}

/* A note: a number (MIDI: 60 is middle C, 69 is the A of 440 Hz; fractions are between) or a name ("C4", "F#3", "Bb5"). */
static float note_arg(lua_State *L, int i, const char *who)
{
    float n;
    if (lua_type(L, i) == LUA_TNUMBER) {
        n = (float)lua_tonumber(L, i);
        if (isfinite(n))
            return n;
    } else if (lua_type(L, i) == LUA_TSTRING && parse_note(lua_tostring(L, i), &n)) {
        return n;
    }
    luaL_error(L, "%s: a note is a number (60 is middle C) or a name like \"C4\", \"F#3\", \"Bb5\"", who);
    return 0;
}

/* A tone's kind (or, for an instrument, "sample" too: SND_PCM). */
static int kind_arg(lua_State *L, int i, int instrument)
{
    static const struct { const char *name; int kind; } kinds[] = {
        {"pulse", SND_PULSE}, {"triangle", SND_TRIANGLE}, {"noise", SND_NOISE}, {"buzz", SND_NOISE_SHORT}, {"wave", SND_WAVE}, {"sample", SND_PCM},
    };
    const char *name = lua_type(L, i) == LUA_TSTRING ? lua_tostring(L, i) : NULL;
    for (size_t k = 0; name && k < sizeof kinds / sizeof *kinds - !instrument; k++)
        if (!strcmp(name, kinds[k].name))
            return kinds[k].kind;
    if (instrument)
        luaL_error(L, "instrument: the kind is \"pulse\", \"triangle\", \"noise\", \"buzz\", \"wave\" or \"sample\"");
    luaL_error(L, "tone: the kind is \"pulse\", \"triangle\", \"noise\", \"buzz\" or \"wave\"");
    return 0;
}

static void start_timer(tl_env_t *e, voice_t *vo, uint32_t ms)
{
    vo->timed = ms > 0;
    vo->off_at = e->hooks.millis() + ms;
}

/* ---- notes ---- */

/* tone(voice, kind, note [, ms]): a tone on the voice, with its envelope, volume, pan and duty; it holds until release(voice), or is let go after ms. */
static int l_tone(lua_State *L)
{
    tl_env_t *e = E;
    const int v = voice_arg(L, 1, "tone");
    const int kind = kind_arg(L, 2, 0);
    const float note = note_arg(L, 3, "tone");
    const uint32_t ms = ms_opt(L, 4);
    voice_t *vo = &S.v[v];
    vo->kind = kind;
    vo->sample = 0;
    vo->pitch = note;
    vo->sliding = 0;
    start_timer(e, vo, ms);
    snd_t *s = e->hooks.snd;
    if (s && room(e, 1)) {
        snd_play_tone(s, v, (snd_kind_t)kind, note_hz(s, note), vo->vol, vo->pan, vo->duty, vo->wave);
        vo->mark = snd_mark(s);
    }
    return 0;
}

/* play(voice, sample [, speed [, ms]]): a sample on the voice; speed 1 is as recorded, 2 an octave up. It ends by itself, or loops until release. */
static int l_play(lua_State *L)
{
    tl_env_t *e = E;
    const int v = voice_arg(L, 1, "play");
    const int n = sample_arg(L, 2, "play");
    if (!S.samples[n].data)
        return luaL_error(L, "play: there is no sample %d: sample() or loadsample() makes one", n + 1);
    const float speed = lua_isnoneornil(L, 3) ? 1.0f : (float)luaL_checknumber(L, 3);
    if (!(speed > 0.0f))
        return luaL_error(L, "play: the speed is more than 0 (1 is as recorded)");
    const uint32_t ms = ms_opt(L, 4);
    voice_t *vo = &S.v[v];
    vo->kind = SND_PCM;
    vo->sample = n + 1;
    vo->pitch = speed;
    vo->sliding = 0;
    start_timer(e, vo, ms);
    snd_t *s = e->hooks.snd;
    if (s && room(e, 1)) {
        snd_play_pcm(s, v, &S.samples[n].s, speed_q16(speed), vo->vol, vo->pan);
        vo->mark = snd_mark(s);
    }
    return 0;
}

/* release(voice): let go of the note: it fades out over the envelope's release. */
static int l_release(lua_State *L)
{
    tl_env_t *e = E;
    const int v = voice_arg(L, 1, "release");
    S.v[v].timed = 0;
    snd_t *s = e->hooks.snd;
    if (s && room(e, 1))
        snd_key_off(s, v);
    return 0;
}

/* silence([voice]): stop the voice at once, or every voice. */
static int l_silence(lua_State *L)
{
    tl_env_t *e = E;
    if (lua_isnoneornil(L, 1)) {
        tl_sound_silence(e);
        return 0;
    }
    const int v = voice_arg(L, 1, "silence");
    voice_t *vo = &S.v[v];
    vo->kind = 0;
    vo->sample = 0;
    forget_motion(vo);
    snd_t *s = e->hooks.snd;
    if (s && room(e, 1))
        snd_stop(s, v);
    return 0;
}

/* playing(voice): true while the voice makes sound (a note that has not ended, or one still on its way to the audio thread). */
static int l_playing(lua_State *L)
{
    tl_env_t *e = E;
    const int v = voice_arg(L, 1, "playing");
    const snd_t *s = e->hooks.snd;
    const voice_t *vo = &S.v[v];
    lua_pushboolean(L, s && vo->kind && (((snd_sounding(s) >> v) & 1) || !snd_reached(s, vo->mark)));
    return 1;
}

/* ---- the voice's settings ---- */

/* envelope(voice [, attack, decay, sustain, release]): times in ms, sustain a level 0 to 255; with sustain 0 a note ends by itself once it has decayed.
 * envelope(voice) is the default again: at once to full, held, 5 ms to fade. */
static int l_envelope(lua_State *L)
{
    tl_env_t *e = E;
    const int v = voice_arg(L, 1, "envelope");
    const uint16_t a = lua_isnoneornil(L, 2) ? 0 : (uint16_t)lrintf(clamped(L, 2, 0.0f, 65535.0f));
    const uint16_t d = lua_isnoneornil(L, 3) ? 0 : (uint16_t)lrintf(clamped(L, 3, 0.0f, 65535.0f));
    const uint8_t sus = lua_isnoneornil(L, 4) ? 255 : (uint8_t)level_arg(L, 4);
    const uint16_t r = lua_isnoneornil(L, 5) ? 5 : (uint16_t)lrintf(clamped(L, 5, 0.0f, 65535.0f));
    snd_t *s = e->hooks.snd;
    if (s && room(e, 1))
        snd_set_adsr(s, v, a, d, sus, r);
    return 0;
}

/* volume(voice, level), pan(voice, -128 to 127), duty(voice, level): for the note playing and the ones after it. */
static int l_volume(lua_State *L)
{
    tl_env_t *e = E;
    const int v = voice_arg(L, 1, "volume");
    S.v[v].vol = (uint8_t)level_arg(L, 2);
    snd_t *s = e->hooks.snd;
    if (s && room(e, 1))
        snd_set_volume(s, v, S.v[v].vol);
    return 0;
}

static int l_pan(lua_State *L)
{
    tl_env_t *e = E;
    const int v = voice_arg(L, 1, "pan");
    S.v[v].pan = (int8_t)lrintf(clamped(L, 2, -128.0f, 127.0f));
    snd_t *s = e->hooks.snd;
    if (s && room(e, 1))
        snd_set_pan(s, v, S.v[v].pan);
    return 0;
}

static int l_duty(lua_State *L)
{
    tl_env_t *e = E;
    const int v = voice_arg(L, 1, "duty");
    S.v[v].duty = (uint8_t)level_arg(L, 2);
    snd_t *s = e->hooks.snd;
    if (s && room(e, 1))
        snd_set_duty(s, v, S.v[v].duty);
    return 0;
}

/* The pitch argument of pitch and slide: a note for a tone, a speed for a sample. */
static float pitch_arg(lua_State *L, int i, const voice_t *vo, const char *who)
{
    if (vo->kind != SND_PCM)
        return note_arg(L, i, who);
    const float speed = (float)luaL_checknumber(L, i);
    if (!(speed > 0.0f))
        luaL_error(L, "%s: a sample's speed is more than 0 (1 is as recorded)", who);
    return speed;
}

/* pitch(voice, note): a tone's note now (a sample's speed). */
static int l_pitch(lua_State *L)
{
    tl_env_t *e = E;
    const int v = voice_arg(L, 1, "pitch");
    voice_t *vo = &S.v[v];
    vo->pitch = pitch_arg(L, 2, vo, "pitch");
    vo->sliding = 0;
    send_pitch(e, v);
    return 0;
}

/* slide(voice, note, ms): from the pitch it has to the note, evenly in semitones, over ms (a sample: to the speed). */
static int l_slide(lua_State *L)
{
    tl_env_t *e = E;
    const int v = voice_arg(L, 1, "slide");
    voice_t *vo = &S.v[v];
    const float to = pitch_arg(L, 2, vo, "slide");
    const uint32_t ms = ms_opt(L, 3);
    if (ms == 0) {
        vo->pitch = to;
        vo->sliding = 0;
        send_pitch(e, v);
        return 0;
    }
    vo->slide_from = vo->pitch;
    vo->slide_to = to;
    vo->slide_t0 = e->hooks.millis();
    vo->slide_ms = ms;
    vo->sliding = 1;
    return 0;
}

/* wave(voice, shape): the voice's table for "wave" tones: "sine" (the default), "triangle", "saw", "square", a table of 32 numbers from -1 to 1, or a
 * string of 32 bytes (signed: 0 to 127, then 128 to 255 for -128 to -1). A note playing changes at once. */
/* A wavetable from argument i (a shape's name, a table of 32 numbers from -1 to 1, a string of 32 bytes) into w; 0 if it is none of them. */
static int wave_from(lua_State *L, int i, int8_t *w, const char *who)
{
    size_t len = 0;
    if (lua_type(L, i) == LUA_TSTRING) {
        const char *t = lua_tolstring(L, i, &len);
        if (len == WAVE_LEN) {
            for (int k = 0; k < WAVE_LEN; k++)
                w[k] = (int8_t)(uint8_t)t[k];
            return 1;
        }
        if (!strcmp(t, "sine") || !strcmp(t, "triangle") || !strcmp(t, "saw") || !strcmp(t, "square")) {
            set_wave(w, t);
            return 1;
        }
    } else if (lua_type(L, i) == LUA_TTABLE && lua_rawlen(L, i) == WAVE_LEN) {
        int8_t tmp[WAVE_LEN];
        for (int k = 0; k < WAVE_LEN; k++) {
            lua_rawgeti(L, i, k + 1);
            int isnum;
            float x = (float)lua_tonumberx(L, -1, &isnum);
            lua_pop(L, 1);
            if (!isnum)
                return luaL_error(L, "%s: entry %d of the wave's table is not a number", who, k + 1);
            x = x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x;
            tmp[k] = (int8_t)lrintf(x * 127.0f);
        }
        memcpy(w, tmp, sizeof tmp);
        return 1;
    }
    return 0;
}

static int l_wave(lua_State *L)
{
    const int v = voice_arg(L, 1, "wave");
    if (!wave_from(L, 2, S.v[v].wave, "wave"))
        return luaL_error(L, "wave(voice, shape): \"sine\", \"triangle\", \"saw\", \"square\", a table of %d numbers (-1 to 1) or a string of %d bytes",
                          WAVE_LEN, WAVE_LEN);
    return 0;
}

/* mastervolume(level): all of the sound, 0 to 255. */
static int l_mastervolume(lua_State *L)
{
    tl_env_t *e = E;
    const int level = level_arg(L, 1);
    snd_t *s = e->hooks.snd;
    if (s && room(e, 1))
        snd_set_master(s, (uint8_t)level);
    return 0;
}

/* note(name): the number of a note's name ("C4" is 60); hz(frequency): the note (with a fraction) of a frequency (440 is 69). */
static int l_note(lua_State *L)
{
    lua_pushnumber(L, note_arg(L, 1, "note"));
    return 1;
}

static int l_hz(lua_State *L)
{
    const float f = (float)luaL_checknumber(L, 1);
    if (!(f > 0.0f) || !isfinite(f))
        return luaL_error(L, "hz(frequency): the frequency is more than 0");
    lua_pushnumber(L, 69.0f + 12.0f * log2f(f / 440.0f));
    return 1;
}

/* ---- samples ---- */

/* The loop of a sample of `length` samples from the arguments i and i + 1 (the first and last sample that repeat, from 1, as string.sub counts; the last is
 * the end if not given), or none. 0 if they are wrong (nothing is raised: the caller may have memory to give back first). */
static int loop_get(lua_State *L, int i, uint32_t length, snd_sample_t *s)
{
    s->loop_start = s->loop_end = 0;
    if (lua_isnoneornil(L, i))
        return 1;
    int ok1, ok2 = 1;
    const lua_Integer from = lua_tointegerx(L, i, &ok1), to = lua_isnoneornil(L, i + 1) ? (lua_Integer)length : lua_tointegerx(L, i + 1, &ok2);
    if (!ok1 || !ok2 || from < 1 || to < from || (lua_Unsigned)to > length)
        return 0;
    s->loop_start = (uint32_t)from - 1;
    s->loop_end = (uint32_t)to;
    return 1;
}

static int bad_loop(lua_State *L, const char *who, uint32_t length)
{
    return luaL_error(L, "%s: the loop is two whole numbers, the first and last sample that repeat (1 to %d)", who, (int)length);
}

/* Puts a new sample's memory in as sample n, after the old one is gone. */
static void install(tl_env_t *e, int n, void *data, uint32_t length, uint32_t rate, int bits, const snd_sample_t *loop)
{
    if (S.samples[n].data)
        drop_sample(e, n, let_go(e, n));
    sample_t *sm = &S.samples[n];
    sm->data = data;
    sm->s.data = data;
    sm->s.length = length;
    sm->s.rate = rate;
    sm->s.bits = (uint8_t)bits;
    sm->s.loop_start = loop->loop_start;
    sm->s.loop_end = loop->loop_end;
}

/* sample(n, data, rate [, first, last]): makes sample n from a string (one byte a sample, signed: 0 to 127, then 128 to 255 for -128 to -1) or a table
 * of numbers from -1 to 1, recorded at `rate` samples a second. first and last make it loop: those samples repeat until release(voice). Returns its length. */
static int l_sample(lua_State *L)
{
    tl_env_t *e = E;
    const int n = sample_arg(L, 1, "sample");
    const int type = lua_type(L, 2);
    if (type != LUA_TSTRING && type != LUA_TTABLE)
        return luaL_error(L, "sample(n, data, rate): the data is a string of bytes or a table of numbers from -1 to 1");
    const lua_Integer rate = luaL_checkinteger(L, 3);
    if (rate < 100 || rate > 200000)
        return luaL_error(L, "sample: the rate is 100 to 200000 samples a second");
    const size_t length = (size_t)lua_rawlen(L, 2);
    if (length == 0 || length > LENGTH_MAX)
        return luaL_error(L, "sample: the data is empty");
    snd_sample_t loop;
    if (!loop_get(L, 4, (uint32_t)length, &loop))
        return bad_loop(L, "sample", (uint32_t)length);
    void *data;
    if (type == LUA_TSTRING) {
        data = mem(e, length);
        if (!data)
            return luaL_error(L, "sample: not enough memory for %d samples", (int)length);
        memcpy(data, lua_tostring(L, 2), length);
    } else {
        int16_t *d = mem(e, length * 2);
        if (!d)
            return luaL_error(L, "sample: not enough memory for %d samples", (int)length);
        for (size_t i = 0; i < length; i++) {
            lua_rawgeti(L, 2, (lua_Integer)i + 1);
            int isnum;
            float x = (float)lua_tonumberx(L, -1, &isnum);
            lua_pop(L, 1);
            if (!isnum) {
                unmem(e, d);
                return luaL_error(L, "sample: entry %d of the table is not a number", (int)i + 1);
            }
            x = x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x;
            d[i] = (int16_t)lrintf(x * 32767.0f);
        }
        data = d;
    }
    install(e, n, data, (uint32_t)length, (uint32_t)rate, type == LUA_TSTRING ? 8 : 16, &loop);
    lua_pushinteger(L, (lua_Integer)length);
    return 1;
}

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

/* Reads n bytes (fewer only at the end of the file); the count, or a negative error. */
static int read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        const int r = fs_read(fd, (char *)buf + got, n - got);
        if (r < 0)
            return r;
        if (r == 0)
            break;
        got += (size_t)r;
    }
    return (int)got;
}

typedef struct {
    void *data;
    uint32_t length, rate;
    int bits;
} wav_t;

/* A WAV file's sound (8- or 16-bit PCM, mono or stereo, which is made mono) into new memory; NULL, or what is wrong with it. */
static const char *read_wav(tl_env_t *e, int fd, wav_t *w)
{
    uint8_t h[40];
    if (read_full(fd, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        return "not a WAV file";
    int have_fmt = 0;
    uint32_t channels = 0, bits = 0, rate = 0, format = 0, size = 0;
    for (;;) {
        if (read_full(fd, h, 8) != 8)
            return have_fmt ? "no sound in it (no data chunk)" : "not a WAV file (no fmt chunk)";
        size = le32(h + 4);
        if (!memcmp(h, "data", 4))
            break;
        uint32_t skip = size + (size & 1);
        if (!memcmp(h, "fmt ", 4)) {
            const uint32_t take = size < sizeof h ? size : (uint32_t)sizeof h;
            if (size < 16 || read_full(fd, h, take) != (int)take)
                return "a fmt chunk that is too short";
            format = le16(h);
            channels = le16(h + 2);
            rate = le32(h + 4);
            bits = le16(h + 14);
            if (format == 0xFFFE && take >= 26)   /* WAVE_FORMAT_EXTENSIBLE: the real format is the subformat's first two bytes */
                format = le16(h + 24);
            have_fmt = 1;
            skip -= take;
        }
        if (skip && fs_seek(fd, skip, FS_SEEK_CUR) < 0)
            return "a broken chunk";
    }
    if (!have_fmt)
        return "not a WAV file (no fmt chunk)";
    if (format != 1 || (bits != 8 && bits != 16))
        return "not 8- or 16-bit PCM (only those can be loaded)";
    if (channels < 1 || channels > 2)
        return "not mono or stereo";
    if (rate < 100 || rate > 200000)
        return "a sample rate that is not 100 to 200000";
    const uint32_t frame = channels * bits / 8;
    uint32_t length = size / frame;
    if (length == 0)
        return "no sound in it (an empty data chunk)";
    if (length > LENGTH_MAX)
        length = LENGTH_MAX;
    uint8_t *out = mem(e, (size_t)length * (bits / 8));
    if (!out)
        return "not enough memory for it";
    uint8_t buf[512];
    const uint32_t per = (uint32_t)sizeof buf / frame;
    uint32_t done = 0;
    while (done < length) {
        const uint32_t want = length - done < per ? length - done : per;
        const int r = read_full(fd, buf, (size_t)want * frame);
        if (r < 0) {
            unmem(e, out);
            return "a read error";
        }
        const uint32_t got = (uint32_t)r / frame;
        for (uint32_t i = 0; i < got; i++) {
            const uint8_t *p = buf + i * frame;
            if (bits == 8) {
                int x = (int)p[0] - 128;
                if (channels == 2)
                    x = (x + (int)p[1] - 128) >> 1;
                ((int8_t *)out)[done + i] = (int8_t)x;
            } else {
                int x = (int16_t)le16(p);
                if (channels == 2)
                    x = (x + (int16_t)le16(p + 2)) >> 1;
                ((int16_t *)out)[done + i] = (int16_t)x;
            }
        }
        done += got;
        if (got < want)   /* the file ends early: what there is */
            break;
    }
    if (done == 0) {
        unmem(e, out);
        return "no sound in it (the data is cut off)";
    }
    w->data = out;
    w->length = done;
    w->rate = rate;
    w->bits = (int)bits;
    return NULL;
}

/* loadsample(n, file [, first, last]): makes sample n from a WAV file (8- or 16-bit PCM; stereo is made mono). Returns its length and rate. */
static int l_loadsample(lua_State *L)
{
    tl_env_t *e = E;
    const int n = sample_arg(L, 1, "loadsample");
    const char *path = luaL_checkstring(L, 2);
    const int fd = fs_open(path, FS_RDONLY);
    if (fd < 0)
        return luaL_error(L, "loadsample: cannot open %s: %s", path, fs_strerror(fd));
    wav_t w;
    const char *why = read_wav(e, fd, &w);
    fs_close(fd);
    if (why)
        return luaL_error(L, "loadsample: %s: %s", path, why);
    snd_sample_t loop;
    if (!loop_get(L, 3, w.length, &loop)) {
        unmem(e, w.data);
        return bad_loop(L, "loadsample", w.length);
    }
    install(e, n, w.data, w.length, w.rate, w.bits, &loop);
    lua_pushinteger(L, (lua_Integer)w.length);
    lua_pushinteger(L, (lua_Integer)w.rate);
    return 2;
}

/* ---- music ---- */

/* instrument(n, kind [, settings]): instrument n (0 to 31) of the music: kind as tone's, or "sample"; settings is a table of any of duty, attack, decay,
 * sustain, release (as envelope's), volume (0 to 255), pan, wave (as wave's), sample (its number) and root (the note at which the sample plays as
 * recorded; C4, 60, if not given). It is used by the music started after it. */
static int l_instrument(lua_State *L)
{
    const int n = whole_arg(L, 1, "instrument", "the instrument");
    if (n < 0 || n >= SND_MAX_INSTRUMENTS)
        return luaL_error(L, "instrument: the instrument is 0 to %d", SND_MAX_INSTRUMENTS - 1);
    snd_instrument_t in;
    snd_instrument_default(&in);
    set_wave(in.wave, "sine");
    in.kind = (uint8_t)kind_arg(L, 2, 1);
    int sample = 0;
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        lua_pushnil(L);
        while (lua_next(L, 3)) {
            if (lua_type(L, -2) != LUA_TSTRING)
                return luaL_error(L, "instrument: the settings have names (instrument(1, \"pulse\", {duty = 64, decay = 200}))");
            const char *key = lua_tostring(L, -2);
            const int vi = lua_gettop(L);
            if (!strcmp(key, "duty"))
                in.duty = (uint8_t)level_arg(L, vi);
            else if (!strcmp(key, "attack"))
                in.attack_ms = (uint16_t)lrintf(clamped(L, vi, 0.0f, 65535.0f));
            else if (!strcmp(key, "decay"))
                in.decay_ms = (uint16_t)lrintf(clamped(L, vi, 0.0f, 65535.0f));
            else if (!strcmp(key, "sustain"))
                in.sustain = (uint8_t)level_arg(L, vi);
            else if (!strcmp(key, "release"))
                in.release_ms = (uint16_t)lrintf(clamped(L, vi, 0.0f, 65535.0f));
            else if (!strcmp(key, "volume"))
                in.vol = (uint8_t)level_arg(L, vi);
            else if (!strcmp(key, "pan"))
                in.pan = (int8_t)lrintf(clamped(L, vi, -128.0f, 127.0f));
            else if (!strcmp(key, "wave")) {
                if (!wave_from(L, vi, in.wave, "instrument"))
                    return luaL_error(L, "instrument: wave is \"sine\", \"triangle\", \"saw\", \"square\", a table of %d numbers or a string of %d bytes",
                                      WAVE_LEN, WAVE_LEN);
            } else if (!strcmp(key, "sample"))
                sample = sample_arg(L, vi, "instrument") + 1;
            else if (!strcmp(key, "root")) {
                const float r = note_arg(L, vi, "instrument");
                in.root = (int8_t)(r < 0.0f ? 0 : r > 127.0f ? 127 : (int)lrintf(r));
            } else
                return luaL_error(L, "instrument: there is no setting \"%s\" (duty, attack, decay, sustain, release, volume, pan, wave, sample, root)", key);
            lua_pop(L, 1);
        }
    }
    if (in.kind == SND_PCM && !sample)
        return luaL_error(L, "instrument: a \"sample\" instrument says which one: {sample = n}");
    S.inst[n] = in;
    S.inst_sample[n] = sample;
    return 0;
}

static void *song_alloc(void *ctx, size_t n) { return mem((tl_env_t *)ctx, n); }

/* The music stops, and its memory is given back once the audio thread has let go of it; 0 if the stop could not be queued (the music plays on). */
static int stop_music(tl_env_t *e)
{
    snd_t *s = e->hooks.snd;
    if (!S.song)
        return 1;
    if (s && !room(e, 1))
        return 0;
    if (s)
        snd_music_stop(s);
    drop_song(e, settle(e));
    return 1;
}

/* music([first voice,] track, ...): plays the tracks (MML, one string each) on voices of their own, the first on `first voice` and the next ones
 * after it (if not given, the last voices: four tracks are on 13 to 16), in place of any music playing. music() stops it. */
static int l_music(lua_State *L)
{
    tl_env_t *e = E;
    const int top = lua_gettop(L);
    if (top == 0) {
        if (!stop_music(e))
            return luaL_error(L, "music: the sound is not answering, and the music plays on");
        return 0;
    }
    int arg = 1, first = -1;
    if (lua_type(L, 1) == LUA_TNUMBER) {
        first = voice_arg(L, 1, "music");
        arg = 2;
    }
    const int n = top - arg + 1;
    if (n < 1 || n > TL_VOICES)
        return luaL_error(L, "music([first voice,] track, ...): 1 to %d tracks, each a string of MML", TL_VOICES);
    const char *tracks[TL_VOICES];
    for (int i = 0; i < n; i++)
        tracks[i] = luaL_checkstring(L, arg + i);
    if (first < 0)
        first = TL_VOICES - n;
    if (first + n > TL_VOICES)
        return luaL_error(L, "music: %d tracks from voice %d do not fit in the %d voices", n, first + 1, TL_VOICES);
    snd_instrument_t ins[SND_MAX_INSTRUMENTS];
    uint32_t uses = 0;
    for (int i = 0; i < SND_MAX_INSTRUMENTS; i++) {
        ins[i] = S.inst[i];
        const int k = S.inst_sample[i] - 1;
        if (ins[i].kind == SND_PCM && k >= 0 && S.samples[k].data) {   /* (a sample that is not made: the instrument's notes are silent) */
            ins[i].sample = &S.samples[k].s;
            uses |= 1u << k;
        }
    }
    char err[200];
    snd_song_t *song = snd_mml_compile(tracks, n, ins, song_alloc, e, err, sizeof err);
    if (!song)
        return luaL_error(L, "music: %s", err);
    snd_t *s = e->hooks.snd;
    if (s && !room(e, 1)) {   /* (the old song plays on, and is still read: it must not be given back) */
        unmem(e, song);
        return luaL_error(L, "music: the sound is not answering");
    }
    snd_song_t *old = S.song;
    if (s) {
        snd_music_play(s, song, first);   /* (the old song, if any, stops as this one starts) */
        S.music_mark = snd_mark(s);
    }
    if (old) {
        const int settled = settle(e);
        S.song = old;
        drop_song(e, settled);
    }
    S.song = song;
    S.song_samples = uses;
    S.music_on = s != NULL;
    return 0;
}

/* musicplaying(): true while the music plays (music that loops plays until it is stopped). */
static int l_musicplaying(lua_State *L)
{
    tl_env_t *e = E;
    const snd_t *s = e->hooks.snd;
    lua_pushboolean(L, s && S.song && S.music_on && (snd_music_playing(s) || !snd_reached(s, S.music_mark)));
    return 1;
}

/* musicvolume(level): the music's volume, 0 to 255, from its next notes on. */
static int l_musicvolume(lua_State *L)
{
    tl_env_t *e = E;
    const int level = level_arg(L, 1);
    snd_t *s = e->hooks.snd;
    if (s && room(e, 1))
        snd_music_volume(s, (uint8_t)level);
    return 0;
}

/* ---- registration ---- */

#define F(n) {#n, l_##n}
void tl_sound_open(lua_State *L)
{
    E = tl_env_current();
    static const luaL_Reg functions[] = {
        F(tone), F(play), F(release), F(silence), F(playing), F(envelope), F(volume), F(pan), F(duty), F(pitch), F(slide), F(wave),
        F(mastervolume), F(note), F(hz), F(sample), F(loadsample), F(instrument), F(music), F(musicplaying), F(musicvolume),
        {NULL, NULL},
    };
    for (const luaL_Reg *r = functions; r->func; r++)
        lua_register(L, r->name, r->func);
}
