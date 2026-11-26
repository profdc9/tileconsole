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

/* ---- music: a song is a list of events for each track (each plays on a voice of its own), and the instruments its notes use. The audio
 * thread plays it by itself (snd_music_play), so its timing does not depend on the game. snd_mml_compile makes one from MML text. ---- */

#define SND_TICKS_PER_QUARTER 96   /* the music's clock: a whole note is 384 ticks */
#define SND_MAX_INSTRUMENTS 32

/* How a track's notes sound. */
typedef struct {
    uint8_t kind;                          /* SND_PULSE ... SND_WAVE, or SND_PCM for a sample */
    uint8_t duty;                          /* SND_PULSE */
    uint8_t vol;                           /* 0..255: the instrument's own level, times the track's volume */
    int8_t pan;                            /* unless the track sets its own */
    uint16_t attack_ms, decay_ms, release_ms;
    uint8_t sustain;                       /* 0..255; 0: a note ends by itself once it has decayed */
    int8_t root;                           /* SND_PCM: the note at which the sample plays as it was recorded */
    const snd_sample_t *sample;            /* SND_PCM (NULL: the notes are silent) */
    int8_t wave[32];                       /* SND_WAVE */
} snd_instrument_t;

/* The instrument a song has before an @: a square wave (pulse, duty 128), full volume, held until the note's end, 5 ms to fade. */
void snd_instrument_default(snd_instrument_t *in);

enum { SND_EV_NOTE = 1, SND_EV_REST, SND_EV_INST, SND_EV_VOL, SND_EV_PAN, SND_EV_TEMPO, SND_EV_LOOP_START, SND_EV_LOOP_END };
#define SND_LOOP_DEPTH 8                   /* loops in loops, at most */
#define SND_EV_SLUR 1                      /* a note's flag: it follows the last one without a new attack (only the pitch changes) */

typedef struct {
    uint8_t op;                            /* SND_EV_... */
    uint8_t a;                             /* NOTE: the MIDI note; INST: the instrument; VOL: the volume 0..255; PAN: the pan (as an int8_t);
                                            * LOOP_START: how many times the events up to its LOOP_END are played (1..255) */
    uint8_t flags;
    uint8_t unused;
    uint32_t len;                          /* ticks from this event to the next one */
    uint32_t gate;                         /* NOTE: ticks until the note is let go (0: it is not; the next note slurs on); TEMPO: quarter notes a minute;
                                            * LOOP_END: how many events back the loop starts again (from the event after it, to the one after LOOP_START) */
} snd_event_t;

typedef struct {
    const snd_event_t *events;
    uint32_t count;
    int32_t loop;                          /* where the track starts again after its last event (MML's L), or -1: it ends */
} snd_track_t;

typedef struct snd_song {
    int tracks;
    snd_track_t track[SND_MAX_VOICES];
    snd_instrument_t instrument[SND_MAX_INSTRUMENTS];
} snd_song_t;

/*
 * MML (snd_mml.c): a song from text, one string a track. Commands are lower or upper case, except that l is the default length and L the
 * loop point; spaces, new lines and | (a bar line, for the eye) are ignored.
 *   c d e f g a b   a note, then + or # (sharp) or - (flat), then a length: 4 a quarter, 8 an eighth ... any n that divides 384 (3, 6, 12 ... for
 *                   triplets), or %n for n ticks (96 a quarter); then dots, each adding half of what was added before. No length: l's
 *   r               a rest, with a length the same way
 *   n60,8           a note by its MIDI number (60 is middle C), with a length after a comma or none
 *   o4 > <          the octave (0 to 9; o4 has middle C), one up, one down
 *   l8              the length of a note or rest that has none (l4 at first)
 *   ^8              adds to the length of the note or rest before it
 *   &               between two notes: the same pitch makes them one longer note; another pitch slurs to it (no new attack)
 *   t120            tempo, quarter notes a minute (20 to 600; 120 at first), for every track from that point on
 *   v15             volume, 0 to 15 (15 at first)
 *   q8              how much of its length a note sounds, in eighths (1 to 8; 8 at first)
 *   @1              the instrument, 0 to 31
 *   p-64            pan, -128 (left) to 127 (right); until then, the instrument's
 *   k-12            transpose, -48 to 48 semitones
 *   [ ... ]3        repeat 3 times (2 with no number), nested up to 8 deep; kept as a loop, so it takes no more memory for more passes
 *   L               where the track starts again when it reaches its end: the music loops for ever
 * Returns the song in one block from `alloc` (the caller frees it, once the audio thread has let go of it), or NULL with what is wrong, and
 * where, in `err`. `instruments` (SND_MAX_INSTRUMENTS of them, or NULL for the default ones) are copied into the song.
 */
