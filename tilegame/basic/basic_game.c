/* SPDX-License-Identifier: MIT */
#include "basic_game.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "my_basic/my_basic.h"
#include "platform.h"
#include "tg_actor.h"
#include "tg_world.h"

#define BG_MAX_ACTORS 128
#define BG_TILES 256                /* tiles in the map's set and in the sprite sheet */
#define BG_MAP_MAX (1 << 20)        /* the most tiles in a map */
#define BG_STEP_MAX 4               /* WAITFRAME moves the actors for at most this many frames at once */

/* The state of a run. Its tiles, sprites and window maps are what the display reads (the board wants them in SRAM: alloc hook, fast = 1). */
struct basic_game {
    tg_t g;
    int size_log2;                  /* 3 or 4: the size of tiles and of sprite tiles, 8 or 16 pixels */
    uint8_t *tiles, *sprites;       /* BG_TILES of size * size bytes each, one byte a pixel */
    uint8_t flags[TG_TILES_MAX];    /* what each tile number is (solid ...) */
    tg_world_t world;
    int has_world;
    uint16_t *win_a, *win_b;
    uint32_t *dirty;
    uint16_t *map;
    int32_t cam_x, cam_y;           /* before there is a map */
    tg_actors_t actors;
    tg_actor_t pool[BG_MAX_ACTORS];
    uint16_t order[4 * BG_MAX_ACTORS];
    uint32_t last_frame;            /* the frame number WAITFRAME last got (0: it has not been called) */
    char bg;                        /* the character of a graphics string that is pixel 0, see-through ('.' unless PALETTE changed it) */
};
typedef struct basic_game basic_game_t;

/* ---- memory and the state ---- */

static void *mem(basic_env_t *e, size_t n, int fast)
{
    void *p = e->hooks.alloc ? e->hooks.alloc(n, fast) : malloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}

static void unmem(basic_env_t *e, void *p)
{
    if (!p)
        return;
    if (e->hooks.release)
        e->hooks.release(p);
    else
        free(p);
}

static void free_world(basic_env_t *e, basic_game_t *G)
{
    unmem(e, G->win_a);
    unmem(e, G->win_b);
    unmem(e, G->dirty);
    unmem(e, G->map);
    G->win_a = G->win_b = NULL;
    G->dirty = NULL;
    G->map = NULL;
    G->has_world = 0;
    G->g.layers[0].kind = TG_LAYER_OFF;
    tg_actors_set_wrap(&G->actors, 0, 0);
}

static void free_all(basic_env_t *e)
{
    basic_game_t *G = e->game;
    if (G) {
        free_world(e, G);
        unmem(e, G->tiles);
        unmem(e, G->sprites);
        unmem(e, G);
    }
    e->game = NULL;
    e->game_active = 0;
}

/* The 16 colours of the PC's text mode, as RGB565; they are the palette entries of the characters 0 to 9 and A to F (the hex digits). */
static const uint16_t ega[16] = {0x0000, 0x0015, 0x0540, 0x0555, 0xA800, 0xA815, 0xAAA0, 0xAD55,
                                 0x52AA, 0x52BF, 0x57EA, 0x57FF, 0xFAAA, 0xFABF, 0xFFEA, 0xFFFF};

static int setup(basic_env_t *e, int size_log2)
{
    free_all(e);
    basic_game_t *G = mem(e, sizeof *G, 0);
    if (!G)
        return 0;
    const size_t bytes = (size_t)BG_TILES << (2 * size_log2);
    G->tiles = mem(e, bytes, 1);
    G->sprites = mem(e, bytes, 1);
    e->game = G;
    if (!G->tiles || !G->sprites) {
        free_all(e);
        return 0;
    }
    G->size_log2 = size_log2;
    tg_init(&G->g);
    G->g.mode = TG_MODE_TILES;
    G->g.sprite_tile_log2 = (uint8_t)size_log2;
    G->g.sprite_depth = 8;
    G->g.sprite_tiles = G->sprites;
    for (int i = 0; i < 16; i++)
        G->g.palette[(unsigned char)"0123456789ABCDEF"[i]] = ega[i];
    G->g.palette[0] = ega[0];   /* (the backdrop) */
    G->bg = '.';
    tg_actors_init(&G->actors, G->pool, BG_MAX_ACTORS, G->order);
    G->actors.sprite_log2 = (uint8_t)size_log2;   /* (collision boxes before the first draw) */
    return 1;
}

