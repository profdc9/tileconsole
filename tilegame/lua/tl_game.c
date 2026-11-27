/* SPDX-License-Identifier: MIT */
#include "tl_game.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "platform.h"
#include "tg_actor.h"
#include "tg_world.h"
#include "tgf.h"
#include "tilefs.h"

#define TL_MAX_ACTORS 128
#define TL_TILES 256                /* tiles in the map's set and in the sprite sheet */
#define TL_MAP_MAX (1 << 20)        /* the most tiles in a map */
#define TL_MAX_ANIMS 32             /* animation tables, numbered from 1 */
#define TL_ANIM_FRAMES 32           /* frames in a table */
#define TL_STEP_MAX 4               /* waitframe moves the actors for at most this many frames at once */

/* A tile layer of the picture and its map (tg_world). Lua numbers them 1 to TG_MAX_LAYERS, C from 0; 0 is drawn at the back. All of them use the one set of tiles. */
typedef struct {
    tg_world_t world;
    int has_world;
    uint16_t *win_a, *win_b;
    uint32_t *dirty;
    uint16_t *map;
    float fx, fy;                   /* parallax: the layer's camera is the main camera times fx, fy, plus ox, oy (1, 1, 0, 0 at first) */
    int32_t ox, oy;
} tl_layer_t;

/* The game's state. Its tiles, sprites and window maps are what the display reads (the board wants them in SRAM: alloc hook, fast = 1). */
struct tl_game {
    tg_t g;
    int size_log2;                  /* 3 or 4: the size of tiles and of sprite tiles, 8 or 16 pixels */
    uint8_t *tiles, *sprites;       /* TL_TILES of size * size bytes each, one byte a pixel */
    uint8_t flags[TG_TILES_MAX];    /* what each tile number is (solid ...) */
    uint8_t rgb[256][3];            /* the palette as palette() was given it, 0 to 255 each (the display's is RGB565): what savegfx writes */
    tl_layer_t lay[TG_MAX_LAYERS];
    int cur;                        /* the layer the map functions work on (layer(n)) */
    int solid;                      /* the layer whose tiles stop the actors and whose map follow keeps the camera inside (solidlayer(n)) */
    int32_t cam_x, cam_y;           /* the main camera, in the solid layer's pixels; it does not wrap (the layers' own cameras do) */
    tg_actors_t actors;
    tg_actor_t pool[TL_MAX_ACTORS];
    uint16_t order[4 * TL_MAX_ACTORS];
    tg_anim_t anims[TL_MAX_ANIMS];
    tg_anim_frame_t anim_frames[TL_MAX_ANIMS][TL_ANIM_FRAMES];
    uint32_t last_frame;            /* the frame number waitframe last got (0: it has not been called) */
    char bg;                        /* the character of a graphics string that is pixel 0, see-through ('.' unless palette changed it) */
    int frame_ref;                  /* the routine onframe gave (a reference in Lua's registry), LUA_NOREF if none */
    int in_run;                     /* run() is looping: waitframe is an error */
    int follow_id;                  /* the actor the camera follows, -1: none */
    int follow_dw, follow_dh;       /* the dead zone, in pixels: the actor's centre moves in it without the camera moving */
    int follow_smooth;              /* 0: the camera goes straight to where it should be; n: it covers 1/(n+1) of the way each frame */
    int follow_limits;              /* keep the camera inside a map that does not repeat */
};
typedef struct tl_game tl_game_t;

/* ---- memory and the state ---- */

