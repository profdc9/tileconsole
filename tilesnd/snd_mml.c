/* SPDX-License-Identifier: MIT */
/*
 * MML text to a song for the audio thread (snd.h lists the commands). Two passes over each track: the first counts its events and finds
 * any mistake, the second writes them into the one block of memory the song is.
 *
 * A note is written out only when what follows shows it is finished: until then ^ and & can still make it longer, and & to another pitch
 * makes it a note that is not let go. Settings (v, @, p, t) are events of no length at the time the note before them ends.
 *
 * [ ]n is kept as a loop: a LOOP_START event with the count, the part inside once, and a LOOP_END that jumps back, so a repeat costs no more
 * memory however many times it goes round. That is right only when every pass comes out the same. The octave, the default length, q and the
 * transpose are worked out here, not by the player, so a part inside that changes them ([c >]3) may give different notes on its next pass:
 * then it is parsed again from where the first pass left off, and if the events or the state come out different, the repeat is written out
 * n times instead. A tie or a ^ that reaches across the brackets is written out too. Either way the music is the same.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "snd.h"

#define WHOLE (4 * SND_TICKS_PER_QUARTER)   /* 384 */
#define MAX_EVENTS 500000u                  /* in a song */
#define HASH_SEED 14695981039346656037ull   /* FNV-1a */
#define TICKS_MAX (1ull << 62)
#define MAX_DEPTH 8
#define MAX_LEN (1u << 30)                  /* the longest note or rest, in ticks */

typedef struct {
    const char *text;
    int track;                 /* from 1, for messages */
    snd_event_t *out;          /* NULL: only counting */
    uint32_t n;                /* events in this track so far */
    uint32_t peak;             /* the most there have been (a pass parsed only to be compared is taken back) */
    uint32_t *total;           /* in the song so far, for the limit */
    uint64_t hash;             /* of the events written since the part being compared began */
    int32_t loop;              /* L: the event after it, or -1 */
    uint64_t ticks, loop_ticks;
    int octave, transpose, q;
    uint32_t deflen;
    int pending;               /* a note (1) or a rest (2) not written yet */
    snd_event_t pend;
    int pend_q, pend_held;     /* the q it started with; held: & to another pitch follows, so it is not let go */
    int tie;                   /* & came after the pending note */
    char *err;
    size_t errlen;
    int failed;
} mml_t;