static int fail(struct mb_interpreter_t *s, void **l, mb_error_e err, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int fail(struct mb_interpreter_t *s, void **l, mb_error_e err, const char *fmt, ...)
{
    char text[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    basic_env_problem(basic_env_current(), "%s", text);
    return mb_raise_error(s, l, err, MB_FUNC_ERR);
}

/* ---- the functions ---- */

#define FUNC(n) static int bf_##n(struct mb_interpreter_t *s, void **l)
#define CK(x) do { const int ck_ = (x); if (ck_ != MB_FUNC_OK) return ck_; } while (0)
#define OPEN() CK(mb_attempt_open_bracket(s, l))
#define CLOSE() CK(mb_attempt_close_bracket(s, l))
#define RET(v) CK(mb_push_int(s, l, (int_t)(v)))
#define RET_REAL(v) CK(mb_push_real(s, l, (real_t)(v)))
#define NEED_GAME()                                                                                          \
    basic_env_t *e = basic_env_current();                                                                    \
    if (!e->game && !setup(e, 3))                                                                            \
        return fail(s, l, SE_RN_OUT_OF_MEMORY, "Not enough memory for the graphics");                        \
    basic_game_t *G = e->game
#define NEED_WORLD()                                                                                         \
    if (!G->has_world)                                                                                       \
        return fail(s, l, SE_RN_INVALID_ID_USAGE, "There is no map: NEWMAP(width, height) makes one")

static int arg_int(struct mb_interpreter_t *s, void **l, int *v)
{
    int_t t = 0;
    const int r = mb_pop_int(s, l, &t);
    *v = (int)t;
    return r;
}

static int arg_real(struct mb_interpreter_t *s, void **l, double *v)
{
    real_t t = 0;
    const int r = mb_pop_real(s, l, &t);
    *v = (double)t;
    return r;
}

/* Pixels (or a fraction) to the actors' 24.8 fixed point, rounded and kept in range. */
static int32_t fixed(double v)
{
    v *= 256.0;
    if (v >= 2147483647.0)
        return INT32_MAX;
    if (v <= -2147483648.0)
        return INT32_MIN;
    return (int32_t)floor(v + 0.5);
}

static int32_t whole(double v)
{
    if (v >= 2147483647.0)
        return INT32_MAX;
    if (v <= -2147483648.0)
        return INT32_MIN;
    return (int32_t)floor(v);
}

FUNC(gfx)
{
    int size;
    OPEN();
    CK(arg_int(s, l, &size));
    CLOSE();
    basic_env_t *e = basic_env_current();
    if (size != 8 && size != 16)
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "GFX(size): the size of a tile is 8 or 16");
    if (!setup(e, size == 8 ? 3 : 4))
        return fail(s, l, SE_RN_OUT_OF_MEMORY, "Not enough memory for the graphics");
    RET(0);
    return MB_FUNC_OK;
}

/*
 * PALETTE(c, red, green, blue): a colour is a palette entry, and the entry of a character is its code ("A" is 65), the same character a graphics string
 * uses for it; a number is the entry itself (0 is the backdrop, which no character can name). A negative red, green or blue with a character makes
 * that character the one that means see-through in graphics strings (it is "." at first).
 */
FUNC(palette)
{
    int r, g, b;
    mb_value_t v;
    OPEN();
    mb_make_nil(v);
    CK(mb_pop_value(s, l, &v));
    CK(arg_int(s, l, &r));
    CK(arg_int(s, l, &g));
    CK(arg_int(s, l, &b));
    CLOSE();
    NEED_GAME();
    int i, is_char = 0;
    if (v.type == MB_DT_INT)
        i = (int)v.value.integer;
    else if (v.type == MB_DT_REAL)
        i = (int)v.value.float_point;
    else if (v.type == MB_DT_STRING && v.value.string && v.value.string[0] && !v.value.string[1]) {
        i = (unsigned char)v.value.string[0];
        is_char = 1;
    } else
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "PALETTE(colour, red, green, blue): the colour is one character, or a number 0 to 255");
    if (r < 0 || g < 0 || b < 0) {
        if (!is_char)
            return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "PALETTE(colour, -1, -1, -1) needs a character, such as \"@\", for the colour");
        G->bg = (char)i;
        RET(0);
        return MB_FUNC_OK;
    }
    if (i < 0 || i > 255 || r > 255 || g > 255 || b > 255)
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "PALETTE(colour, red, green, blue): the colour is a character or 0 to 255, red, green and blue 0 to 255");
    G->g.palette[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    RET(0);
    return MB_FUNC_OK;
}

