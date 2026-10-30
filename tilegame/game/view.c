/* SPDX-License-Identifier: MIT */
#include "view.h"

#include <math.h>
#include <string.h>

#include "tg_font.h"
#include "tg_fonts.h"

#define RGB(r, gr, b) ((uint16_t)((((r) >> 3) << 11) | (((gr) >> 2) << 5) | ((b) >> 3)))
#define SUBPAL(p, i, r, gr, b) (v->g.palette[((p) << 4) | (i)] = RGB(r, gr, b))

/* ---- drawing into a 16x16 tile of 4-bit pixels ---- */

typedef uint8_t px_t[16][16];

static void put(px_t px, int x, int y, int c)
{
    if ((unsigned)x < 16 && (unsigned)y < 16)
        px[y][x] = (uint8_t)c;
}

static void rect(px_t px, int x, int y, int w, int h, int c)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            put(px, x + i, y + j, c);
}

static void ellipse(px_t px, int cx, int cy, int rx, int ry, int c)
{
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            const int dx = x - cx, dy = y - cy;
            if (dx * dx * ry * ry + dy * dy * rx * rx <= rx * rx * ry * ry)
                px[y][x] = (uint8_t)c;
        }
}

/* Left pixel in the high nibble, as tilegfx wants 4 bpp tiles. */
static void pack(uint8_t *dst, px_t px)
{
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x += 2)
            dst[y * 8 + x / 2] = (uint8_t)(px[y][x] << 4 | px[y][x + 1]);
}

static unsigned noise(unsigned x, unsigned y, unsigned seed)
{
    unsigned h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/* ---- the level's tiles (sub-palette PAL_TILES) ---- */

static void draw_dirt(px_t px, unsigned seed)
{
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            const unsigned n = noise((unsigned)x, (unsigned)y, seed) % 11;
            px[y][x] = n == 0 ? 4 : (n == 1 ? 6 : 5);   /* dark and light specks on brown */
        }
}

static void draw_grass(px_t px)
{
    draw_dirt(px, 7);
    for (int y = 0; y < 5; y++)
        for (int x = 0; x < 16; x++)
            px[y][x] = y < 2 ? 3 : 2;
    for (int x = 0; x < 16; x++) {   /* a ragged lower edge of blades */
        const int len = 4 + (int)(noise((unsigned)x, 1, 3) % 3);
        for (int y = 5; y < len + 1; y++)
            px[y][x] = 1;
        px[len][x] = 1;
    }
}

static void draw_brick(px_t px)
{
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            px[y][x] = 8;
    for (int row = 0; row < 4; row++) {
        const int y0 = row * 4, shift = (row & 1) ? 4 : 0;
        for (int x = 0; x < 16; x++) {
            px[y0][x] = 9;           /* the light top edge of each brick row... */
            px[y0 + 3][x] = 10;      /* ...and the mortar under it */
        }
        for (int x = shift; x < 16; x += 8)
            for (int y = y0; y < y0 + 3; y++)
                px[y][x] = 10;       /* the joints */
        for (int x = 0; x < 16; x++)
            px[y0 + 2][x] = px[y0 + 2][x] == 8 ? 7 : px[y0 + 2][x];   /* a darker band at the bottom */
    }
}

static void draw_block(px_t px)
{
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            px[y][x] = 12;
    for (int i = 0; i < 16; i++) {
        px[0][i] = 13;
        px[i][0] = 13;
        px[15][i] = 11;
        px[i][15] = 11;
    }
    rect(px, 4, 4, 8, 8, 11);        /* a recessed centre */
    rect(px, 5, 5, 6, 6, 12);
}

/* ---- the sprites ---- */