static void *mem(tl_env_t *e, size_t n, int fast)
{
    void *p = e->hooks.alloc ? e->hooks.alloc(n, fast) : malloc(n);
    if (p)
        memset(p, 0, n);
    return p;
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

static void free_layer(tl_env_t *e, tl_game_t *G, int i)
{
    tl_layer_t *l = &G->lay[i];
    unmem(e, l->win_a);
    unmem(e, l->win_b);
    unmem(e, l->dirty);
    unmem(e, l->map);
    l->win_a = l->win_b = NULL;
    l->dirty = NULL;
    l->map = NULL;
    l->has_world = 0;
    G->g.layers[i].kind = TG_LAYER_OFF;
    if (i == G->solid)
        tg_actors_set_wrap(&G->actors, 0, 0);
}

void tl_game_reset(tl_env_t *e)
{
    tl_game_t *G = e->game;
    if (G) {
        for (int i = 0; i < TG_MAX_LAYERS; i++)   /* (the frame routine's reference goes with the Lua state, which is closed next) */
            free_layer(e, G, i);
        unmem(e, G->tiles);
        unmem(e, G->sprites);
        unmem(e, G);
    }
    e->game = NULL;
    e->game_active = 0;
}

/* The 16 colours of the PC's text mode; they are the palette entries of the characters 0 to 9 and A to F (the hex digits). */
static const uint8_t ega[16][3] = {{0x00, 0x00, 0x00}, {0x00, 0x00, 0xAA}, {0x00, 0xAA, 0x00}, {0x00, 0xAA, 0xAA}, {0xAA, 0x00, 0x00}, {0xAA, 0x00, 0xAA},
                                   {0xAA, 0x55, 0x00}, {0xAA, 0xAA, 0xAA}, {0x55, 0x55, 0x55}, {0x55, 0x55, 0xFF}, {0x55, 0xFF, 0x55}, {0x55, 0xFF, 0xFF},
                                   {0xFF, 0x55, 0x55}, {0xFF, 0x55, 0xFF}, {0xFF, 0xFF, 0x55}, {0xFF, 0xFF, 0xFF}};

/* Palette entry i: the colour as given, and as the display has it. */
static void set_colour(tl_game_t *G, int i, int r, int g, int b)
{
    G->rgb[i][0] = (uint8_t)r;
    G->rgb[i][1] = (uint8_t)g;
    G->rgb[i][2] = (uint8_t)b;
    G->g.palette[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static int setup_state(tl_env_t *e, int size_log2)
{
    tl_game_reset(e);
    tl_game_t *G = mem(e, sizeof *G, 0);
    if (!G)
        return 0;
    const size_t bytes = (size_t)TL_TILES << (2 * size_log2);
    G->tiles = mem(e, bytes, 1);
    G->sprites = mem(e, bytes, 1);
    e->game = G;
    if (!G->tiles || !G->sprites) {
        tl_game_reset(e);
        return 0;
    }
    G->size_log2 = size_log2;
    tg_init(&G->g);
    G->g.mode = TG_MODE_TILES;
    G->g.sprite_tile_log2 = (uint8_t)size_log2;
    G->g.sprite_depth = 8;
    G->g.sprite_tiles = G->sprites;
    for (int i = 0; i < 16; i++)
        set_colour(G, (unsigned char)"0123456789ABCDEF"[i], ega[i][0], ega[i][1], ega[i][2]);
    set_colour(G, 0, 0, 0, 0);   /* (the backdrop) */
    G->bg = '.';
    G->frame_ref = LUA_NOREF;
    G->follow_id = -1;
    G->follow_limits = 1;
    for (int i = 0; i < TG_MAX_LAYERS; i++)
        G->lay[i].fx = G->lay[i].fy = 1.0f;
    tg_actors_init(&G->actors, G->pool, TL_MAX_ACTORS, G->order);
    G->actors.sprite_log2 = (uint8_t)size_log2;   /* (collision boxes before the first draw) */
    for (int i = 0; i < TL_MAX_ANIMS; i++)
        G->anims[i].frames = G->anim_frames[i];
    tg_actors_set_anims(&G->actors, G->anims, TL_MAX_ANIMS);
    return 1;
}

/* (the board writes down what the program is doing with its video log: 7 is making the graphics state, 5 running) */
static int setup(tl_env_t *e, int size_log2)
{
    if (e->hooks.activity)
        e->hooks.activity(7);
    const int r = setup_state(e, size_log2);
    if (e->hooks.activity)
        e->hooks.activity(5);
    return r;
}

/* The environment the functions belong to, kept by tl_game_open (there is one at a time): looking it up on every call cost as much as a check. */
static tl_env_t *E;

static tl_game_t *make_game(lua_State *L)
{
    if (!setup(E, 3))
        luaL_error(L, "not enough memory for the graphics");
    return E->game;
}

static inline tl_game_t *need_game(lua_State *L)
{
    tl_game_t *G = E->game;
    return G ? G : make_game(L);
}

/* The map of the layer the map functions work on (layer(n)): an error if it has none. */
static tg_world_t *cur_world(lua_State *L, tl_game_t *G)
{
    if (!G->lay[G->cur].has_world)
        luaL_error(L, "there is no map on layer %d: newmap(width, height) makes one", G->cur + 1);
    return &G->lay[G->cur].world;
}

/* The solid layer's map (actors collide with it, follow keeps the camera inside it), or NULL. */
static tg_world_t *solid_world(tl_game_t *G)
{
    return G->lay[G->solid].has_world ? &G->lay[G->solid].world : NULL;
}

/* Where the main camera is as the program sees it: the solid layer's camera (kept within one map if the map repeats), or the main camera itself. */
static int32_t view_x(tl_game_t *G)
{
    const tg_world_t *w = solid_world(G);
    return w ? w->cam_x : G->cam_x;
}

static int32_t view_y(tl_game_t *G)
{
    const tg_world_t *w = solid_world(G);
    return w ? w->cam_y : G->cam_y;
}

/* Every layer's camera from the main one: times its parallax factor, plus its offset. */
static int32_t layer_cam(int32_t cam, float f, int32_t off)
{
    return (f == 1.0f ? cam : (int32_t)floorf((float)cam * f)) + off;
}

static void set_cameras(tl_game_t *G)
{
    for (int i = 0; i < TG_MAX_LAYERS; i++) {
        tl_layer_t *l = &G->lay[i];
        if (l->has_world)
            tg_world_set_camera(&l->world, layer_cam(G->cam_x, l->fx, l->ox), layer_cam(G->cam_y, l->fy, l->oy));
    }
}

static inline int iarg(lua_State *L, int i);

/* A layer number 1 to TG_MAX_LAYERS: the index from 0. */
static int layer_arg(lua_State *L, int i, const char *who)
{
    const int n = iarg(L, i);
    if (n < 1 || n > TG_MAX_LAYERS)
        luaL_error(L, "%s: the layer is 1 to %d", who, TG_MAX_LAYERS);
    return n - 1;
}

/* The actors wrap with the solid layer, if its map repeats (and is big enough). */
static int wrap_actors(tl_game_t *G)
{
    const tg_world_t *w = solid_world(G);
    int32_t pw = 0, ph = 0;
    const int on = w && w->border == TG_BORDER_WRAP && tg_world_wrap_px(w, &pw, &ph) && tg_actors_set_wrap(&G->actors, pw, ph);
    if (!on)
        tg_actors_set_wrap(&G->actors, 0, 0);
    return on;
}

/* ---- arguments ---- */

/* A whole number: any number, rounded down (a pixel position divided by 8 is a tile position), kept in range. An integer (or a float with nothing after the
 * point) is taken as it is, which is most of the time: converting each argument to a float and back was a quarter of the cost of a call. */
static int iarg_float(lua_State *L, int i)
{
    const lua_Number v = luaL_checknumber(L, i);
    if (v >= 2147483647.0f)
        return INT32_MAX;
    if (v <= -2147483648.0f)
        return INT32_MIN;
    return (int)floorf((float)v);
}

static inline int iarg(lua_State *L, int i)
{
    int isnum;
    const lua_Integer v = lua_tointegerx(L, i, &isnum);
    return isnum ? (int)v : iarg_float(L, i);
}

static int iopt(lua_State *L, int i, int def)
{
    return lua_isnoneornil(L, i) ? def : iarg(L, i);
}

/* On or off: true or false, or a number (0 is off). */
static int flag_arg(lua_State *L, int i)
{
    if (lua_type(L, i) == LUA_TNUMBER)
        return lua_tonumber(L, i) != 0;
    luaL_checkany(L, i);
    return lua_toboolean(L, i);
}

/* Pixels (or a fraction) to the actors' 24.8 fixed point, rounded and kept in range. */
static int32_t fixed(lua_Number v)
{
    v *= 256;
    if (v >= 2147483647.0f)
        return INT32_MAX;
    if (v <= -2147483648.0f)
        return INT32_MIN;
    return (int32_t)floorf((float)v + 0.5f);
}

/* The argument as pixels in 24.8: a whole number of pixels is shifted, anything else goes through the float. */
static inline int32_t fixed_arg(lua_State *L, int i)
{
    int isnum;
    const lua_Integer v = lua_tointegerx(L, i, &isnum);
    if (isnum && v >= -8388608 && v <= 8388607)
        return (int32_t)v * 256;
    return fixed(luaL_checknumber(L, i));
}

/* A position or a velocity (24.8) for Lua: a whole number of pixels is an integer (so it prints as 118, not 118.0), a fraction a float. */
static void push_px(lua_State *L, int32_t v)
{
    if ((v & 255) == 0)
        lua_pushinteger(L, v >> 8);
    else
        lua_pushnumber(L, (lua_Number)v / 256);
}

/* Is the name, in any case, the lower case word? */
static int same_name(const char *name, const char *word)
{
    while (*word && *name && tolower((unsigned char)*name) == *word) {
        word++;
        name++;
    }
    return !*word && !*name;
}

/* ---- graphics ---- */

static int l_gfx(lua_State *L)
{
    const int size = iarg(L, 1);
    if (size != 8 && size != 16)
        return luaL_error(L, "gfx(size): the size of a tile is 8 or 16");
    if (!setup(E, size == 8 ? 3 : 4))
        return luaL_error(L, "not enough memory for the graphics");
    return 0;
}

/*
 * palette(c, red, green, blue): a colour is a palette entry, and the entry of a character is its code ("A" is 65), the same character a graphics string
 * uses for it; a number is the entry itself (0 is the backdrop, which no character can name). A negative red, green or blue with a character makes
 * that character the one that means see-through in graphics strings (it is "." at first). palette(c): the red, green and blue of the colour now.
 */
static int l_palette(lua_State *L)
{
    tl_game_t *G = need_game(L);
    int i, is_char = 0;
    if (lua_type(L, 1) == LUA_TSTRING) {
        size_t len;
        const char *s = lua_tolstring(L, 1, &len);
        if (len != 1)
            return luaL_error(L, "palette(colour, red, green, blue): the colour is one character, or a number 0 to 255");
        i = (unsigned char)s[0];
        is_char = 1;
    } else {
        i = iarg(L, 1);
    }
    if (lua_isnone(L, 2)) {
        if (i < 0 || i > 255)
            return luaL_error(L, "palette(colour): the colour is one character, or a number 0 to 255");
        for (int k = 0; k < 3; k++)
            lua_pushinteger(L, G->rgb[i][k]);
        return 3;
    }
    const int r = iarg(L, 2), g = iarg(L, 3), b = iarg(L, 4);
    if (r < 0 || g < 0 || b < 0) {
        if (!is_char)
            return luaL_error(L, "palette(colour, -1, -1, -1) needs a character, such as \"@\", for the colour");
        G->bg = (char)i;
        return 0;
    }
    if (i < 0 || i > 255 || r > 255 || g > 255 || b > 255)
        return luaL_error(L, "palette(colour, red, green, blue): the colour is a character or 0 to 255, red, green and blue 0 to 255");
    set_colour(G, i, r, g, b);
    return 0;
}

/* A character of a graphics string: the background character is 0 (see-through), any other is its own code, the palette entry palette gave it. */
static uint8_t pixel_of(const tl_game_t *G, char c)
{
    return c == G->bg ? 0 : (uint8_t)c;
}

/* The other way: a pixel as a graphics string has it, 0 (see-through) the see-through character, any other its own code. (So a pixel of the see-through
 * character's own entry comes back as that character, which a string reads as see-through: such a colour needs tilepix, or another entry.) */
static char char_of(const tl_game_t *G, uint8_t p)
{
    return p ? (char)p : G->bg;
}

/* n pixels (256 at most: a 16 x 16 tile) as a string, on the stack */
static int push_pixels(lua_State *L, const tl_game_t *G, const uint8_t *src, size_t n)
{
    char out[256];
    for (size_t i = 0; i < n; i++)
        out[i] = char_of(G, src[i]);
    lua_pushlstring(L, out, n);
    return 1;
}

/* tiledef(n, graphics): tile n from a string of every pixel, row by row; tiledef(n): the string of the tile now (sprdef the same for sprites). */
static int define_tile(lua_State *L, int sprites)
{
    tl_game_t *G = need_game(L);
    const int n = iarg(L, 1);
    const int get = lua_isnone(L, 2);
    size_t len = 0;
    const char *text = get ? NULL : luaL_checklstring(L, 2, &len);
    if (n < 0 || n >= TL_TILES)
        return luaL_error(L, "%s(n%s): n is 0 to %d", sprites ? "sprdef" : "tiledef", get ? "" : ", graphics", TL_TILES - 1);
    const size_t px = (size_t)1 << (2 * G->size_log2);
    uint8_t *dst = (sprites ? G->sprites : G->tiles) + (size_t)n * px;
    if (get)
        return push_pixels(L, G, dst, px);
    for (size_t i = 0; i < px; i++)
        dst[i] = i < len ? pixel_of(G, text[i]) : 0;
    return 0;
}

static int l_tiledef(lua_State *L) { return define_tile(L, 0); }
static int l_sprdef(lua_State *L) { return define_tile(L, 1); }

/* tilerow(n, y, graphics): one row of a tile (its size in characters). With y = 0 and a string long enough for every pixel of the tile it is the whole tile, row by row.
 * tilerow(n, y): the string of the row now. */
static int define_row(lua_State *L, int sprites)
{
    tl_game_t *G = need_game(L);
    const int n = iarg(L, 1), y = iarg(L, 2);
    const int get = lua_isnone(L, 3);
    size_t len = 0;
    const char *text = get ? NULL : luaL_checklstring(L, 3, &len);
    const int size = 1 << G->size_log2;
    if (n < 0 || n >= TL_TILES || y < 0 || y >= size)
        return luaL_error(L, "%s(n, y%s): n is 0 to %d, y 0 to %d", sprites ? "sprrow" : "tilerow", get ? "" : ", graphics", TL_TILES - 1, size - 1);
    uint8_t *dst = (sprites ? G->sprites : G->tiles) + ((size_t)n << (2 * G->size_log2)) + (size_t)y * (size_t)size;
    if (get)
        return push_pixels(L, G, dst, (size_t)size);
    const size_t count = y == 0 && len >= (size_t)size * (size_t)size ? (size_t)size * (size_t)size : (size_t)size;
    for (size_t i = 0; i < count; i++)
        dst[i] = i < len ? pixel_of(G, text[i]) : 0;
    return 0;
}

static int l_tilerow(lua_State *L) { return define_row(L, 0); }
static int l_sprrow(lua_State *L) { return define_row(L, 1); }

/* tilepix(n, x, y, colour): one pixel of tile n, the colour its palette entry (0 see-through); tilepix(n, x, y): the colour it has now. */
static int set_pixel(lua_State *L, int sprites)
{
    tl_game_t *G = need_game(L);
    const int n = iarg(L, 1), x = iarg(L, 2), y = iarg(L, 3);
    const int get = lua_isnone(L, 4);
    const int c = get ? 0 : iarg(L, 4);
    const int size = 1 << G->size_log2;
    const char *who = sprites ? "sprpix" : "tilepix";
    if (n < 0 || n >= TL_TILES || x < 0 || x >= size || y < 0 || y >= size || c < 0 || c > 255) {
        if (get)
            return luaL_error(L, "%s(n, x, y): n 0 to %d, x and y 0 to %d", who, TL_TILES - 1, size - 1);
        return luaL_error(L, "%s(n, x, y, colour): n 0 to %d, x and y 0 to %d, colour 0 to 255", who, TL_TILES - 1, size - 1);
    }
    uint8_t *p = (sprites ? G->sprites : G->tiles) + ((size_t)n << (2 * G->size_log2)) + (size_t)y * (size_t)size + (size_t)x;
    if (get) {
        lua_pushinteger(L, *p);
        return 1;
    }
    *p = (uint8_t)c;
    return 0;
}

static int l_tilepix(lua_State *L) { return set_pixel(L, 0); }
static int l_sprpix(lua_State *L) { return set_pixel(L, 1); }

/* ---- the map ---- */

/* newmap(width, height [, layer]): a map for the layer (the one layer(n) chose, if not given), all tile 0; its camera follows the main one. Returns the layer. */
/* The memory of a layer's map: the window (what the display reads, a little more than the screen, double buffered so that no border is needed), its
 * bitmap of changes, and the whole map (cleared: all tile 0). */
typedef struct {
    uint16_t *win_a, *win_b;
    uint32_t *dirty;
    uint16_t *map;
} layer_mem_t;

static void window_log2(const tl_game_t *G, unsigned *wl, unsigned *hl)
{
    *wl = G->size_log2 == 3 ? 6 : 5;
    *hl = G->size_log2 == 3 ? 5 : 4;
}

static void layer_mem_free(tl_env_t *e, layer_mem_t *m)
{
    unmem(e, m->win_a);
    unmem(e, m->win_b);
    unmem(e, m->dirty);
    unmem(e, m->map);
    memset(m, 0, sizeof *m);
}

/* 1, or 0 with nothing kept, if there was not memory enough */
static int layer_mem(tl_env_t *e, const tl_game_t *G, int32_t w, int32_t h, layer_mem_t *m)
{
    unsigned wl, hl;
    window_log2(G, &wl, &hl);
    m->win_a = mem(e, TG_WORLD_WIN_ENTRIES(wl, hl) * sizeof *m->win_a, 1);
    m->win_b = mem(e, TG_WORLD_WIN_ENTRIES(wl, hl) * sizeof *m->win_b, 1);
    m->dirty = mem(e, TG_WORLD_DIRTY_WORDS(wl, hl) * sizeof *m->dirty, 0);
    m->map = mem(e, (size_t)w * (size_t)h * sizeof *m->map, 0);
    if (m->win_a && m->win_b && m->dirty && m->map)
        return 1;
    layer_mem_free(e, m);
    return 0;
}

/* Layer i (which free_layer has emptied) gets the memory, and its world is set up on the map in it, with its camera where the main one says. 1, or 0 if
 * the world could not be (the memory is given back). */
static int install_layer(tl_env_t *e, tl_game_t *G, int i, int32_t w, int32_t h, layer_mem_t *m)
{
    tl_layer_t *l = &G->lay[i];
    unsigned wl, hl;
    window_log2(G, &wl, &hl);
    l->win_a = m->win_a;
    l->win_b = m->win_b;
    l->dirty = m->dirty;
    l->map = m->map;
    memset(m, 0, sizeof *m);
    G->g.layers[i] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = (uint8_t)G->size_log2, .depth = 8, .map_w_log2 = (uint8_t)wl,
                                  .map_h_log2 = (uint8_t)hl, .tiles = G->tiles};
    if (!tg_world_init(&l->world, &G->g.layers[i], l->win_a, l->win_b, l->dirty, l->map, w, h)) {
        free_layer(e, G, i);
        return 0;
    }
    tg_world_set_flags(&l->world, G->flags);
    l->has_world = 1;
    tg_world_set_camera(&l->world, layer_cam(G->cam_x, l->fx, l->ox), layer_cam(G->cam_y, l->fy, l->oy));
    return 1;
}

static int l_newmap(lua_State *L)
{
    tl_env_t *e = E;
    tl_game_t *G = need_game(L);
    const int w = iarg(L, 1), h = iarg(L, 2);
    const int i = lua_isnoneornil(L, 3) ? G->cur : layer_arg(L, 3, "newmap(width, height, layer)");
    if (w < 1 || h < 1 || (long)w * h > TL_MAP_MAX)
        return luaL_error(L, "newmap(width, height): the map has at most %d tiles", TL_MAP_MAX);
    free_layer(e, G, i);
    layer_mem_t m;
    if (!layer_mem(e, G, w, h, &m))
        return luaL_error(L, "not enough memory for a map of %d x %d", w, h);
    if (!install_layer(e, G, i, w, h, &m))
        return luaL_error(L, "the map could not be set up");
    lua_pushinteger(L, i + 1);
    return 1;
}

/* wrap(true): the map of the current layer repeats in every direction (infinite scrolling); wrap(false): it does not. For the solid layer, returns true if the actors
 * wrap too (the map must be at least 192 x 160 pixels); for another, true if it repeats. */
static int l_wrap(lua_State *L)
{
    tl_game_t *G = need_game(L);
    tg_world_t *w = cur_world(L, G);
    const int on = flag_arg(L, 1);
    tg_world_set_border(w, on ? TG_BORDER_WRAP : TG_BORDER_TILE, 0);
    const int actors = wrap_actors(G);
    lua_pushboolean(L, G->cur == G->solid ? actors : on);
    return 1;
}

static int l_tile(lua_State *L)
{
    tl_game_t *G = need_game(L);
    tg_world_t *w = cur_world(L, G);
    const int x = iarg(L, 1), y = iarg(L, 2), n = iarg(L, 3), flip = iopt(L, 4, 0);
    if (n < 0 || n >= TL_TILES)
        return luaL_error(L, "tile(x, y, n): n is 0 to %d", TL_TILES - 1);
    tg_world_set(w, x, y, TG_ENTRY(n, 0, ((flip & 1) ? TG_HFLIP : 0) | ((flip & 2) ? TG_VFLIP : 0)));
    return 0;
}

static int l_gettile(lua_State *L)
{
    tl_game_t *G = need_game(L);
    lua_pushinteger(L, tg_world_get(cur_world(L, G), iarg(L, 1), iarg(L, 2)) & TG_TILE_MASK);
    return 1;
}

static int l_tilefill(lua_State *L)
{
    tl_game_t *G = need_game(L);
    tg_world_t *world = cur_world(L, G);
    const int x = iarg(L, 1), y = iarg(L, 2), w = iarg(L, 3), h = iarg(L, 4), n = iarg(L, 5);
    if (n < 0 || n >= TL_TILES)
        return luaL_error(L, "tilefill(x, y, width, height, n): n is 0 to %d", TL_TILES - 1);
    tg_world_fill(world, x, y, w, h, TG_ENTRY(n, 0, 0));
    return 0;
}

/* tilestr(x, y, width, s [, offset]): the bytes of the string are tile numbers (the byte plus the offset, 0 if it is not given) put in the map one after the
 * other, `width` to a row from x, y, then the next row. */
static int l_tilestr(lua_State *L)
{
    tl_game_t *G = need_game(L);
    tg_world_t *world = cur_world(L, G);
    const int x = iarg(L, 1), y = iarg(L, 2), w = iarg(L, 3);
    size_t len;
    const char *text = luaL_checklstring(L, 4, &len);
    const int offset = iopt(L, 5, 0);
    if (w < 1)
        return luaL_error(L, "tilestr(x, y, width, string): the width is 1 or more");
    for (size_t i = 0; i < len; i++) {
        const int n = (unsigned char)text[i] + offset;
        if (n < 0 || n >= TL_TILES)
            return luaL_error(L, "tilestr: character %d is tile %d, tiles are 0 to %d", (int)i + 1, n, TL_TILES - 1);
    }
    for (size_t i = 0; i < len; i++)
        tg_world_set(world, x + (int)(i % (size_t)w), y + (int)(i / (size_t)w), TG_ENTRY((unsigned char)text[i] + offset, 0, 0));
    return 0;
}

/* settileflags(n, flags): what tile n is (1 solid, 2 one-way, 4 trigger, the rest the program's); settileflags(n): its flags now. */
static int l_settileflags(lua_State *L)
{
    tl_game_t *G = need_game(L);
    const int n = iarg(L, 1);
    if (lua_isnone(L, 2)) {
        if (n < 0 || n >= TL_TILES)
            return luaL_error(L, "settileflags(n): n is 0 to %d", TL_TILES - 1);
        lua_pushinteger(L, G->flags[n]);
        return 1;
    }
    const int f = iarg(L, 2);
    if (n < 0 || n >= TL_TILES || f < 0 || f > 255)
        return luaL_error(L, "settileflags(n, flags): n is 0 to %d, flags 0 to 255 (1 solid, 2 one-way, 4 trigger)", TL_TILES - 1);
    G->flags[n] = (uint8_t)f;
    return 0;
}

static int l_tileflag(lua_State *L)
{
    tl_game_t *G = need_game(L);
    lua_pushinteger(L, tg_world_flags_px(cur_world(L, G), iarg(L, 1), iarg(L, 2)));
    return 1;
}

static int l_tilebox(lua_State *L)
{
    tl_game_t *G = need_game(L);
    lua_pushinteger(L, tg_world_flags_box(cur_world(L, G), iarg(L, 1), iarg(L, 2), iarg(L, 3), iarg(L, 4)));
    return 1;
}

/* camera(x, y): the main camera; every layer's camera follows it (parallax) */
static int l_camera(lua_State *L)
{
    tl_game_t *G = need_game(L);
    G->cam_x = iarg(L, 1);
    G->cam_y = iarg(L, 2);
    set_cameras(G);
    return 0;
}

static int l_camx(lua_State *L)
{
    lua_pushinteger(L, view_x(need_game(L)));
    return 1;
}

static int l_camy(lua_State *L)
{
    lua_pushinteger(L, view_y(need_game(L)));
    return 1;
}

/* layer(n): the map functions (tile, gettile, tilefill, tilestr, tileflag, tilebox, wrap, newmap without a layer) work on layer n from now on (1 at first).
 * Returns the layer that was chosen before; layer() returns the one chosen now. */
static int l_layer(lua_State *L)
{
    tl_game_t *G = need_game(L);
    const int was = G->cur;
    if (!lua_isnoneornil(L, 1))
        G->cur = layer_arg(L, 1, "layer(n)");
    lua_pushinteger(L, was + 1);
    return 1;
}

/* solidlayer(n): the layer whose tiles stop the actors (actormove) and whose map follow keeps the camera inside (1 at first). Returns the one before;
 * solidlayer() the one now. */
static int l_solidlayer(lua_State *L)
{
    tl_game_t *G = need_game(L);
    const int was = G->solid;
    if (!lua_isnoneornil(L, 1)) {
        G->solid = layer_arg(L, 1, "solidlayer(n)");
        wrap_actors(G);
    }
    lua_pushinteger(L, was + 1);
    return 1;
}

/* parallax(n, fx [, fy [, ox, oy]]): layer n's camera is the main camera times fx across and fy down (fy is fx if not given), plus ox, oy pixels: 1 moves with the
 * main camera (as at first), 0.5 at half its speed (far away), 0 not at all (a panel that stays on the screen). */
static int l_parallax(lua_State *L)
{
    tl_game_t *G = need_game(L);
    tl_layer_t *l = &G->lay[layer_arg(L, 1, "parallax(n, fx, fy, ox, oy)")];
    l->fx = (float)luaL_checknumber(L, 2);
    l->fy = lua_isnoneornil(L, 3) ? l->fx : (float)luaL_checknumber(L, 3);
    l->ox = iopt(L, 4, 0);
    l->oy = iopt(L, 5, 0);
    set_cameras(G);
    return 0;
}

/* ---- actors ---- */

static tg_actor_t *no_actor(lua_State *L, const char *who, int id)
{
    luaL_error(L, "%s: there is no actor %d", who, id);
    return NULL;
}

/* (as tg_actor, without the call) */
static inline tg_actor_t *actor_arg(lua_State *L, tl_game_t *G, int i, const char *who)
{
    const int id = iarg(L, i);
    if ((unsigned)id < TL_MAX_ACTORS && (G->pool[id].flags & TG_ACTOR_ACTIVE))
        return &G->pool[id];
    return no_actor(L, who, id);
}

/*
 * follow(actor [, dead_width, dead_height [, smooth]]): from now on the camera follows the actor, once a frame (waitframe or run), keeping the middle of the actor in the middle
 * of the screen. With a dead zone the camera stays still while the actor moves inside a box of that size in the middle of the screen, and moves only as far as it must when the
 * actor goes outside it. With smooth n the camera covers 1/(n+1) of the way to where it should be each frame. A map that does not repeat keeps the camera inside it (followlimit(false)
 * does not), a map that repeats goes the short way round the seam. follow(nil) or follow(-1) stops it, and so does freeing the actor; camera(x, y) is for moving it yourself.
 */
static int l_follow(lua_State *L)
{
    tl_game_t *G = need_game(L);
    if (lua_isnoneornil(L, 1) || iarg(L, 1) < 0) {
        G->follow_id = -1;
        return 0;
    }
    const int id = iarg(L, 1), dw = iopt(L, 2, 0), dh = iopt(L, 3, 0), smooth = iopt(L, 4, 0);
    if (!tg_actor(&G->actors, id))
        return luaL_error(L, "follow(actor): there is no actor %d", id);
    if (dw < 0 || dh < 0 || dw > TG_WIDTH || dh > TG_HEIGHT || smooth < 0 || smooth > 255)
        return luaL_error(L, "follow(actor, width, height, smooth): width 0-%d, height 0-%d, smooth 0-255", TG_WIDTH, TG_HEIGHT);
    G->follow_id = id;
    G->follow_dw = dw;
    G->follow_dh = dh;
    G->follow_smooth = smooth;
    return 0;
}

/* followlimit(on): whether follow keeps the camera inside a map that does not repeat (it does at first). */
static int l_followlimit(lua_State *L)
{
    tl_game_t *G = need_game(L);
    G->follow_limits = flag_arg(L, 1);
    return 0;
}

/* actor(sprite, x, y): a new actor; its number, or nil when all of them are in use. */
static int l_actor(lua_State *L)
{
    tl_game_t *G = need_game(L);
    const int tile = iarg(L, 1);
    const lua_Number x = luaL_checknumber(L, 2), y = luaL_checknumber(L, 3);
    if (tile < 0 || tile >= TL_TILES)
        return luaL_error(L, "actor(sprite, x, y): the sprite is 0 to %d", TL_TILES - 1);
    const int id = tg_actor_alloc(&G->actors);
    if (id < 0) {
        lua_pushnil(L);
        return 1;
    }
    tg_actor_t *a = tg_actor(&G->actors, id);
    a->tile = (uint16_t)tile;
    a->above = TG_MAX_LAYERS;   /* in front of the map (0 would be behind it) */
    a->x = fixed(x);
    a->y = fixed(y);
    lua_pushinteger(L, id);
    return 1;
}

static int l_actorfree(lua_State *L)
{
    tl_game_t *G = need_game(L);
    actor_arg(L, G, 1, "actorfree");
    tg_actor_free(&G->actors, iarg(L, 1));
    return 0;
}

/* actorpos(a, x, y) puts it there; actorpos(a) is where it is: x, y. */
static int l_actorpos(lua_State *L)
{
    tl_game_t *G = need_game(L);
    tg_actor_t *a = actor_arg(L, G, 1, "actorpos");
    if (lua_isnoneornil(L, 2)) {
        push_px(L, a->x);
        push_px(L, a->y);
        return 2;
    }
    tg_actor_set_pos(a, fixed_arg(L, 2), fixed_arg(L, 3));
    return 0;
}

static int l_actorx(lua_State *L)
{
    tl_game_t *G = need_game(L);
    push_px(L, actor_arg(L, G, 1, "actorx")->x);
    return 1;
}

static int l_actory(lua_State *L)
{
    tl_game_t *G = need_game(L);
    push_px(L, actor_arg(L, G, 1, "actory")->y);
    return 1;
}

/* actorvel(a, vx, vy): pixels a frame, added by waitframe; actorvel(a) is the velocity: vx, vy. */
static int l_actorvel(lua_State *L)
{
    tl_game_t *G = need_game(L);
    tg_actor_t *a = actor_arg(L, G, 1, "actorvel");
    if (lua_isnoneornil(L, 2)) {
        push_px(L, a->vx);
        push_px(L, a->vy);
        return 2;
    }
    a->vx = fixed_arg(L, 2);
    a->vy = fixed_arg(L, 3);
    return 0;
}

static int l_actortile(lua_State *L)
{
    tl_game_t *G = need_game(L);
    tg_actor_t *a = actor_arg(L, G, 1, "actortile");
    if (lua_isnoneornil(L, 2)) {
        lua_pushinteger(L, a->tile);
        return 1;
    }
    const int tile = iarg(L, 2);
    if (tile < 0 || tile >= TL_TILES)
        return luaL_error(L, "actortile(actor, sprite): the sprite is 0 to %d", TL_TILES - 1);
    a->tile = (uint16_t)tile;
    return 0;
}

static int l_actorsize(lua_State *L)
{
    tl_game_t *G = need_game(L);
    tg_actor_t *a = actor_arg(L, G, 1, "actorsize");
    const int cols = iarg(L, 2), rows = iarg(L, 3);
    if (cols < 1 || cols > 4 || rows < 1 || rows > 4)
        return luaL_error(L, "actorsize(actor, columns, rows): 1 to 4 of each (sprites side by side, row by row)");
    a->cols = (uint8_t)cols;
    a->rows = (uint8_t)rows;
    return 0;
}

static int l_actorflip(lua_State *L)
{
    tl_game_t *G = need_game(L);
    tg_actor_t *a = actor_arg(L, G, 1, "actorflip");
    const int f = iarg(L, 2);
    a->flags = (uint8_t)((a->flags & ~(TG_ACTOR_HFLIP | TG_ACTOR_VFLIP)) | ((f & 1) ? TG_ACTOR_HFLIP : 0) | ((f & 2) ? TG_ACTOR_VFLIP : 0));
    return 0;
}

static int l_actorhide(lua_State *L)
{
    tl_game_t *G = need_game(L);
    tg_actor_t *a = actor_arg(L, G, 1, "actorhide");
    a->flags = (uint8_t)(flag_arg(L, 2) ? (a->flags | TG_ACTOR_HIDDEN) : (a->flags & ~TG_ACTOR_HIDDEN));
    return 0;
}

static int l_actorprio(lua_State *L)
{
    tl_game_t *G = need_game(L);
    tg_actor_t *a = actor_arg(L, G, 1, "actorprio");
    const int p = iarg(L, 2);
    if (p < 0 || p > 255)
        return luaL_error(L, "actorprio(actor, priority): 0 (in front) to 255");
    a->prio = (uint8_t)p;
    return 0;
}

/* actorbox(actor, x, y, width, height): the part of the actor that collides, in pixels from its top left; a width or height of 0 is all of it. */
static int l_actorbox(lua_State *L)
{
    tl_game_t *G = need_game(L);
    tg_actor_t *a = actor_arg(L, G, 1, "actorbox");
    const int x = iarg(L, 2), y = iarg(L, 3), w = iarg(L, 4), h = iarg(L, 5);
    if (x < 0 || x > 255 || y < 0 || y > 255 || w < 0 || w > 255 || h < 0 || h > 255)
        return luaL_error(L, "actorbox(actor, x, y, width, height): each 0 to 255");
    a->hit_x = (uint8_t)x;
    a->hit_y = (uint8_t)y;
    a->hit_w = (uint8_t)w;
    a->hit_h = (uint8_t)h;
    return 0;
}

/* actoruser(actor) reads the number the program keeps in an actor (0 to 65535); actoruser(actor, n) sets it. */
static int l_actoruser(lua_State *L)
{
    tl_game_t *G = need_game(L);
    tg_actor_t *a = actor_arg(L, G, 1, "actoruser");
    if (!lua_isnoneornil(L, 2)) {
        const int n = iarg(L, 2);
        if (n < 0 || n > 65535)
            return luaL_error(L, "actoruser(actor, n): n is 0 to 65535");
        a->user = (uint16_t)n;
    }
    lua_pushinteger(L, a->user);
    return 1;
}

/*
 * hitx(a, side), hity(a, side), hittile(a, side), hitflags(a, side): the tile that last stopped the actor on that side (1 or "left", 2 "right", 4 "up",
 * 8 "down", as actormove returns them; 16 or "touch" is the trigger tile that it last touched), as it was then: where (tile coordinates), its tile number and
 * its flags. tile(x, y, n) may change it later, this does not. They are 0 until the first one of the kind, and kept until the next, so the return of actormove
 * says whether this move had one.
 */
static int touch_field(lua_State *L, int field, const char *who)
{
    tl_game_t *G = need_game(L);
    int side = -1;
    if (lua_type(L, 2) == LUA_TNUMBER) {
        const int n = iarg(L, 2);
        side = n == 1 ? 0 : n == 2 ? 1 : n == 4 ? 2 : n == 8 ? 3 : n == 16 ? 4 : -1;
    } else if (lua_type(L, 2) == LUA_TSTRING) {
        static const char *const names[5] = {"left", "right", "up", "down", "touch"};
        for (int i = 0; i < 5; i++)
            if (same_name(lua_tostring(L, 2), names[i]))
                side = i;
    }
    if (side < 0)
        return luaL_error(L, "%s(actor, side): the side is 1 \"left\", 2 \"right\", 4 \"up\", 8 \"down\" or 16 \"touch\"", who);
    const tg_touch_t *t = &actor_arg(L, G, 1, who)->touch[side];
    lua_pushinteger(L, field == 0 ? t->tx : field == 1 ? t->ty : field == 2 ? t->tile : t->flags);
    return 1;
}

static int l_hitx(lua_State *L) { return touch_field(L, 0, "hitx"); }
static int l_hity(lua_State *L) { return touch_field(L, 1, "hity"); }
static int l_hittile(lua_State *L) { return touch_field(L, 2, "hittile"); }
static int l_hitflags(lua_State *L) { return touch_field(L, 3, "hitflags"); }

/* lastx(a), lasty(a): where the actor was before its position last changed (by actormove, actorpos or the velocity at waitframe), in pixels. */
static int l_lastx(lua_State *L)
{
    tl_game_t *G = need_game(L);
    push_px(L, actor_arg(L, G, 1, "lastx")->last_x);
    return 1;
}

static int l_lasty(lua_State *L)
{
    tl_game_t *G = need_game(L);
    push_px(L, actor_arg(L, G, 1, "lasty")->last_y);
    return 1;
}

static int l_ysort(lua_State *L)
{
    tl_game_t *G = need_game(L);
    G->actors.flags = (uint8_t)(flag_arg(L, 1) ? TG_ACTORS_YSORT : 0);
    return 0;
}

/* actormove(actor, dx, dy): moves by dx and dy pixels, stopping at solid tiles; returns the sides that stopped it (1 left, 2 right, 4 up, 8 down) and 16 for a trigger touched. */
static int l_actormove(lua_State *L)
{
    tl_game_t *G = need_game(L);
    const tg_world_t *w = solid_world(G);
    if (!w)
        return luaL_error(L, "actormove: there is no map on layer %d, the solid layer: newmap(width, height) makes one", G->solid + 1);
    actor_arg(L, G, 1, "actormove");
    lua_pushinteger(L, tg_actor_move(&G->actors, iarg(L, 1), w, fixed_arg(L, 2), fixed_arg(L, 3)));
    return 1;
}

/* actorlayer(a, n): the actor is drawn in front of layers 1 to n and behind the others (0: behind them all); it is in front of all of them at first.
 * actorlayer(a) is n. */
static int l_actorlayer(lua_State *L)
{
    tl_game_t *G = need_game(L);
    tg_actor_t *a = actor_arg(L, G, 1, "actorlayer");
    if (!lua_isnoneornil(L, 2)) {
        const int n = iarg(L, 2);
        if (n < 0 || n > TG_MAX_LAYERS)
            return luaL_error(L, "actorlayer(actor, n): n is 0 (behind every layer) to %d (in front of all)", TG_MAX_LAYERS);
        a->above = (uint8_t)n;
    }
    lua_pushinteger(L, a->above);
    return 1;
}

static int l_actorhit(lua_State *L)
{
    tl_game_t *G = need_game(L);
    lua_pushboolean(L, tg_actors_overlap(&G->actors, iarg(L, 1), iarg(L, 2)));
    return 1;
}

/* ---- animation tables ---- */

/* A whole number of at most `max` at *p (after spaces and commas); returns -1 if there is none, -2 if it is too big. */
static int read_number(const char **p, int max)
{
    while (**p == ' ' || **p == ',' || **p == '\t')
        (*p)++;
    if (**p < '0' || **p > '9')
        return -1;
    long v = 0;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (**p - '0');
        if (v > max)
            v = max + 1;
        (*p)++;
    }
    return v > max ? -2 : (int)v;
}

#define ANIMDEF "animdef(n, mode, ticks, frames)"

static const char *const anim_modes[] = {"loop", "once", "pingpong"};   /* TG_ANIM_LOOP, TG_ANIM_ONCE, TG_ANIM_PINGPONG */

/* The frames as a string, "3 4:6 5" (a sprite, and after a colon the frames it is shown, if not `ticks`). The count, or an error. */
static int frames_from_string(lua_State *L, const char *p, int ticks, tg_anim_frame_t *out)
{
    int count = 0;
    for (;;) {
        const int sprite = read_number(&p, TL_TILES - 1);
        if (sprite == -1 && !*(p += strspn(p, " ,\t")))
            return count;
        if (sprite < 0)
            return luaL_error(L, ANIMDEF ": frames is sprite numbers 0 to %d, each with :frames if it is not %d long, like \"3 4:6 5\"", TL_TILES - 1, ticks);
        int t = ticks;
        if (*p == ':') {
            p++;
            t = read_number(&p, 255);
            if (t < 1)
                return luaL_error(L, ANIMDEF ": after a colon, how many frames the sprite is shown, 1 to 255");
        }
        if (count >= TL_ANIM_FRAMES)
            return luaL_error(L, ANIMDEF ": at most %d frames", TL_ANIM_FRAMES);
        out[count].tile = (uint16_t)sprite;
        out[count].ticks = (uint8_t)t;
        count++;
    }
}

/* The frames as a table: {3, 4, 5}, or with times of their own, {3, {4, 6}, 5}. */
static int frames_from_table(lua_State *L, int idx, int ticks, tg_anim_frame_t *out)
{
    const lua_Integer n = luaL_len(L, idx);
    if (n > TL_ANIM_FRAMES)
        return luaL_error(L, ANIMDEF ": at most %d frames", TL_ANIM_FRAMES);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_geti(L, idx, i);
        int sprite, t = ticks;
        if (lua_istable(L, -1)) {
            lua_geti(L, -1, 1);
            lua_geti(L, -2, 2);
            sprite = lua_isinteger(L, -2) ? (int)lua_tointeger(L, -2) : -1;
            t = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : -1;
            lua_pop(L, 2);
        } else {
            sprite = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : -1;
        }
        lua_pop(L, 1);
        if (sprite < 0 || sprite >= TL_TILES || t < 1 || t > 255)
            return luaL_error(L, ANIMDEF ": frame %d: a sprite 0 to %d, or {sprite, frames} with frames 1 to 255", (int)i, TL_TILES - 1);
        out[i - 1].tile = (uint16_t)sprite;
        out[i - 1].ticks = (uint8_t)t;
    }
    return (int)n;
}