/* A character of a graphics string: the background character is 0 (see-through), any other is its own code, the palette entry PALETTE gave it. */
static uint8_t pixel_of(const basic_game_t *G, char c)
{
    return c == G->bg ? 0 : (uint8_t)c;
}

static int define_tile(struct mb_interpreter_t *s, void **l, int sprites)
{
    int n;
    char *text = NULL;
    OPEN();
    CK(arg_int(s, l, &n));
    CK(mb_pop_string(s, l, &text));
    CLOSE();
    NEED_GAME();
    if (n < 0 || n >= BG_TILES)
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "%s(n, graphics): n is 0 to %d", sprites ? "SPRDEF" : "TILEDEF", BG_TILES - 1);
    const size_t px = (size_t)1 << (2 * G->size_log2);
    uint8_t *dst = (sprites ? G->sprites : G->tiles) + (size_t)n * px;
    const size_t len = text ? strlen(text) : 0;
    for (size_t i = 0; i < px; i++)
        dst[i] = i < len ? pixel_of(G, text[i]) : 0;
    RET(0);
    return MB_FUNC_OK;
}

FUNC(tiledef) { return define_tile(s, l, 0); }
FUNC(sprdef) { return define_tile(s, l, 1); }

/*
 * TILEROW(n, y, graphics): one row of a tile (its size in characters). With y = 0 and a string long enough for every pixel of the tile it is the whole tile,
 * row by row (like TILEDEF): a string in a program is at most 128 characters in MY-BASIC, so a 16 pixel tile is made by joining strings.
 */
static int define_row(struct mb_interpreter_t *s, void **l, int sprites)
{
    int n, y;
    char *text = NULL;
    OPEN();
    CK(arg_int(s, l, &n));
    CK(arg_int(s, l, &y));
    CK(mb_pop_string(s, l, &text));
    CLOSE();
    NEED_GAME();
    const int size = 1 << G->size_log2;
    if (n < 0 || n >= BG_TILES || y < 0 || y >= size)
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "%s(n, y, graphics): n is 0 to %d, y 0 to %d", sprites ? "SPRROW" : "TILEROW", BG_TILES - 1, size - 1);
    uint8_t *dst = (sprites ? G->sprites : G->tiles) + ((size_t)n << (2 * G->size_log2)) + (size_t)y * (size_t)size;
    const size_t len = text ? strlen(text) : 0;
    const size_t count = y == 0 && len >= (size_t)size * (size_t)size ? (size_t)size * (size_t)size : (size_t)size;
    for (size_t i = 0; i < count; i++)
        dst[i] = i < len ? pixel_of(G, text[i]) : 0;
    RET(0);
    return MB_FUNC_OK;
}

FUNC(tilerow) { return define_row(s, l, 0); }
FUNC(sprrow) { return define_row(s, l, 1); }

static int set_pixel(struct mb_interpreter_t *s, void **l, int sprites)
{
    int n, x, y, c;
    OPEN();
    CK(arg_int(s, l, &n));
    CK(arg_int(s, l, &x));
    CK(arg_int(s, l, &y));
    CK(arg_int(s, l, &c));
    CLOSE();
    NEED_GAME();
    const int size = 1 << G->size_log2;
    if (n < 0 || n >= BG_TILES || x < 0 || x >= size || y < 0 || y >= size || c < 0 || c > 255)
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "%s(n, x, y, colour): n 0 to %d, x and y 0 to %d, colour 0 to 255", sprites ? "SPRPIX" : "TILEPIX", BG_TILES - 1, size - 1);
    (sprites ? G->sprites : G->tiles)[((size_t)n << (2 * G->size_log2)) + (size_t)y * (size_t)size + (size_t)x] = (uint8_t)c;
    RET(0);
    return MB_FUNC_OK;
}

FUNC(tilepix) { return set_pixel(s, l, 0); }
FUNC(sprpix) { return set_pixel(s, l, 1); }