static void draw_player(px_t px, int frame)
{
    memset(px, 0, sizeof(px_t));
    ellipse(px, 8, 8, 7, 7, 1);
    ellipse(px, 8, 8, 6, 6, 2);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            if (px[y][x] == 2 && (x - 8) + (y - 8) > 4)
                px[y][x] = 3;        /* shaded underside */
    rect(px, 9, 5, 3, 4, 4);         /* an eye, white... */
    rect(px, 11, 6, 1, 2, 5);        /* ...with the pupil looking ahead */
    switch (frame) {
    case SPR_PLAYER_IDLE:
        rect(px, 4, 13, 3, 2, 6);
        rect(px, 9, 13, 3, 2, 6);
        break;
    case SPR_PLAYER_RUN1:
        rect(px, 2, 13, 3, 2, 6);
        rect(px, 10, 12, 3, 2, 6);
        break;
    case SPR_PLAYER_RUN2:
        rect(px, 5, 13, 3, 2, 6);
        rect(px, 7, 13, 3, 2, 6);
        break;
    default:                         /* jumping: feet tucked up */
        rect(px, 4, 12, 3, 2, 6);
        rect(px, 9, 12, 3, 2, 6);
        break;
    }
}

static void draw_coin(px_t px, int frame)
{
    static const int rx[4] = {5, 3, 1, 3};
    memset(px, 0, sizeof(px_t));
    ellipse(px, 8, 8, rx[frame] + 1, 6, 1);
    ellipse(px, 8, 8, rx[frame], 5, 2);
    if (rx[frame] > 2)
        rect(px, 8 - rx[frame] + 1, 4, 1, 4, 3);   /* a highlight */
    put(px, 8, 6, 4);
}

static void draw_enemy(px_t px, int frame)
{
    memset(px, 0, sizeof(px_t));
    if (frame == SPR_ENEMY_SQUASHED - SPR_ENEMY0) {
        ellipse(px, 8, 13, 7, 2, 1);
        ellipse(px, 8, 13, 6, 1, 2);
        rect(px, 4, 12, 2, 1, 4);
        rect(px, 10, 12, 2, 1, 4);
        return;
    }
    ellipse(px, 8, 10, 7, 6, 1);
    ellipse(px, 8, 10, 6, 5, 2);
    for (int y = 4; y < 9; y++)
        for (int x = 3; x < 13; x++)
            if (px[y][x] == 2)
                px[y][x] = 3;        /* a lighter back */
    rect(px, 4, 6, 3, 3, 4);
    rect(px, 9, 6, 3, 3, 4);
    put(px, 4, 7, 5);                /* both pupils look left, the way it walks (mirrored when it goes right) */
    put(px, 9, 7, 5);
    rect(px, 5, 11, 6, 1, 7);        /* a mouth */
    const int s = frame ? 1 : 0;
    for (int leg = 0; leg < 4; leg++)
        rect(px, 2 + leg * 3 + s, 14, 2, 2, 6);
}

/* ---- the background: hills and clouds, drawn as one 512 x 240 picture and cut into deduplicated tiles ---- */

#define BG_W 512
#define BG_H 240
#define PI 3.14159265358979323846

/* The two hill lines depend only on x, so they are worked out once (they are static: the board's stack is small). */
static int16_t far_top[BG_W], near_top[BG_W];

static void hill_lines(void)
{
    for (int x = 0; x < BG_W; x++) {
        const double t = 2 * PI * x / BG_W;
        far_top[x] = (int16_t)(150 + (int)(18 * sin(2 * t) + 10 * sin(3 * t + 1.0)));
        near_top[x] = (int16_t)(178 + (int)(14 * sin(t + 2.0) + 6 * sin(5 * t)));
    }
}

static int bg_pixel(int x, int y)
{
    static const struct { int cx, cy, r; } clouds[] = {
        {60, 40, 14},  {76, 36, 18},  {94, 42, 12},  {230, 62, 12}, {246, 58, 16}, {262, 64, 11},
        {380, 34, 13}, {396, 30, 17}, {414, 37, 12}, {470, 70, 10}, {484, 66, 13},
    };
    if (y >= near_top[x])
        return 2;                          /* near hills */
    if (y >= far_top[x])
        return 1;                          /* far hills */
    for (size_t i = 0; i < sizeof clouds / sizeof *clouds; i++) {
        const int dx = x - clouds[i].cx, dy = (y - clouds[i].cy) * 2;
        if (dx * dx + dy * dy <= clouds[i].r * clouds[i].r) {
            const int below = y > clouds[i].cy + clouds[i].r / 4;
            return below ? 5 : 4;
        }
    }
    return 0;                              /* sky: the backdrop shows through */
}