snd_song_t *snd_mml_compile(const char *const *tracks, int ntracks, const snd_instrument_t *instruments, void *(*alloc)(void *ctx, size_t n),
                            void *ctx, char *err, size_t errlen);

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

#define SND_PAN_INSTRUMENT 0x7FFF          /* a track's pan before its first p: the instrument's */

typedef struct {
    uint32_t idx;                          /* the next event */
    uint32_t wait;                         /* ticks before it */
    uint32_t off;                          /* ticks before the note playing is let go (0: none to let go) */
    uint8_t inst, vol, ended, depth;       /* depth: loops it is inside */
    int16_t pan;
    uint8_t left[SND_LOOP_DEPTH];          /* the passes still to come of each loop it is inside */
} snd_track_state_t;

typedef struct snd {
    uint32_t rate;
    uint32_t master;                     /* 0..256 */
    snd_voice_t voices[SND_MAX_VOICES];
    snd_cmd_t queue[SND_QUEUE_SIZE];
    uint32_t q_head, q_tail;             /* head: consumer (audio thread), tail: producer (game thread) */
    uint32_t done;                       /* the head when the last snd_render ended (audio thread writes, game thread reads) */
    uint32_t sounding;                   /* the voices making sound when the last snd_render ended, bit n for voice n (likewise) */
    uint32_t music_on;                   /* a song is playing (some track has not ended) when the last snd_render ended (likewise) */
    const snd_song_t *song;              /* the music: NULL if none */
    uint8_t music_first, music_vol;      /* the voice of its first track; its volume, 0..255 */
    uint32_t music_spt, music_acc;       /* samples a tick, and those gone by toward the next tick, both 16.16 */
    snd_track_state_t track[SND_MAX_VOICES];
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

/* Music: the audio thread plays the song's tracks on the voices first_voice, first_voice + 1 ... (tracks that would be past the last voice
 * are left out), each from its start, and any song that was playing stops (its voices fall silent). The song must stay as it is, and
 * alive, until a later snd_music_play or snd_music_stop and its mark are reached. snd_music_volume scales every note from the next one
 * on. snd_music_playing: a song was playing when the last snd_render ended (one that loops plays for ever). */
bool snd_music_play(snd_t *s, const snd_song_t *song, int first_voice);
bool snd_music_stop(snd_t *s);
bool snd_music_volume(snd_t *s, uint8_t vol);
bool snd_music_playing(const snd_t *s);

/*
 * What the audio thread has done, for the game thread. snd_room is how many commands the queue has room for now. snd_mark is a mark after the
 * commands queued so far; snd_reached is true once the audio thread has applied all of them and finished the snd_render that did, so that
 * after snd_stop(v) and waiting for its mark, nothing reads what voice v played any more (a sample's memory can be given back). snd_sounding
 * is the voices that were making sound when the last snd_render ended, bit n for voice n: a note queued since is not in it yet.
 */
int snd_room(const snd_t *s);
uint32_t snd_mark(const snd_t *s);
bool snd_reached(const snd_t *s, uint32_t mark);
uint32_t snd_sounding(const snd_t *s);

/* ---- audio thread ---- */

/* Apply the queued commands and produce `frames` stereo frames: left, right, left, right, ... */
void snd_render(snd_t *s, int16_t *lr, int frames);

/* True if the voice is making sound (still playing, or fading out). Audio-thread state: for tests and diagnostics. */
bool snd_voice_active(const snd_t *s, int voice);

#endif