/*
 * animdef(n, mode, ticks, frames): table n (1 to 32) is the list of sprites in `frames`, shown one after the other for `ticks` frames each: a string, "3 4 5 4" or
 * "3:6 4:2" (a sprite, and after a colon the frames it is shown), or a Lua table, {3, 4, 5, 4} or {{3, 6}, {4, 2}}; 32 at most. The mode is "loop" (0),
 * "once" (1) or "pingpong" (2). Returns the count of frames. Making a table again replaces it, also for actors that play it.
 * animdef(n): the table now, as the mode's name, the first frame's ticks (1 if it has none) and a Lua table of {sprite, ticks} for every frame, which animdef
 * takes back as it is: animdef(m, animdef(n)) copies table n.
 */
static int l_animdef(lua_State *L)
{
    tl_game_t *G = need_game(L);
    const int n = iarg(L, 1);
    if (lua_isnone(L, 2)) {
        if (n < 1 || n > TL_MAX_ANIMS)
            return luaL_error(L, "animdef(n): n is 1 to %d", TL_MAX_ANIMS);
        const tg_anim_t *a = &G->anims[n - 1];
        const tg_anim_frame_t *f = G->anim_frames[n - 1];
        lua_pushstring(L, anim_modes[a->mode <= 2 ? a->mode : 0]);
        lua_pushinteger(L, a->count ? f[0].ticks : 1);
        lua_createtable(L, a->count, 0);
        for (int i = 0; i < a->count; i++) {
            lua_createtable(L, 2, 0);
            lua_pushinteger(L, f[i].tile);
            lua_rawseti(L, -2, 1);
            lua_pushinteger(L, f[i].ticks);
            lua_rawseti(L, -2, 2);
            lua_rawseti(L, -2, i + 1);
        }
        return 3;
    }
    int mode = -1;
    if (lua_type(L, 2) == LUA_TSTRING) {
        for (int i = 0; i < 3; i++)
            if (same_name(lua_tostring(L, 2), anim_modes[i]))
                mode = i;
    } else if (lua_type(L, 2) == LUA_TNUMBER) {
        mode = iarg(L, 2);
    }
    const int ticks = iarg(L, 3);
    if (n < 1 || n > TL_MAX_ANIMS)
        return luaL_error(L, ANIMDEF ": n is 1 to %d", TL_MAX_ANIMS);
    if (mode < 0 || mode > 2)
        return luaL_error(L, ANIMDEF ": the mode is \"loop\", \"once\" or \"pingpong\" (or 0, 1, 2)");
    if (ticks < 1 || ticks > 255)
        return luaL_error(L, ANIMDEF ": ticks is 1 to 255");
    tg_anim_frame_t tmp[TL_ANIM_FRAMES];
    int count;
    if (lua_istable(L, 4))
        count = frames_from_table(L, 4, ticks, tmp);
    else
        count = frames_from_string(L, luaL_checkstring(L, 4), ticks, tmp);
    memcpy(G->anim_frames[n - 1], tmp, (size_t)count * sizeof tmp[0]);
    G->anims[n - 1].count = (uint8_t)count;
    G->anims[n - 1].mode = (uint8_t)mode;
    lua_pushinteger(L, count);
    return 1;
}