static void build_background(view_t *v)
{
    hill_lines();
    v->bg_tiles_used = 1;                  /* tile 0 stays blank */
    for (int ty = 0; ty < 15; ty++)
        for (int tx = 0; tx < 32; tx++) {
            px_t px;
            int any = 0;
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) {
                    px[y][x] = (uint8_t)bg_pixel(tx * 16 + x, ty * 16 + y);
                    any |= px[y][x];
                }
            int found = 0;
            if (any) {
                uint8_t packed[128];
                pack(packed, px);
                for (int i = 1; i < v->bg_tiles_used && !found; i++)
                    if (memcmp(v->bg_tiles + i * 128, packed, 128) == 0)
                        found = i;
                if (!found && v->bg_tiles_used < BG_TILES_MAX) {
                    found = v->bg_tiles_used++;
                    memcpy(v->bg_tiles + found * 128, packed, 128);
                }
            }
            v->bg_map[ty * 32 + tx] = TG_ENTRY(found, PAL_BG, 0);
        }
}

/* ---- setting up ---- */

void view_init(view_t *v)
{
    memset(v, 0, sizeof *v);
    tg_init(&v->g);

    /* Colours. Index 0 of each sub-palette is transparent; the backdrop (palette 0) is the sky. */
    v->g.palette[0] = RGB(110, 170, 250);
    SUBPAL(PAL_BG, 1, 96, 150, 150);    SUBPAL(PAL_BG, 2, 70, 126, 120);    SUBPAL(PAL_BG, 4, 250, 250, 255);
    SUBPAL(PAL_BG, 5, 205, 220, 245);
    SUBPAL(PAL_TILES, 1, 40, 110, 40);  SUBPAL(PAL_TILES, 2, 70, 170, 60);  SUBPAL(PAL_TILES, 3, 130, 210, 90);
    SUBPAL(PAL_TILES, 4, 110, 70, 40);  SUBPAL(PAL_TILES, 5, 150, 100, 60); SUBPAL(PAL_TILES, 6, 185, 135, 85);
    SUBPAL(PAL_TILES, 7, 150, 60, 40);  SUBPAL(PAL_TILES, 8, 190, 90, 60); SUBPAL(PAL_TILES, 9, 220, 130, 90);
    SUBPAL(PAL_TILES, 10, 90, 60, 50);  SUBPAL(PAL_TILES, 11, 90, 90, 130); SUBPAL(PAL_TILES, 12, 140, 140, 190);
    SUBPAL(PAL_TILES, 13, 200, 200, 240);
    SUBPAL(PAL_PLAYER, 1, 40, 20, 60);   SUBPAL(PAL_PLAYER, 2, 240, 140, 40);  SUBPAL(PAL_PLAYER, 3, 200, 90, 30);
    SUBPAL(PAL_PLAYER, 4, 255, 255, 255); SUBPAL(PAL_PLAYER, 5, 20, 20, 40);   SUBPAL(PAL_PLAYER, 6, 80, 50, 140);
    SUBPAL(PAL_ENEMY, 1, 50, 10, 30);    SUBPAL(PAL_ENEMY, 2, 190, 40, 90);    SUBPAL(PAL_ENEMY, 3, 230, 90, 130);
    SUBPAL(PAL_ENEMY, 4, 255, 255, 255); SUBPAL(PAL_ENEMY, 5, 20, 10, 30);     SUBPAL(PAL_ENEMY, 6, 120, 30, 70);
    SUBPAL(PAL_ENEMY, 7, 255, 200, 210);
    SUBPAL(PAL_COIN, 1, 150, 100, 0);    SUBPAL(PAL_COIN, 2, 255, 210, 40);    SUBPAL(PAL_COIN, 3, 255, 240, 140);
    SUBPAL(PAL_COIN, 4, 255, 255, 220);
    SUBPAL(PAL_TEXT, 1, 255, 255, 255);  SUBPAL(PAL_TEXT_HI, 1, 255, 230, 80);

    /* The level's tiles. Tile 0 is empty. */
    px_t px;
    draw_grass(px);   pack(v->main_tiles + T_GRASS * 128, px);
    draw_dirt(px, 3); pack(v->main_tiles + T_DIRT * 128, px);
    draw_brick(px);   pack(v->main_tiles + T_BRICK * 128, px);
    draw_block(px);   pack(v->main_tiles + T_BLOCK * 128, px);

    /* The sprites. */
    for (int f = SPR_PLAYER_IDLE; f <= SPR_PLAYER_JUMP; f++) {
        draw_player(px, f);
        pack(v->sprite_gfx + f * 128, px);
    }
    for (int f = 0; f < 4; f++) {
        draw_coin(px, f);
        pack(v->sprite_gfx + (SPR_COIN0 + f) * 128, px);
    }
    for (int f = 0; f < 3; f++) {
        draw_enemy(px, f);
        pack(v->sprite_gfx + (SPR_ENEMY0 + f) * 128, px);
    }

    build_background(v);

    /* The layers: 0 the background, 1 the level, 2 the HUD text; sprites are drawn above 0 and 1. */
    v->g.layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = 4, .depth = 4, .map_w_log2 = 5, .map_h_log2 = 4,
                                  .map = v->bg_map, .tiles = v->bg_tiles};
    v->g.layers[1] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = 4, .depth = 4, .map_w_log2 = 5, .map_h_log2 = 4,
                                  .map = v->main_map[0], .tiles = v->main_tiles};
    v->g.layers[2] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = 3, .depth = 4, .map_w_log2 = 6, .map_h_log2 = 5,
                                  .map = v->hud_map[0], .tiles = v->hud_tiles};
    tg_layer_load_font_ptr(&v->g.layers[2], &tg_font_unscii8, 32, 95, 1, 1, 0);   /* ASCII in tiles 1..95, glyph 1 = white */

    v->g.sprite_tile_log2 = 4;
    v->g.sprite_depth = 4;
    v->g.sprite_tiles = v->sprite_gfx;
    v->player_slot = tg_sprite_alloc(&v->g);
    for (int i = 0; i < MAX_ENEMIES; i++)
        v->enemy_slot[i] = tg_sprite_alloc(&v->g);
    for (int i = 0; i < COIN_POOL; i++)
        v->coin_slot[i] = tg_sprite_alloc(&v->g);
    for (int s = 0; s < 1 + MAX_ENEMIES + COIN_POOL; s++)   /* allocated sprites start visible: hide them until drawn */
        v->g.sprites[s].flags = 0;
}