FUNC(newmap)
{
    int w, h;
    OPEN();
    CK(arg_int(s, l, &w));
    CK(arg_int(s, l, &h));
    CLOSE();
    NEED_GAME();
    if (w < 1 || h < 1 || (long)w * h > BG_MAP_MAX)
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "NEWMAP(width, height): the map has at most %d tiles", BG_MAP_MAX);
    free_world(e, G);
    /* the window: what the display reads, a little more than the screen (double buffered, so no border is needed) */
    const unsigned wl = G->size_log2 == 3 ? 6 : 5, hl = G->size_log2 == 3 ? 5 : 4;
    G->win_a = mem(e, TG_WORLD_WIN_ENTRIES(wl, hl) * sizeof *G->win_a, 1);
    G->win_b = mem(e, TG_WORLD_WIN_ENTRIES(wl, hl) * sizeof *G->win_b, 1);
    G->dirty = mem(e, TG_WORLD_DIRTY_WORDS(wl, hl) * sizeof *G->dirty, 0);
    G->map = mem(e, (size_t)w * (size_t)h * sizeof *G->map, 0);
    if (!G->win_a || !G->win_b || !G->dirty || !G->map) {
        free_world(e, G);
        return fail(s, l, SE_RN_OUT_OF_MEMORY, "Not enough memory for a map of %d x %d", w, h);
    }
    G->g.layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = (uint8_t)G->size_log2, .depth = 8, .map_w_log2 = (uint8_t)wl,
                                  .map_h_log2 = (uint8_t)hl, .tiles = G->tiles};
    if (!tg_world_init(&G->world, &G->g.layers[0], G->win_a, G->win_b, G->dirty, G->map, w, h)) {
        free_world(e, G);
        return fail(s, l, SE_RN_INVALID_ID_USAGE, "The map could not be set up");
    }
    tg_world_set_flags(&G->world, G->flags);
    G->has_world = 1;
    tg_world_set_camera(&G->world, G->cam_x, G->cam_y);
    RET(0);
    return MB_FUNC_OK;
}

/* WRAP(1): the map repeats in every direction (infinite scrolling); WRAP(0): it does not. Returns 1 if the actors wrap too (they need the map to be at least 192 x 160 pixels). */
FUNC(wrap)
{
    int on;
    OPEN();
    CK(arg_int(s, l, &on));
    CLOSE();
    NEED_GAME();
    NEED_WORLD();
    tg_world_set_border(&G->world, on ? TG_BORDER_WRAP : TG_BORDER_TILE, 0);
    int32_t pw = 0, ph = 0;
    const int actors = on && tg_world_wrap_px(&G->world, &pw, &ph) && tg_actors_set_wrap(&G->actors, pw, ph);
    if (!actors)
        tg_actors_set_wrap(&G->actors, 0, 0);
    RET(actors);
    return MB_FUNC_OK;
}

FUNC(tile)
{
    int x, y, n, flip = 0;
    OPEN();
    CK(arg_int(s, l, &x));
    CK(arg_int(s, l, &y));
    CK(arg_int(s, l, &n));
    if (mb_has_arg(s, l))
        CK(arg_int(s, l, &flip));
    CLOSE();
    NEED_GAME();
    NEED_WORLD();
    if (n < 0 || n >= BG_TILES)
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "TILE(x, y, n): n is 0 to %d", BG_TILES - 1);
    tg_world_set(&G->world, x, y, TG_ENTRY(n, 0, ((flip & 1) ? TG_HFLIP : 0) | ((flip & 2) ? TG_VFLIP : 0)));
    RET(0);
    return MB_FUNC_OK;
}

FUNC(gettile)
{
    int x, y;
    OPEN();
    CK(arg_int(s, l, &x));
    CK(arg_int(s, l, &y));
    CLOSE();
    NEED_GAME();
    NEED_WORLD();
    RET(tg_world_get(&G->world, x, y) & TG_TILE_MASK);
    return MB_FUNC_OK;
}

FUNC(tilefill)
{
    int x, y, w, h, n;
    OPEN();
    CK(arg_int(s, l, &x));
    CK(arg_int(s, l, &y));
    CK(arg_int(s, l, &w));
    CK(arg_int(s, l, &h));
    CK(arg_int(s, l, &n));
    CLOSE();
    NEED_GAME();
    NEED_WORLD();
    if (n < 0 || n >= BG_TILES)
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "TILEFILL(x, y, width, height, n): n is 0 to %d", BG_TILES - 1);
    tg_world_fill(&G->world, x, y, w, h, TG_ENTRY(n, 0, 0));
    RET(0);
    return MB_FUNC_OK;
}

/*
 * TILESTR(x, y, width, s [, offset]): the bytes of the string are tile numbers (the byte plus the offset, 0 if it is not given) put in the map one after the
 * other, `width` to a row from x, y, then the next row. A string cannot hold a 0, so the offset is how a program reaches tile 0 (-32 makes a space tile 0).
 */