/* actoranim(a, n [, restart]): the actor plays table n (0: none; the sprite stays as it was). If it is already playing n and has not finished it nothing happens, so a program can say what
 * it should be playing every frame; restart = true starts it over. actoranim(a) is the number it is playing, 0 if none (or if it was an empty table). */
static int l_actoranim(lua_State *L)
{
    tl_game_t *G = need_game(L);
    tg_actor_t *a = actor_arg(L, G, 1, "actoranim");
    if (!lua_isnoneornil(L, 2)) {
        const int n = iarg(L, 2);
        if (n < 0 || n > TL_MAX_ANIMS)
            return luaL_error(L, "actoranim(actor, n): n is 0 to %d", TL_MAX_ANIMS);
        tg_actor_play(&G->actors, iarg(L, 1), n, !lua_isnoneornil(L, 3) && flag_arg(L, 3));
    }
    lua_pushinteger(L, a->anim);
    return 1;
}

/* animdone(a): true once the actor's "once" animation has shown its last frame for its whole time. animframe(a): which frame of its table it is on, from 0 (0 if none plays). */
static int l_animdone(lua_State *L)
{
    tl_game_t *G = need_game(L);
    const tg_actor_t *a = actor_arg(L, G, 1, "animdone");
    lua_pushboolean(L, a->anim && a->anim_done);
    return 1;
}