/* ---- each frame ---- */

static char *put_uint(char *out, unsigned value, int digits)
{
    for (int i = digits - 1; i >= 0; i--) {
        out[i] = (char)('0' + value % 10);
        value /= 10;
    }
    return out + digits;
}

static void centred(const tg_layer_t *hud, int y, const char *text, unsigned pal)
{
    int n = 0;
    while (text[n])
        n++;
    tg_layer_put_string(hud, (40 - n) / 2, y, text, pal);
}

void view_draw(view_t *v, const game_t *game)
{
    tg_t *g = &v->g;
    const int cam = game->camera_x;
    const int buf = v->buf;

    /* The level: the columns on screen, from the level into the back buffer. */
    uint16_t *map = v->main_map[buf];
    for (int tx = (cam >> 4) - 1; tx <= (cam >> 4) + 21; tx++)
        for (int ty = 0; ty < LEVEL_H; ty++)
            map[(ty << 5) | (tx & 31)] = TG_ENTRY(level_tile(&game->level, tx, ty), PAL_TILES, 0);
    g->layers[1].map = map;
    g->layers[1].scroll_x = (uint32_t)cam;
    g->layers[0].scroll_x = (uint32_t)(cam / 2);

    /* The HUD, into its back buffer. */
    uint16_t *hud_map = v->hud_map[buf];
    memset(hud_map, 0, sizeof v->hud_map[0]);
    g->layers[2].map = hud_map;
    char line[48];
    char *p = line;
    memcpy(p, "SCORE ", 6);  p += 6;
    p = put_uint(p, (unsigned)game->score, 6);
    memcpy(p, "  COINS ", 8); p += 8;
    p = put_uint(p, (unsigned)game->coins, 2);
    memcpy(p, "  LIVES ", 8); p += 8;
    p = put_uint(p, (unsigned)game->lives, 1);
    *p = 0;
    tg_layer_put_string(&g->layers[2], 4, 1, line, PAL_TEXT);
    if (game->state == GS_GAMEOVER) {
        centred(&g->layers[2], 12, "GAME OVER", PAL_TEXT_HI);
        centred(&g->layers[2], 14, "PRESS START", PAL_TEXT);
    } else if (game->state == GS_WIN) {
        centred(&g->layers[2], 12, "YOU WIN!", PAL_TEXT_HI);
        centred(&g->layers[2], 14, "PRESS START", PAL_TEXT);
    }

    /* The player. */
    {
        const player_t *pl = &game->p;
        int frame = SPR_PLAYER_IDLE;
        if (!pl->b.on_ground || game->state == GS_DYING)
            frame = SPR_PLAYER_JUMP;
        else if (pl->anim)
            frame = (pl->anim / 5) % 2 ? SPR_PLAYER_RUN2 : SPR_PLAYER_RUN1;
        /* The art faces right: mirror it when facing left; and when dying the player tumbles upside down. */
        g->sprites[v->player_slot] = (tg_sprite_t){
            .x = (int16_t)((pl->b.x >> FP) - cam), .y = (int16_t)(pl->b.y >> FP), .tile = (uint16_t)frame,
            .flags = (uint8_t)(TG_SPRITE_ON | (pl->facing_left ? TG_SPRITE_HFLIP : 0) | (game->state == GS_DYING ? TG_SPRITE_VFLIP : 0)),
            .pal = PAL_PLAYER, .above = 2};
    }

    /* The enemies. */
    for (int i = 0; i < MAX_ENEMIES; i++) {
        tg_sprite_t *s = &g->sprites[v->enemy_slot[i]];
        if (i >= game->level.nenemies) {
            s->flags = 0;
            continue;
        }
        const enemy_t *e = &game->enemy[i];
        const int sx = (e->b.x >> FP) - cam;
        if ((!e->alive && !e->squash) || sx < -16 || sx > 320) {
            s->flags = 0;
            continue;
        }
        const int frame = !e->alive ? SPR_ENEMY_SQUASHED : SPR_ENEMY0 + ((game->frame / 8) & 1);
        *s = (tg_sprite_t){.x = (int16_t)sx, .y = (int16_t)(e->b.y >> FP), .tile = (uint16_t)frame,
                           .flags = (uint8_t)(TG_SPRITE_ON | (e->facing_left ? 0 : TG_SPRITE_HFLIP)), .pal = PAL_ENEMY, .above = 2};
    }

    /* The coins on screen, taken from a small pool of sprites. */
    int next = 0;
    for (int i = 0; i < game->level.ncoins && next < COIN_POOL; i++) {
        if (game->coin_taken[i])
            continue;
        const int sx = game->level.coin[i].x * TILE - cam;
        if (sx < -16 || sx > 320)
            continue;
        g->sprites[v->coin_slot[next++]] = (tg_sprite_t){
            .x = (int16_t)sx, .y = (int16_t)(game->level.coin[i].y * TILE), .tile = (uint16_t)(SPR_COIN0 + ((game->frame / 8 + (unsigned)i) & 3)),
            .flags = TG_SPRITE_ON, .pal = PAL_COIN, .above = 2};
    }
    for (; next < COIN_POOL; next++)
        g->sprites[v->coin_slot[next]].flags = 0;

    v->buf ^= 1;
}