FUNC(tilestr)
{
    int x, y, w, offset = 0;
    char *text = NULL;
    OPEN();
    CK(arg_int(s, l, &x));
    CK(arg_int(s, l, &y));
    CK(arg_int(s, l, &w));
    CK(mb_pop_string(s, l, &text));
    if (mb_has_arg(s, l))
        CK(arg_int(s, l, &offset));
    CLOSE();
    NEED_GAME();
    NEED_WORLD();
    const size_t len = text ? strlen(text) : 0;
    if (w < 1)
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "TILESTR(x, y, width, string): the width is 1 or more");
    for (size_t i = 0; i < len; i++) {
        const int n = (unsigned char)text[i] + offset;
        if (n < 0 || n >= BG_TILES)
            return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "TILESTR: character %d is tile %d, tiles are 0 to %d", (int)i + 1, n, BG_TILES - 1);
    }
    for (size_t i = 0; i < len; i++)
        tg_world_set(&G->world, x + (int)(i % (size_t)w), y + (int)(i / (size_t)w), TG_ENTRY((unsigned char)text[i] + offset, 0, 0));
    RET(0);
    return MB_FUNC_OK;
}

FUNC(settileflags)
{
    int n, f;
    OPEN();
    CK(arg_int(s, l, &n));
    CK(arg_int(s, l, &f));
    CLOSE();
    NEED_GAME();
    if (n < 0 || n >= BG_TILES || f < 0 || f > 255)
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "SETTILEFLAGS(n, flags): n is 0 to %d, flags 0 to 255 (1 solid, 2 one-way)", BG_TILES - 1);
    G->flags[n] = (uint8_t)f;
    RET(0);
    return MB_FUNC_OK;
}

FUNC(tileflag)
{
    double x, y;
    OPEN();
    CK(arg_real(s, l, &x));
    CK(arg_real(s, l, &y));
    CLOSE();
    NEED_GAME();
    NEED_WORLD();
    RET(tg_world_flags_px(&G->world, whole(x), whole(y)));
    return MB_FUNC_OK;
}

FUNC(tilebox)
{
    double x, y, w, h;
    OPEN();
    CK(arg_real(s, l, &x));
    CK(arg_real(s, l, &y));
    CK(arg_real(s, l, &w));
    CK(arg_real(s, l, &h));
    CLOSE();
    NEED_GAME();
    NEED_WORLD();
    RET(tg_world_flags_box(&G->world, whole(x), whole(y), whole(w), whole(h)));
    return MB_FUNC_OK;
}

FUNC(camera)
{
    double x, y;
    OPEN();
    CK(arg_real(s, l, &x));
    CK(arg_real(s, l, &y));
    CLOSE();
    NEED_GAME();
    G->cam_x = whole(x);
    G->cam_y = whole(y);
    if (G->has_world)
        tg_world_set_camera(&G->world, G->cam_x, G->cam_y);
    RET(0);
    return MB_FUNC_OK;
}

FUNC(camx)
{
    OPEN();
    CLOSE();
    NEED_GAME();
    RET(G->has_world ? G->world.cam_x : G->cam_x);
    return MB_FUNC_OK;
}

FUNC(camy)
{
    OPEN();
    CLOSE();
    NEED_GAME();
    RET(G->has_world ? G->world.cam_y : G->cam_y);
    return MB_FUNC_OK;
}

/* ---- actors ---- */

static tg_actor_t *actor_arg(struct mb_interpreter_t *s, void **l, basic_game_t *G, int id, const char *who)
{
    tg_actor_t *a = tg_actor(&G->actors, id);
    if (!a)
        fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "%s: there is no actor %d", who, id);
    return a;
}

#define NEED_ACTOR(who)                                                                                      \
    tg_actor_t *a = actor_arg(s, l, G, id, who);                                                             \
    if (!a)                                                                                                  \
        return MB_FUNC_ERR

FUNC(actor)
{
    int tile;
    double x, y;
    OPEN();
    CK(arg_int(s, l, &tile));
    CK(arg_real(s, l, &x));
    CK(arg_real(s, l, &y));
    CLOSE();
    NEED_GAME();
    if (tile < 0 || tile >= BG_TILES)
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "ACTOR(tile, x, y): tile is a sprite, 0 to %d", BG_TILES - 1);
    const int id = tg_actor_alloc(&G->actors);
    if (id >= 0) {
        tg_actor_t *a = tg_actor(&G->actors, id);
        a->tile = (uint16_t)tile;
        a->above = TG_MAX_LAYERS;               /* in front of the map (0 would be behind it) */
        a->x = fixed(x);
        a->y = fixed(y);
    }
    RET(id);                                    /* -1 when all 128 are in use */
    return MB_FUNC_OK;
}