static int l_animframe(lua_State *L)
{
    tl_game_t *G = need_game(L);
    const tg_actor_t *a = actor_arg(L, G, 1, "animframe");
    lua_pushinteger(L, a->anim ? a->anim_pos : 0);
    return 1;
}

/* ---- the frame, the clock and the buttons ---- */

/* The distance from where the camera looks to where it should, going the short way round when the world repeats (wrap: its size in pixels, 0 if it does not). */
static int32_t short_way(int32_t d, int32_t wrap)
{
    if (wrap > 0) {
        d %= wrap;
        if (d > wrap / 2)
            d -= wrap;
        else if (d < -(wrap / 2))
            d += wrap;
    }
    return d;
}

/* How far the camera moves on one axis: the part of the distance outside the dead zone, a fraction of it if the camera is smoothed (at least a pixel, and never past). */
static int32_t follow_step(int32_t d, int32_t dead, int smooth)
{
    const int32_t half = dead / 2;
    int32_t m = d > half ? d - half : d < -half ? d + half : 0;
    if (m && smooth > 0) {
        const int32_t part = m / (smooth + 1);
        m = part ? part : (m > 0 ? 1 : -1);
    }
    return m;
}

/* The camera goes toward the actor it follows (once a frame, before the picture is made). */
static void apply_follow(tl_game_t *G)
{
    if (G->follow_id < 0)
        return;
    const tg_actor_t *a = tg_actor(&G->actors, G->follow_id);
    if (!a) {   /* it was freed */
        G->follow_id = -1;
        return;
    }
    const int sprite = 1 << G->size_log2;
    const int32_t cx = (a->x >> 8) + (int32_t)a->cols * sprite / 2, cy = (a->y >> 8) + (int32_t)a->rows * sprite / 2;
    const int32_t camx = G->cam_x, camy = G->cam_y;   /* (the main camera, which does not wrap: the layers that scroll more slowly over a map that repeats do not jump) */
    int32_t x = camx + follow_step(short_way(cx - (camx + TG_WIDTH / 2), G->actors.wrap_w), G->follow_dw, G->follow_smooth);
    int32_t y = camy + follow_step(short_way(cy - (camy + TG_HEIGHT / 2), G->actors.wrap_h), G->follow_dh, G->follow_smooth);
    const tg_world_t *w = solid_world(G);
    if (G->follow_limits && w && w->border != TG_BORDER_WRAP) {
        const int32_t mw = w->map_w << w->tile_log2, mh = w->map_h << w->tile_log2;
        x = mw <= TG_WIDTH ? (mw - TG_WIDTH) / 2 : x < 0 ? 0 : x > mw - TG_WIDTH ? mw - TG_WIDTH : x;
        y = mh <= TG_HEIGHT ? (mh - TG_HEIGHT) / 2 : y < 0 ? 0 : y > mh - TG_HEIGHT ? mh - TG_HEIGHT : y;
    }
    G->cam_x = x;
    G->cam_y = y;
    set_cameras(G);
}