static void fail(mml_t *m, const char *at, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void fail(mml_t *m, const char *at, const char *fmt, ...)
{
    if (m->failed)
        return;
    m->failed = 1;
    char what[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    if (m->errlen)
        snprintf(m->err, m->errlen, "track %d, at %d: %s", m->track, (int)(at - m->text) + 1, what);
}

static uint64_t fold(uint64_t h, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        h ^= (v >> (8 * i)) & 0xFF;
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t sat_add(uint64_t a, uint64_t b) { return a + b > TICKS_MAX ? TICKS_MAX : a + b; }
static uint64_t sat_mul(uint64_t a, uint64_t b) { return b && a > TICKS_MAX / b ? TICKS_MAX : a * b; }

static void emit(mml_t *m, const char *at, snd_event_t ev)
{
    if (m->failed)
        return;
    if (*m->total >= MAX_EVENTS) {
        fail(m, at, "the music is too long (more than %u notes and changes)", MAX_EVENTS);
        return;
    }
    if (m->out)
        m->out[m->n] = ev;
    m->n++;
    if (m->n > m->peak)
        m->peak = m->n;
    (*m->total)++;
    m->ticks = sat_add(m->ticks, ev.len);
    m->hash = fold(fold(m->hash, (uint64_t)ev.op | (uint64_t)ev.a << 8 | (uint64_t)ev.flags << 16), (uint64_t)ev.len << 32 | ev.gate);
}

/* Where a track is, to come back to: what has been written, and the settings worked out here. */
typedef struct {
    uint32_t n, total;
    uint64_t ticks, hash;
    int octave, transpose, q;
    uint32_t deflen;
} snap_t;

static snap_t snap(const mml_t *m) { return (snap_t){m->n, *m->total, m->ticks, m->hash, m->octave, m->transpose, m->q, m->deflen}; }

static void rollback(mml_t *m, const snap_t *s)
{
    m->n = s->n;
    *m->total = s->total;
    m->ticks = s->ticks;
    m->hash = s->hash;
    m->octave = s->octave;
    m->transpose = s->transpose;
    m->q = s->q;
    m->deflen = s->deflen;
    m->pending = 0;
    m->tie = 0;
}

static int same_settings(const snap_t *a, const snap_t *b)
{
    return a->octave == b->octave && a->transpose == b->transpose && a->q == b->q && a->deflen == b->deflen;
}

/* Writes the note or rest that was waiting. */
static void flush(mml_t *m, const char *at)
{
    if (!m->pending)
        return;
    if (m->pending == 1) {
        const uint64_t gate = (uint64_t)m->pend.len * (uint64_t)m->pend_q / 8u;
        m->pend.gate = m->pend_held ? 0 : gate < 1 ? 1 : (uint32_t)gate;
    }
    m->pending = 0;
    emit(m, at, m->pend);
}

static void setting(mml_t *m, const char *at, int op, uint8_t a, uint32_t gate)
{
    flush(m, at);
    emit(m, at, (snd_event_t){.op = (uint8_t)op, .a = a, .gate = gate});
}

static void lengthen(mml_t *m, const char *at, uint32_t len)
{
    if ((uint64_t)m->pend.len + len > MAX_LEN)
        fail(m, at, "a note or rest longer than %u ticks", MAX_LEN);
    else
        m->pend.len += len;
}

static void note(mml_t *m, const char *at, int pitch, uint32_t len)
{
    if (pitch < 0 || pitch > 127) {
        fail(m, at, "the note is out of range (MIDI 0 to 127)");
        return;
    }
    int slur = 0;
    if (m->tie && m->pending == 1) {
        m->tie = 0;
        if (m->pend.a == pitch) {   /* the same pitch: one longer note */
            lengthen(m, at, len);
            return;
        }
        m->pend_held = 1;           /* another pitch: the first is not let go, the second does not attack */
        slur = 1;
    }
    m->tie = 0;
    flush(m, at);
    m->pending = 1;
    m->pend = (snd_event_t){.op = SND_EV_NOTE, .a = (uint8_t)pitch, .flags = slur ? SND_EV_SLUR : 0, .len = len};
    m->pend_q = m->q;
    m->pend_held = 0;
}

static void rest(mml_t *m, const char *at, uint32_t len)
{
    m->tie = 0;
    if (m->pending == 2) {   /* rests in a row: one rest */
        lengthen(m, at, len);
        return;
    }
    flush(m, at);
    m->pending = 2;
    m->pend = (snd_event_t){.op = SND_EV_REST, .len = len};
}

/* Digits as a number (with a sign first when `sign`); 0 if there are none. */
static int number(const char **p, int sign, long *out)
{
    int neg = 0;
    if (sign && (**p == '-' || **p == '+'))
        neg = *(*p)++ == '-';
    if (!isdigit((unsigned char)**p))
        return 0;
    long v = 0;
    while (isdigit((unsigned char)**p)) {
        if (v < 100000000)
            v = v * 10 + (*(*p)++ - '0');
        else
            (*p)++;
    }
    *out = neg ? -v : v;
    return 1;
}

/* A length: a division of a whole note (4 a quarter) or %ticks, then dots; `def` if there is neither (`need`: one must be there). 0 if wrong. */
static uint32_t length(mml_t *m, const char **p, uint32_t def, int need, const char *at)
{
    uint32_t len = def;
    long v;
    if (**p == '%') {
        (*p)++;
        if (!number(p, 0, &v) || v < 1 || v > (long)MAX_LEN) {
            fail(m, at, "%% is followed by a number of ticks (96 a quarter note)");
            return 0;
        }
        len = (uint32_t)v;
    } else if (number(p, 0, &v)) {
        if (v < 1 || v > WHOLE || WHOLE % v) {
            fail(m, at, "a length is a number that divides 384: 1 2 4 8 16 32 64, or 3 6 12 24 48 for triplets");
            return 0;
        }
        len = WHOLE / (uint32_t)v;
    } else if (need) {
        fail(m, at, "a length is missing");
        return 0;
    }
    uint32_t add = len;
    while (**p == '.') {
        (*p)++;
        if (add % 2) {
            fail(m, at, "too many dots for this length");
            return 0;
        }
        add /= 2;
        len += add;
    }
    return len;
}

/* A number from lo to hi after a command; fails with `what` if it is not there or out of range. */
static int ranged(mml_t *m, const char **p, int sign, long lo, long hi, const char *what, const char *at, long *out)
{
    if (!number(p, sign, out) || *out < lo || *out > hi) {
        fail(m, at, "%s is %ld to %ld", what, lo, hi);
        return 0;
    }
    return 1;
}

static void parse(mml_t *m, const char *p, const char *end, int depth);

/* The first (or, `last`, the last) character in [p, end) that is not a space or a bar line, or 0. */
static char edge(const char *p, const char *end, int last)
{
    for (; p < end; last ? end-- : p++) {
        const char c = last ? end[-1] : *p;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '|')
            return c;
    }
    return 0;
}

/* [ body ]count: a loop of events when every pass comes out the same; written out count times when not (see the top of the file). */
static void repeat(mml_t *m, const char *at, const char *body, const char *close, int count, int depth)
{
    if (m->tie || edge(body, close, 0) == '^' || edge(body, close, 0) == '&' || edge(body, close, 1) == '&') {
        for (int k = 0; k < count && !m->failed; k++)   /* a tie or a ^ across the brackets */
            parse(m, body, close, depth + 1);
        return;
    }
    flush(m, at);
    const snap_t before = snap(m);
    emit(m, at, (snd_event_t){.op = SND_EV_LOOP_START, .a = (uint8_t)count});
    const uint32_t start = m->n;
    const uint64_t outer = m->hash, ticks0 = m->ticks;
    m->hash = HASH_SEED;
    parse(m, body, close, depth + 1);
    flush(m, close);
    const snap_t first = snap(m);
    const uint64_t body_ticks = m->ticks - ticks0;
    int same = !m->failed && !m->tie && same_settings(&before, &first);
    if (!m->failed && !m->tie && !same) {
        m->hash = HASH_SEED;   /* a second pass, from where the first left off, to compare (and then taken back) */
        parse(m, body, close, depth + 1);
        flush(m, close);
        const snap_t second = snap(m);
        same = !m->failed && !m->tie && second.hash == first.hash && same_settings(&first, &second);
        if (!m->failed)
            rollback(m, &first);
    }
    if (m->failed)
        return;
    if (same) {
        m->hash = fold(outer, first.hash);
        emit(m, close, (snd_event_t){.op = SND_EV_LOOP_END, .gate = m->n - start + 1});
        m->ticks = sat_add(m->ticks, sat_mul(body_ticks, (uint64_t)count - 1));
        return;
    }
    rollback(m, &before);
    for (int k = 0; k < count && !m->failed; k++)
        parse(m, body, close, depth + 1);
}

static void parse(mml_t *m, const char *p, const char *end, int depth)
{
    static const int semis[7] = {9, 11, 0, 2, 4, 5, 7};   /* a b c d e f g */
    while (p < end && !m->failed) {
        const char *at = p;
        const char c = *p++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '|')
            continue;
        if (c == 'L') {
            if (depth)
                fail(m, at, "L cannot be inside [ ]");
            else if (m->loop >= 0)
                fail(m, at, "a track has only one L");
            flush(m, at);
            m->loop = (int32_t)m->n;
            m->loop_ticks = m->ticks;
            continue;
        }
        const char lc = (char)tolower((unsigned char)c);
        long v;
        uint32_t len;
        switch (lc) {
        case 'a': case 'b': case 'c': case 'd': case 'e': case 'f': case 'g': {
            int pitch = (m->octave + 1) * 12 + semis[lc - 'a'];
            while (*p == '+' || *p == '#' || *p == '-')
                pitch += *p++ == '-' ? -1 : 1;
            if ((len = length(m, &p, m->deflen, 0, at)))
                note(m, at, pitch + m->transpose, len);
            break;
        }
        case 'n':
            if (!ranged(m, &p, 0, 0, 127, "n's note", at, &v))
                break;
            if (*p == ',') {
                p++;
                len = length(m, &p, m->deflen, 1, at);
            } else {
                len = length(m, &p, m->deflen, 0, at);
            }
            if (len)
                note(m, at, (int)v + m->transpose, len);
            break;
        case 'r':
            if ((len = length(m, &p, m->deflen, 0, at)))
                rest(m, at, len);
            break;
        case '^':
            if (!m->pending) {
                fail(m, at, "^ needs a note or a rest before it");
                break;
            }
            if ((len = length(m, &p, m->deflen, 0, at)))
                lengthen(m, at, len);
            break;
        case '&':
            if (m->pending != 1)
                fail(m, at, "& needs a note before it");
            m->tie = 1;
            break;
        case 'l':
            if ((len = length(m, &p, m->deflen, 1, at)))
                m->deflen = len;
            break;
        case 'o':
            if (ranged(m, &p, 0, 0, 9, "the octave", at, &v))
                m->octave = (int)v;
            break;
        case '>':
            if (m->octave >= 9)
                fail(m, at, "> past octave 9");
            m->octave++;
            break;
        case '<':
            if (m->octave <= -1)
                fail(m, at, "< below octave -1");
            m->octave--;
            break;
        case 't':
            if (ranged(m, &p, 0, 20, 600, "the tempo", at, &v))
                setting(m, at, SND_EV_TEMPO, 0, (uint32_t)v);
            break;
        case 'v':
            if (ranged(m, &p, 0, 0, 15, "the volume", at, &v))
                setting(m, at, SND_EV_VOL, (uint8_t)(v * 17), 0);
            break;
        case 'q':
            if (ranged(m, &p, 0, 1, 8, "q", at, &v))
                m->q = (int)v;
            break;
        case '@':
            if (ranged(m, &p, 0, 0, SND_MAX_INSTRUMENTS - 1, "the instrument", at, &v))
                setting(m, at, SND_EV_INST, (uint8_t)v, 0);
            break;
        case 'p':
            if (ranged(m, &p, 1, -128, 127, "the pan", at, &v))
                setting(m, at, SND_EV_PAN, (uint8_t)(int8_t)v, 0);
            break;
        case 'k':
            if (ranged(m, &p, 1, -48, 48, "the transpose", at, &v))
                m->transpose = (int)v;
            break;
        case '[': {
            if (depth + 1 > MAX_DEPTH) {
                fail(m, at, "[ ] nested more than %d deep", MAX_DEPTH);
                break;
            }
            const char *close = p;
            for (int nest = 1; close < end; close++) {
                nest += *close == '[' ? 1 : *close == ']' ? -1 : 0;
                if (!nest)
                    break;
            }
            if (close >= end) {
                fail(m, at, "[ without its ]");
                break;
            }
            const char *after = close + 1;
            long count = 2;
            if (isdigit((unsigned char)*after) && (!number(&after, 0, &count) || count < 1 || count > 255)) {
                fail(m, close, "the count after ] is 1 to 255");
                break;
            }
            repeat(m, at, p, close, (int)count, depth);
            p = after;
            break;
        }
        case ']':
            fail(m, at, "] without its [");
            break;
        default:
            if (isprint((unsigned char)c))
                fail(m, at, "'%c' is not an MML command", c);
            else
                fail(m, at, "character %d is not an MML command", (unsigned char)c);
            break;
        }
    }
}

static void track(mml_t *m, const char *text, int number_, snd_event_t *out, uint32_t *total, char *err, size_t errlen)
{
    memset(m, 0, sizeof *m);
    m->text = text ? text : "";
    m->track = number_;
    m->out = out;
    m->total = total;
    m->loop = -1;
    m->octave = 4;
    m->q = 8;
    m->deflen = WHOLE / 4;
    m->hash = HASH_SEED;
    m->err = err;
    m->errlen = errlen;
    const char *end = m->text + strlen(m->text);
    parse(m, m->text, end, 0);
    flush(m, end);
    if (!m->failed && m->loop >= 0 && m->ticks == m->loop_ticks)
        fail(m, end, "nothing after L takes any time: the track would go round for ever without playing");
}

snd_song_t *snd_mml_compile(const char *const *tracks, int ntracks, const snd_instrument_t *instruments, void *(*alloc)(void *ctx, size_t n),
                            void *ctx, char *err, size_t errlen)
{
    if (errlen)
        err[0] = 0;
    if (ntracks < 1 || ntracks > SND_MAX_VOICES) {
        if (errlen)
            snprintf(err, errlen, "1 to %d tracks", SND_MAX_VOICES);
        return NULL;
    }
    uint32_t total = 0, peak[SND_MAX_VOICES];
    size_t room = 0;
    mml_t m;
    for (int i = 0; i < ntracks; i++) {
        track(&m, tracks[i], i + 1, NULL, &total, err, errlen);
        if (m.failed)
            return NULL;
        peak[i] = m.peak;   /* (room for the passes that are only compared, too) */
        room += m.peak;
    }
    const size_t bytes = sizeof(snd_song_t) + room * sizeof(snd_event_t);
    snd_song_t *song = alloc(ctx, bytes);
    if (!song) {
        if (errlen)
            snprintf(err, errlen, "not enough memory for the music (%lu bytes)", (unsigned long)bytes);
        return NULL;
    }
    memset(song, 0, sizeof *song);
    snd_event_t *ev = (snd_event_t *)(song + 1);
    total = 0;
    for (int i = 0; i < ntracks; i++) {
        track(&m, tracks[i], i + 1, ev, &total, err, errlen);   /* (the same text again: it cannot fail now) */
        song->track[i] = (snd_track_t){.events = ev, .count = m.n, .loop = m.loop};
        ev += peak[i];
    }
    song->tracks = ntracks;
    for (int i = 0; i < SND_MAX_INSTRUMENTS; i++) {
        if (instruments)
            song->instrument[i] = instruments[i];
        else
            snd_instrument_default(&song->instrument[i]);
    }
    return song;
}