FUNC(actorfree)
{
    int id;
    OPEN();
    CK(arg_int(s, l, &id));
    CLOSE();
    NEED_GAME();
    NEED_ACTOR("ACTORFREE");
    tg_actor_free(&G->actors, id);
    RET(0);
    return MB_FUNC_OK;
}

FUNC(actorpos)
{
    int id;
    double x, y;
    OPEN();
    CK(arg_int(s, l, &id));
    CK(arg_real(s, l, &x));
    CK(arg_real(s, l, &y));
    CLOSE();
    NEED_GAME();
    NEED_ACTOR("ACTORPOS");
    a->x = fixed(x);
    a->y = fixed(y);
    RET(0);
    return MB_FUNC_OK;
}

FUNC(actorx)
{
    int id;
    OPEN();
    CK(arg_int(s, l, &id));
    CLOSE();
    NEED_GAME();
    NEED_ACTOR("ACTORX");
    RET_REAL(a->x / 256.0);
    return MB_FUNC_OK;
}

FUNC(actory)
{
    int id;
    OPEN();
    CK(arg_int(s, l, &id));
    CLOSE();
    NEED_GAME();
    NEED_ACTOR("ACTORY");
    RET_REAL(a->y / 256.0);
    return MB_FUNC_OK;
}

FUNC(actorvel)
{
    int id;
    double vx, vy;
    OPEN();
    CK(arg_int(s, l, &id));
    CK(arg_real(s, l, &vx));
    CK(arg_real(s, l, &vy));
    CLOSE();
    NEED_GAME();
    NEED_ACTOR("ACTORVEL");
    a->vx = fixed(vx);
    a->vy = fixed(vy);
    RET(0);
    return MB_FUNC_OK;
}

FUNC(actortile)
{
    int id, tile;
    OPEN();
    CK(arg_int(s, l, &id));
    CK(arg_int(s, l, &tile));
    CLOSE();
    NEED_GAME();
    NEED_ACTOR("ACTORTILE");
    if (tile < 0 || tile >= BG_TILES)
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "ACTORTILE(actor, tile): tile is a sprite, 0 to %d", BG_TILES - 1);
    a->tile = (uint16_t)tile;
    RET(0);
    return MB_FUNC_OK;
}

FUNC(actorsize)
{
    int id, cols, rows;
    OPEN();
    CK(arg_int(s, l, &id));
    CK(arg_int(s, l, &cols));
    CK(arg_int(s, l, &rows));
    CLOSE();
    NEED_GAME();
    NEED_ACTOR("ACTORSIZE");
    if (cols < 1 || cols > 4 || rows < 1 || rows > 4)
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "ACTORSIZE(actor, columns, rows): 1 to 4 of each (sprites side by side, row by row)");
    a->cols = (uint8_t)cols;
    a->rows = (uint8_t)rows;
    RET(0);
    return MB_FUNC_OK;
}

FUNC(actorflip)
{
    int id, f;
    OPEN();
    CK(arg_int(s, l, &id));
    CK(arg_int(s, l, &f));
    CLOSE();
    NEED_GAME();
    NEED_ACTOR("ACTORFLIP");
    a->flags = (uint8_t)((a->flags & ~(TG_ACTOR_HFLIP | TG_ACTOR_VFLIP)) | ((f & 1) ? TG_ACTOR_HFLIP : 0) | ((f & 2) ? TG_ACTOR_VFLIP : 0));
    RET(0);
    return MB_FUNC_OK;
}

FUNC(actorhide)
{
    int id, on;
    OPEN();
    CK(arg_int(s, l, &id));
    CK(arg_int(s, l, &on));
    CLOSE();
    NEED_GAME();
    NEED_ACTOR("ACTORHIDE");
    a->flags = (uint8_t)(on ? (a->flags | TG_ACTOR_HIDDEN) : (a->flags & ~TG_ACTOR_HIDDEN));
    RET(0);
    return MB_FUNC_OK;
}

FUNC(actorprio)
{
    int id, p;
    OPEN();
    CK(arg_int(s, l, &id));
    CK(arg_int(s, l, &p));
    CLOSE();
    NEED_GAME();
    NEED_ACTOR("ACTORPRIO");
    if (p < 0 || p > 255)
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "ACTORPRIO(actor, priority): 0 (in front) to 255");
    a->prio = (uint8_t)p;
    RET(0);
    return MB_FUNC_OK;
}