/*
 * Shows the picture (the actors get their sprites, the maps are swapped) and waits for the next frame. Returns the frames that went by since the last time
 * (1 to TL_STEP_MAX); the actors move by that many frames of their velocity, and their animations go on by as many.
 */
static int show_and_wait(lua_State *L, tl_env_t *e, tl_game_t *G)
{
    apply_follow(G);
    G->g.mode = TG_MODE_TILES;
    G->g.text = NULL;
    for (int i = 0; i < TG_MAX_LAYERS; i++)
        if (G->lay[i].has_world)
            tg_world_end_frame(&G->lay[i].world);
    tg_actors_draw(&G->actors, &G->g, view_x(G), view_y(G));   /* (the actors are where the solid layer is) */
    e->hooks.present(&G->g);
    e->game_active = 1;
    const uint32_t f = e->hooks.wait_frame();
    int n = G->last_frame ? (int)(f - G->last_frame) : 1;
    if (n < 1)
        n = 1;
    if (n > TL_STEP_MAX)
        n = TL_STEP_MAX;
    G->last_frame = f;
    tg_actors_step(&G->actors, n);
    for (int i = 0; i < TG_MAX_LAYERS; i++)
        if (G->lay[i].has_world)
            tg_world_begin_frame(&G->lay[i].world);
    tl_env_poll(e);
    if (e->stop) {
        G->in_run = 0;
        luaL_error(L, "Break");
    }
    return n;
}

static void need_screen(lua_State *L, tl_env_t *e, const char *who)
{
    if (!e->hooks.wait_frame || !e->hooks.present)
        luaL_error(L, "this machine has no tile screen for %s", who);
}

static int l_waitframe(lua_State *L)
{
    tl_env_t *e = E;
    tl_game_t *G = need_game(L);
    need_screen(L, e, "waitframe");
    if (G->in_run)
        return luaL_error(L, "waitframe cannot be used while run() is looping: run waits for each frame itself");
    lua_pushinteger(L, show_and_wait(L, e, G));
    return 1;
}

/*
 * onframe(routine): the routine run() calls once a frame: a function, or the name of one in a string ("update"); it is given the number of frames that went
 * by (as waitframe returns it), and a result that is not nil or false ends run. onframe(nil) forgets it.
 */
static int l_onframe(lua_State *L)
{
    tl_game_t *G = need_game(L);
    if (G->in_run)
        return luaL_error(L, "onframe cannot be used while run() is looping");
    if (lua_type(L, 1) == LUA_TSTRING) {
        const char *name = lua_tostring(L, 1);
        if (lua_getglobal(L, name) != LUA_TFUNCTION)
            return luaL_error(L, "onframe(routine): there is no function called \"%s\"", name);
        lua_replace(L, 1);
    }
    if (!lua_isnoneornil(L, 1) && !lua_isfunction(L, 1))
        return luaL_error(L, "onframe(routine): a function, or the name of one");
    luaL_unref(L, LUA_REGISTRYINDEX, G->frame_ref);
    G->frame_ref = LUA_NOREF;
    if (lua_isfunction(L, 1)) {
        lua_settop(L, 1);
        G->frame_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

/*
 * run(): the frame loop, with the program's routine in it: for each frame it draws and waits (as waitframe does), moves the actors, and calls the routine onframe gave with
 * the number of frames that went by. It ends when the routine returns something that is not nil or false (run returns it), or at an error or Ctrl+C.
 */
static int l_run(lua_State *L)
{
    tl_env_t *e = E;
    tl_game_t *G = need_game(L);
    need_screen(L, e, "run");
    if (G->frame_ref == LUA_NOREF)
        return luaL_error(L, "run(): onframe(routine) says what to call each frame");
    if (G->in_run)
        return luaL_error(L, "run cannot be used while run is looping");
    G->in_run = 1;
    for (;;) {
        lua_settop(L, 0);
        lua_rawgeti(L, LUA_REGISTRYINDEX, G->frame_ref);
        const int k = show_and_wait(L, e, G);
        lua_pushinteger(L, k);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            G->in_run = 0;
            return lua_error(L);
        }
        if (lua_toboolean(L, -1))
            break;
    }
    G->in_run = 0;
    return 1;
}

/* textmode(): the text screen now; the graphics are kept, and the next waitframe shows the picture again. */
static int l_textmode(lua_State *L)
{
    (void)L;
    tl_env_t *e = E;
    if (e->game_active) {
        e->game_active = 0;
        e->hooks.frame();
    }
    return 0;
}

static int l_frames(lua_State *L)
{
    tl_game_t *G = need_game(L);
    lua_pushinteger(L, (lua_Integer)G->last_frame);
    return 1;
}

static int l_millis(lua_State *L)
{
    tl_env_t *e = E;
    lua_pushinteger(L, (lua_Integer)e->hooks.millis());
    return 1;
}

/* A button's name, in any case: by its length and first letter (a name is looked up on every btn call), then the rest. 0 if it is not one. */
static int button_mask(const char *name, size_t len)
{
    const int c = tolower((unsigned char)name[0]);
    switch (len) {
    case 1:
        return c == 'a' ? PLAT_BTN_A : c == 'b' ? PLAT_BTN_B : c == 'x' ? PLAT_BTN_X : c == 'y' ? PLAT_BTN_Y : c == 'l' ? PLAT_BTN_L : c == 'r' ? PLAT_BTN_R : 0;
    case 2:
        return same_name(name, "up") ? PLAT_BTN_UP : 0;
    case 4:
        return same_name(name, "down") ? PLAT_BTN_DOWN : same_name(name, "left") ? PLAT_BTN_LEFT : 0;
    case 5:
        return same_name(name, "right") ? PLAT_BTN_RIGHT : same_name(name, "start") ? PLAT_BTN_START : 0;
    case 6:
        return same_name(name, "select") ? PLAT_BTN_SELECT : 0;
    default:
        return 0;
    }
}

/* btn(player, button): true while the button is held. The button is a number (1 up, 2 down, 4 left, 8 right, 16 A, 32 B, 64 X, 128 Y, 256 Start, 512 Select,
 * 1024 L, 2048 R; add them for any of several) or its name: "up", "a", "start" ... pressed(player, button): it went down since the last frame. */
static int button_state(lua_State *L, int kind)
{
    tl_env_t *e = E;
    const int player = iarg(L, 1);
    int mask = 0;
    const int type = lua_type(L, 2);
    if (type == LUA_TNUMBER) {
        mask = iarg(L, 2);
    } else if (type == LUA_TSTRING) {
        size_t len;
        const char *name = lua_tolstring(L, 2, &len);
        mask = button_mask(name, len);
    }
    if (!mask)
        return luaL_error(L, "%s(player, button): a number, or \"up\" \"down\" \"left\" \"right\" \"a\" \"b\" \"x\" \"y\" \"start\" \"select\" \"l\" \"r\"",
                          kind ? "pressed" : "btn");
    if (player < 0 || player >= PLAT_MAX_PLAYERS)
        return luaL_error(L, "the player is 0 to %d", PLAT_MAX_PLAYERS - 1);
    lua_pushboolean(L, e->hooks.buttons && (e->hooks.buttons(player, kind) & (unsigned)mask));
    return 1;
}

static int l_btn(lua_State *L) { return button_state(L, 0); }
static int l_pressed(lua_State *L) { return button_state(L, 1); }

/* ---- the keyboard and the mouse ---- */

/*
 * key(): the next key typed since the program started, or nil if there is none; it does not wait. A character gives itself ("a", "A", "7", " ", "?"), the
 * other keys their names: "enter", "backspace", "tab", "escape", "delete", "up", "down", "left", "right", "home", "end", "pageup", "pagedown", "insert",
 * "f1" to "f12", and Ctrl with a letter "ctrl+a" to "ctrl+z" (Ctrl+C stops the program). Keys typed and not read wait for it, 32 at most: `while key() do
 * end` throws them away. The same key presses count for btn and pressed too.
 */
static int l_key(lua_State *L)
{
    static const char *const moves[] = {"up", "down", "left", "right", "home", "end", "pageup", "pagedown", "insert"};
    tl_env_t *e = E;
    for (;;) {
        const int ch = tl_env_key(e);
        if (e->stop)
            return luaL_error(L, "Break");
        if (ch < 0) {
            lua_pushnil(L);
            return 1;
        }
        char name[12];
        const char *s = name;
        if (ch >= 32 && ch < 127) {
            name[0] = (char)ch;
            name[1] = 0;
        } else if (ch == 10 || ch == 13) {
            s = "enter";
        } else if (ch == 8) {
            s = "backspace";
        } else if (ch == 9) {
            s = "tab";
        } else if (ch == 27) {
            s = "escape";
        } else if (ch == 127) {
            s = "delete";
        } else if (ch < 32) {   /* Ctrl with a letter, or with @ \ ] ^ _ */
            snprintf(name, sizeof name, "ctrl+%c", ch >= 1 && ch <= 26 ? ch + 'a' - 1 : ch + '@');
        } else if (ch >= PLAT_KEY_UP && ch <= PLAT_KEY_INSERT) {
            s = moves[ch - PLAT_KEY_UP];
        } else if (ch >= PLAT_KEY_F(1) && ch <= PLAT_KEY_F(12)) {
            snprintf(name, sizeof name, "f%d", ch - PLAT_KEY_F1 + 1);
        } else {
            continue;   /* (nothing else is typed) */
        }
        lua_pushstring(L, s);
        return 1;
    }
}

/*
 * mouse(): x, y, left, right, middle, wheel: where the pointer is on the screen (x 0 to 319, y 0 to 239; the camera is not in it), whether each button is
 * held, and how far the wheel turned since the last frame (away from you is positive). As it was at the last frame, like the buttons. With no mouse the
 * pointer stays in the middle of the screen. mouse(x, y) puts the pointer there (kept on the screen).
 */
static int l_mouse(lua_State *L)
{
    tl_env_t *e = E;
    if (lua_gettop(L) > 0) {
        const int x = iarg(L, 1), y = iarg(L, 2);
        if (e->hooks.set_mouse)
            e->hooks.set_mouse(x, y);
        return 0;
    }
    const plat_mouse_t m = e->hooks.mouse ? e->hooks.mouse() : (plat_mouse_t){.x = TG_WIDTH / 2, .y = TG_HEIGHT / 2};
    lua_pushinteger(L, m.x);
    lua_pushinteger(L, m.y);
    lua_pushboolean(L, m.buttons & PLAT_MOUSE_LEFT);
    lua_pushboolean(L, m.buttons & PLAT_MOUSE_RIGHT);
    lua_pushboolean(L, m.buttons & PLAT_MOUSE_MIDDLE);
    lua_pushinteger(L, m.wheel);
    return 6;
}

/* click([button]): the mouse button went down since the last frame: "left" (with none named), "right", "middle", or 1, 2, 4 (added for any of several). */
static int l_click(lua_State *L)
{
    tl_env_t *e = E;
    int mask = 0;
    const int type = lua_type(L, 1);
    if (type == LUA_TNONE || type == LUA_TNIL) {
        mask = PLAT_MOUSE_LEFT;
    } else if (type == LUA_TNUMBER) {
        mask = iarg(L, 1) & 7;
    } else if (type == LUA_TSTRING) {
        const char *name = lua_tostring(L, 1);
        mask = same_name(name, "left") ? PLAT_MOUSE_LEFT : same_name(name, "right") ? PLAT_MOUSE_RIGHT : same_name(name, "middle") ? PLAT_MOUSE_MIDDLE : 0;
    }
    if (!mask)
        return luaL_error(L, "click(button): \"left\" \"right\" \"middle\", or 1, 2, 4");
    lua_pushboolean(L, e->hooks.mouse && (e->hooks.mouse().pressed & mask));
    return 1;
}

/* ---- registration ---- */

/* ---- graphics and map files (TGF: ../src/tgf.h, ../docs/tgf-format.md) ---- */

typedef struct {
    int fd;
} file_io_t;

static int fio_read(void *c, void *b, uint32_t n) { return fs_read(((file_io_t *)c)->fd, b, n); }
static int fio_write(void *c, const void *b, uint32_t n) { return fs_write(((file_io_t *)c)->fd, b, n); }
static int fio_seek(void *c, uint32_t pos)
{
    const int64_t r = fs_seek(((file_io_t *)c)->fd, pos, FS_SEEK_SET);
    return r < 0 ? (int)r : 0;
}

/* Opens the file and checks all of it: 0 with the file open (the caller closes f->fd), or an error with what is wrong in `why`. */
static int open_tgf(const char *path, file_io_t *f, tgf_io_t *io, tgf_info_t *info, char *why, size_t n)
{
    fs_stat_t st;
    int r = fs_stat(path, &st);
    if (r == 0 && st.is_dir)
        r = FS_EISDIR;
    if (r < 0) {
        snprintf(why, n, "%s", fs_strerror(r));
        return r;
    }
    f->fd = fs_open(path, FS_RDONLY);
    if (f->fd < 0) {
        snprintf(why, n, "%s", fs_strerror(f->fd));
        return f->fd;
    }
    *io = (tgf_io_t){fio_read, fio_seek, fio_write, f, st.size > 0xFFFFFFFFll ? 0xFFFFFFFFu : (uint32_t)st.size};
    r = tgf_check(io, info, why, n);
    if (r < 0)
        fs_close(f->fd);
    return r;
}

/* Writes a file through `body`, into a new file that then takes the old one's place, so that a failure leaves the old one as it was. 0, or an error
 * with what went wrong in `why`. */
static int write_tgf(const char *path, int (*body)(const tgf_io_t *io, void *arg), void *arg, char *why, size_t n)
{
    char tmp[FS_PATH_MAX + 1];
    if (snprintf(tmp, sizeof tmp, "%s.TMP", path) >= (int)sizeof tmp) {
        snprintf(why, n, "%s", fs_strerror(FS_ENAMETOOLONG));
        return FS_ENAMETOOLONG;
    }
    file_io_t f = {fs_open(tmp, FS_WRONLY | FS_CREAT | FS_TRUNC)};
    if (f.fd < 0) {
        snprintf(why, n, "%s", fs_strerror(f.fd));
        return f.fd;
    }
    const tgf_io_t io = {fio_read, fio_seek, fio_write, &f, 0};
    int r = body(&io, arg);
    const int closed = fs_close(f.fd);
    if (!r)
        r = closed;
    fs_stat_t st;
    if (!r && fs_stat(path, &st) == 0)
        r = fs_unlink(path);
    if (!r)
        r = fs_rename(tmp, path);
    if (r < 0) {
        fs_unlink(tmp);
        snprintf(why, n, "%s", r == TGF_EIO ? "the card is full" : fs_strerror(r));
    }
    return r;
}

/* loadgfx(file): the palette, tiles, sprites, flags and animation tables in the file (what it has of them); with no graphics yet, gfx(size) first with the
 * file's tile size. The file is checked before anything changes. */
static int l_loadgfx(lua_State *L)
{
    tl_env_t *e = E;
    const char *path = luaL_checkstring(L, 1);
    file_io_t f;
    tgf_io_t io;
    tgf_info_t info;
    char why[160];
    if (open_tgf(path, &f, &io, &info, why, sizeof why) < 0)
        return luaL_error(L, "loadgfx: %s: %s", path, why);
    const int log2 = info.tile_size == 8 ? 3 : 4;
    tl_game_t *G = e->game;
    if (G && G->size_log2 != log2) {
        fs_close(f.fd);
        return luaL_error(L, "loadgfx: %s has tiles of %d pixels, the graphics now %d (gfx(%d) first)", path, info.tile_size, 1 << G->size_log2, info.tile_size);
    }
    if (!G && !setup(e, log2)) {
        fs_close(f.fd);
        return luaL_error(L, "not enough memory for the graphics");
    }
    G = e->game;
    int r = tgf_read_part(&io, &info, TGF_PALT, G->rgb);
    for (int i = info.part[TGF_PALT].first; !r && i < info.part[TGF_PALT].first + info.part[TGF_PALT].count; i++)
        set_colour(G, i, G->rgb[i][0], G->rgb[i][1], G->rgb[i][2]);
    if (!r)
        r = tgf_read_part(&io, &info, TGF_TILE, G->tiles);
    if (!r)
        r = tgf_read_part(&io, &info, TGF_SPRT, G->sprites);
    if (!r)
        r = tgf_read_part(&io, &info, TGF_FLAG, G->flags);
    for (int t = 0; !r && t < TGF_TABLES; t++) {
        if (!info.table[t].present)
            continue;
        tgf_frame_t fr[TGF_FRAMES];
        r = tgf_read_table(&io, &info, t + 1, fr);
        for (int i = 0; !r && i < info.table[t].count; i++)
            G->anim_frames[t][i] = (tg_anim_frame_t){.tile = fr[i].sprite, .ticks = fr[i].ticks};
        G->anims[t].count = info.table[t].count;
        G->anims[t].mode = info.table[t].mode;
    }
    fs_close(f.fd);
    if (r)
        return luaL_error(L, "loadgfx: %s could not be read: %s", path, fs_strerror(r));
    return 0;
}

/* loadmap(file [, layer]): the maps in the file, each into its layer, made at its size (as newmap makes one); with a layer, the file's one map into that layer.
 * The file is checked, and the memory found, before any map changes. */
static int l_loadmap(lua_State *L)
{
    tl_env_t *e = E;
    const char *path = luaL_checkstring(L, 1);
    const int target = lua_isnoneornil(L, 2) ? -1 : layer_arg(L, 2, "loadmap(file, layer)");
    tl_game_t *G = need_game(L);
    file_io_t f;
    tgf_io_t io;
    tgf_info_t info;
    char why[160];
    if (open_tgf(path, &f, &io, &info, why, sizeof why) < 0)
        return luaL_error(L, "loadmap: %s: %s", path, why);
    if (info.maps == 0 || (target >= 0 && info.maps != 1)) {
        fs_close(f.fd);
        if (info.maps == 0)
            return luaL_error(L, "loadmap: %s has no map", path);
        return luaL_error(L, "loadmap: %s has %d maps: loadmap(file) puts each in its own layer", path, info.maps);
    }
    layer_mem_t m[TGF_LAYERS];
    memset(m, 0, sizeof m);
    int r = 0, short_of = 0;
    for (int k = 0; k < TGF_LAYERS && !r && !short_of; k++) {
        if (!info.map[k].present)
            continue;
        if (!layer_mem(e, G, (int32_t)info.map[k].w, (int32_t)info.map[k].h, &m[k]))
            short_of = 1;
        else
            r = tgf_read_map(&io, &info, k + 1, m[k].map);
    }
    fs_close(f.fd);
    if (r || short_of) {
        for (int k = 0; k < TGF_LAYERS; k++)
            layer_mem_free(e, &m[k]);
        if (short_of)
            return luaL_error(L, "loadmap: not enough memory for the maps of %s", path);
        return luaL_error(L, "loadmap: %s could not be read: %s", path, fs_strerror(r));
    }
    int bad = 0;
    for (int k = 0; k < TGF_LAYERS; k++) {
        if (!info.map[k].present)
            continue;
        const int i = target >= 0 ? target : k;
        free_layer(e, G, i);
        if (bad || !install_layer(e, G, i, (int32_t)info.map[k].w, (int32_t)info.map[k].h, &m[k])) {
            layer_mem_free(e, &m[k]);
            bad = 1;
            continue;
        }
        if (info.map[k].flags & TGF_MAP_WRAPS)
            tg_world_set_border(&G->lay[i].world, TG_BORDER_WRAP, 0);
    }
    wrap_actors(G);   /* (the solid layer's map may be new) */
    if (bad)
        return luaL_error(L, "loadmap: a map of %s could not be set up", path);
    return 0;
}

/* How many of the set's tiles to write: up to the last that is not all see-through. */
static int tiles_used(const uint8_t *set, int px)
{
    int n = TL_TILES;
    while (n > 0) {
        const uint8_t *t = set + (size_t)(n - 1) * (size_t)px;
        int empty = 1;
        for (int i = 0; i < px && empty; i++)
            empty = t[i] == 0;
        if (!empty)
            break;
        n--;
    }
    return n;
}

static int gfx_body(const tgf_io_t *io, void *arg)
{
    const tl_game_t *G = arg;
    const int size = 1 << G->size_log2, px = size * size;
    const int nt = tiles_used(G->tiles, px), ns = tiles_used(G->sprites, px);
    int nf = TL_TILES;
    while (nf > 0 && !G->flags[nf - 1])
        nf--;
    int r = tgf_write_header(io, size);
    if (!r)
        r = tgf_write_part(io, TGF_PALT, 0, 256, size, G->rgb);
    if (!r && nt)
        r = tgf_write_part(io, TGF_TILE, 0, nt, size, G->tiles);
    if (!r && ns)
        r = tgf_write_part(io, TGF_SPRT, 0, ns, size, G->sprites);
    if (!r && nf)
        r = tgf_write_part(io, TGF_FLAG, 0, nf, size, G->flags);
    for (int t = 0; !r && t < TL_MAX_ANIMS; t++) {
        if (!G->anims[t].count)
            continue;
        tgf_frame_t fr[TGF_FRAMES];
        for (int i = 0; i < G->anims[t].count; i++)
            fr[i] = (tgf_frame_t){.sprite = G->anim_frames[t][i].tile, .ticks = G->anim_frames[t][i].ticks};
        r = tgf_write_table(io, t + 1, G->anims[t].mode, G->anims[t].count, fr);
    }
    return r ? r : tgf_write_end(io);
}

/* savegfx(file): the palette, tiles, sprites, flags and animation tables, as a TGF file */
static int l_savegfx(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    tl_game_t *G = need_game(L);
    char why[160];
    if (write_tgf(path, gfx_body, G, why, sizeof why) < 0)
        return luaL_error(L, "savegfx: %s: %s", path, why);
    return 0;
}

typedef struct {
    const tl_game_t *G;
    int only;   /* the one layer to write, or -1 for all that have a map */
} map_job_t;

static int map_body(const tgf_io_t *io, void *arg)
{
    const map_job_t *j = arg;
    int r = tgf_write_header(io, 1 << j->G->size_log2);
    for (int i = 0; !r && i < TG_MAX_LAYERS; i++) {
        const tl_layer_t *l = &j->G->lay[i];
        if ((j->only >= 0 && i != j->only) || !l->has_world)
            continue;
        r = tgf_write_map(io, i + 1, l->world.border == TG_BORDER_WRAP ? TGF_MAP_WRAPS : 0, (uint32_t)l->world.map_w, (uint32_t)l->world.map_h, l->world.map);
    }
    return r ? r : tgf_write_end(io);
}

/* savemap(file [, layer]): the maps of the layers that have one, or the one layer's, as a TGF file */
static int l_savemap(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    tl_game_t *G = need_game(L);
    map_job_t j = {G, lua_isnoneornil(L, 2) ? -1 : layer_arg(L, 2, "savemap(file, layer)")};
    int any = 0;
    for (int i = 0; i < TG_MAX_LAYERS; i++)
        any |= G->lay[i].has_world && (j.only < 0 || i == j.only);
    if (!any)
        return j.only >= 0 ? luaL_error(L, "savemap: there is no map on layer %d", j.only + 1) : luaL_error(L, "savemap: there is no map to save");
    char why[160];
    if (write_tgf(path, map_body, &j, why, sizeof why) < 0)
        return luaL_error(L, "savemap: %s: %s", path, why);
    return 0;
}

#define F(n) {#n, l_##n}
void tl_game_open(lua_State *L)
{
    E = tl_env_current();
    static const luaL_Reg functions[] = {
        F(gfx), F(palette), F(tiledef), F(sprdef), F(tilerow), F(sprrow), F(tilepix), F(sprpix), F(loadgfx), F(loadmap), F(savegfx), F(savemap),
        F(newmap), F(layer), F(solidlayer), F(parallax), F(wrap), F(tile), F(gettile), F(tilefill), F(tilestr), F(settileflags), F(tileflag), F(tilebox), F(camera), F(camx), F(camy), F(follow), F(followlimit),
        F(actor), F(actorfree), F(actorpos), F(actorx), F(actory), F(actorvel), F(actortile), F(actorsize), F(actorflip), F(actorhide), F(actorprio),
        F(actorbox), F(actoruser), F(actorlayer), F(animdef), F(actoranim), F(animdone), F(animframe), F(ysort), F(actormove), F(hitx), F(hity), F(hittile), F(hitflags),
        F(lastx), F(lasty), F(actorhit), F(waitframe), F(onframe), F(run), F(textmode), F(frames), F(millis), F(btn), F(pressed),
        F(key), F(mouse), F(click),
        {NULL, NULL},
    };
    for (const luaL_Reg *r = functions; r->func; r++)
        lua_register(L, r->name, r->func);
}