/* ACTORBOX(actor, x, y, width, height): the part of the actor that collides, in pixels from its top left; a width or height of 0 is all of it. */
FUNC(actorbox)
{
    int id, x, y, w, h;
    OPEN();
    CK(arg_int(s, l, &id));
    CK(arg_int(s, l, &x));
    CK(arg_int(s, l, &y));
    CK(arg_int(s, l, &w));
    CK(arg_int(s, l, &h));
    CLOSE();
    NEED_GAME();
    NEED_ACTOR("ACTORBOX");
    if (x < 0 || x > 255 || y < 0 || y > 255 || w < 0 || w > 255 || h < 0 || h > 255)
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "ACTORBOX(actor, x, y, width, height): each 0 to 255");
    a->hit_x = (uint8_t)x;
    a->hit_y = (uint8_t)y;
    a->hit_w = (uint8_t)w;
    a->hit_h = (uint8_t)h;
    RET(0);
    return MB_FUNC_OK;
}

/* ACTORUSER(actor) reads the number the program keeps in an actor (0 to 65535); ACTORUSER(actor, n) sets it. */
FUNC(actoruser)
{
    int id, n = 0, set = 0;
    OPEN();
    CK(arg_int(s, l, &id));
    if (mb_has_arg(s, l)) {
        CK(arg_int(s, l, &n));
        set = 1;
    }
    CLOSE();
    NEED_GAME();
    NEED_ACTOR("ACTORUSER");
    if (set) {
        if (n < 0 || n > 65535)
            return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "ACTORUSER(actor, n): n is 0 to 65535");
        a->user = (uint16_t)n;
    }
    RET(a->user);
    return MB_FUNC_OK;
}

FUNC(ysort)
{
    int on;
    OPEN();
    CK(arg_int(s, l, &on));
    CLOSE();
    NEED_GAME();
    G->actors.flags = (uint8_t)(on ? TG_ACTORS_YSORT : 0);
    RET(0);
    return MB_FUNC_OK;
}

/* ACTORMOVE(actor, dx, dy): moves by dx and dy pixels, stopping at solid tiles; returns the sides that stopped it (1 left, 2 right, 4 up, 8 down). */
FUNC(actormove)
{
    int id;
    double dx, dy;
    OPEN();
    CK(arg_int(s, l, &id));
    CK(arg_real(s, l, &dx));
    CK(arg_real(s, l, &dy));
    CLOSE();
    NEED_GAME();
    NEED_WORLD();
    NEED_ACTOR("ACTORMOVE");
    (void)a;
    RET(tg_actor_move(&G->actors, id, &G->world, fixed(dx), fixed(dy)));
    return MB_FUNC_OK;
}

FUNC(actorhit)
{
    int a, b;
    OPEN();
    CK(arg_int(s, l, &a));
    CK(arg_int(s, l, &b));
    CLOSE();
    NEED_GAME();
    RET(tg_actors_overlap(&G->actors, a, b) ? 1 : 0);
    return MB_FUNC_OK;
}

/* ---- the frame, the clock and the buttons ---- */

/*
 * Shows the picture (the actors get their sprites, the maps are swapped) and waits for the next frame. Returns the frames that went by since the last time
 * (1 to BG_STEP_MAX); the actors move by that many frames of their velocity.
 */
static int show_and_wait(basic_env_t *e, basic_game_t *G)
{
    G->g.mode = TG_MODE_TILES;
    G->g.text = NULL;
    if (G->has_world)
        tg_world_end_frame(&G->world);
    tg_actors_draw(&G->actors, &G->g, G->has_world ? G->world.cam_x : G->cam_x, G->has_world ? G->world.cam_y : G->cam_y);
    e->hooks.present(&G->g);
    e->game_active = 1;
    const uint32_t f = e->hooks.wait_frame();
    int n = G->last_frame ? (int)(f - G->last_frame) : 1;
    if (n < 1)
        n = 1;
    if (n > BG_STEP_MAX)
        n = BG_STEP_MAX;
    G->last_frame = f;
    tg_actors_step(&G->actors, n);
    if (G->has_world)
        tg_world_begin_frame(&G->world);
    return n;
}

FUNC(waitframe)
{
    OPEN();
    CLOSE();
    NEED_GAME();
    if (!e->hooks.wait_frame || !e->hooks.present)
        return fail(s, l, SE_RN_INVALID_ID_USAGE, "This machine has no tile screen for WAITFRAME");
    RET(show_and_wait(e, G));
    return MB_FUNC_OK;
}

/* TEXTMODE(): the text screen now; the graphics are kept, and the next WAITFRAME shows the picture again. */
FUNC(textmode)
{
    OPEN();
    CLOSE();
    basic_env_t *e = basic_env_current();
    if (e->game_active) {
        e->game_active = 0;
        e->hooks.frame();
    }
    RET(0);
    return MB_FUNC_OK;
}

FUNC(frames)
{
    OPEN();
    CLOSE();
    NEED_GAME();
    RET(G->last_frame);
    return MB_FUNC_OK;
}

FUNC(millis)
{
    OPEN();
    CLOSE();
    basic_env_t *e = basic_env_current();
    RET(e->hooks.millis ? e->hooks.millis() : 0);
    return MB_FUNC_OK;
}

static int button_mask(const char *name)
{
    static const struct { const char *name; int mask; } names[] = {
        {"up", PLAT_BTN_UP}, {"down", PLAT_BTN_DOWN}, {"left", PLAT_BTN_LEFT}, {"right", PLAT_BTN_RIGHT}, {"a", PLAT_BTN_A}, {"b", PLAT_BTN_B},
        {"x", PLAT_BTN_X}, {"y", PLAT_BTN_Y}, {"start", PLAT_BTN_START}, {"select", PLAT_BTN_SELECT}, {"l", PLAT_BTN_L}, {"r", PLAT_BTN_R},
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        const char *p = names[i].name, *q = name;
        while (*p && *q && tolower((unsigned char)*q) == *p) {
            p++;
            q++;
        }
        if (!*p && !*q)
            return names[i].mask;
    }
    return 0;
}

/* BTN(player, button): the button is a number (1 up, 2 down, 4 left, 8 right, 16 A, 32 B, 64 X, 128 Y, 256 Start, 512 Select, 1024 L, 2048 R; add them
 * for any of several) or its name: "up", "a", "start" ... */
static int button_state(struct mb_interpreter_t *s, void **l, int kind)
{
    int player;
    mb_value_t v;
    OPEN();
    CK(arg_int(s, l, &player));
    mb_make_nil(v);
    CK(mb_pop_value(s, l, &v));
    CLOSE();
    basic_env_t *e = basic_env_current();
    int mask;
    if (v.type == MB_DT_INT)
        mask = (int)v.value.integer;
    else if (v.type == MB_DT_REAL)
        mask = (int)v.value.float_point;
    else if (v.type == MB_DT_STRING && v.value.string && (mask = button_mask(v.value.string)) != 0)
        ;
    else
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "%s(player, button): a number, or \"up\" \"down\" \"left\" \"right\" \"a\" \"b\" \"x\" \"y\" \"start\" \"select\" \"l\" \"r\"",
                    kind ? "PRESSED" : "BTN");
    if (player < 0 || player >= PLAT_MAX_PLAYERS)
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "The player is 0 to %d", PLAT_MAX_PLAYERS - 1);
    RET(e->hooks.buttons && (e->hooks.buttons(player, kind) & (unsigned)mask) ? 1 : 0);
    return MB_FUNC_OK;
}

FUNC(btn) { return button_state(s, l, 0); }
FUNC(pressed) { return button_state(s, l, 1); }

/* ---- registration, and the run ---- */

#define F(n) {#n, bf_##n}
static const struct { const char *name; mb_func_t fn; } functions[] = {
    F(gfx), F(palette), F(tiledef), F(sprdef), F(tilerow), F(sprrow), F(tilepix), F(sprpix),
    F(newmap), F(wrap), F(tile), F(gettile), F(tilefill), F(tilestr), F(settileflags), F(tileflag), F(tilebox), F(camera), F(camx), F(camy),
    F(actor), F(actorfree), F(actorpos), F(actorx), F(actory), F(actorvel), F(actortile), F(actorsize), F(actorflip), F(actorhide), F(actorprio),
    F(actorbox), F(actoruser), F(ysort), F(actormove), F(actorhit),
    F(waitframe), F(textmode), F(frames), F(millis), F(btn), F(pressed),
};

void basic_game_register(struct mb_interpreter_t *s)
{
    for (size_t i = 0; i < sizeof functions / sizeof *functions; i++)
        mb_register_func(s, functions[i].name, functions[i].fn);
}

void basic_game_begin_run(basic_env_t *e)
{
    free_all(e);
}

void basic_game_end_run(basic_env_t *e)
{
    free_all(e);
}

void basic_game_close(basic_env_t *e)
{
    free_all(e);
}
