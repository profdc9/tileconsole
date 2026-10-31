/* SPDX-License-Identifier: MIT */
/* Tests of the view: layers, double buffering, the HUD, sprites, and that everything drawn has a defined colour. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "view.h"

static int checks;
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static game_t game;
static view_t view;

static void fresh(void)
{
    game_init(&game);
    view_init(&view);
}

#define HUD_ENTRY(c, pal) TG_ENTRY((c) - 32 + 1, (pal), 0)   /* ASCII is loaded into tiles 1..95 */

static void test_setup(void)
{
    fresh();
    CHECK(view.bg_tiles_used > 10 && view.bg_tiles_used <= BG_TILES_MAX);
    CHECK(view.g.layers[2].glyphs.offset == 1 - 32 && view.g.layers[2].glyphs.first == 32 && view.g.layers[2].glyphs.count == 95);
    CHECK(view.g.sprite_count == 1 + MAX_ENEMIES + COIN_POOL);
    CHECK(view.player_slot == 0);                       /* the lowest slot is in front */
    /* The background map names real tiles. */
    for (int i = 0; i < 32 * 16; i++)
        CHECK((view.bg_map[i] & TG_TILE_MASK) < (unsigned)view.bg_tiles_used);
}

static void test_level_columns_and_scroll(void)
{
    fresh();
    view_draw(&view, &game);
    CHECK(view.g.layers[1].scroll_x == 0);
    for (int tx = 0; tx <= 21; tx++)
        for (int ty = 0; ty < LEVEL_H; ty++)
            CHECK(view.g.layers[1].map[ty * 32 + tx] == TG_ENTRY(level_tile(&game.level, tx, ty), PAL_TILES, 0));

    /* Scrolled far along, the visible columns wrap round the 32-column map and still match the level. */
    game.camera_x = 777;
    view_draw(&view, &game);
    CHECK(view.g.layers[1].scroll_x == 777 && view.g.layers[0].scroll_x == 777 / 2);
    for (int tx = 777 / 16; tx <= 777 / 16 + 20; tx++)
        for (int ty = 0; ty < LEVEL_H; ty++)
            CHECK(view.g.layers[1].map[ty * 32 + (tx & 31)] == TG_ENTRY(level_tile(&game.level, tx, ty), PAL_TILES, 0));
}

/* What was just presented must not be written to by the next draw: the display may still be reading it. */
static void test_double_buffering(void)
{
    fresh();
    view_draw(&view, &game);
    const uint16_t *main_a = view.g.layers[1].map, *hud_a = view.g.layers[2].map;
    static uint16_t main_copy[32 * 16], hud_copy[64 * 32];
    memcpy(main_copy, main_a, sizeof main_copy);
    memcpy(hud_copy, hud_a, sizeof hud_copy);

    game.camera_x = 400;
    game.score = 4321;
    view_draw(&view, &game);
    CHECK(view.g.layers[1].map != main_a && view.g.layers[2].map != hud_a);   /* another buffer */
    CHECK(memcmp(main_a, main_copy, sizeof main_copy) == 0);                  /* and the first one untouched */
    CHECK(memcmp(hud_a, hud_copy, sizeof hud_copy) == 0);

    view_draw(&view, &game);
    CHECK(view.g.layers[1].map == main_a && view.g.layers[2].map == hud_a);   /* they alternate */
    /* The background is never written after start-up, so it needs no buffering; nor do the graphics. */
    static uint16_t bg_copy[32 * 16];
    memcpy(bg_copy, view.bg_map, sizeof bg_copy);
    for (int f = 0; f < 5; f++)
        view_draw(&view, &game);
    CHECK(memcmp(bg_copy, view.bg_map, sizeof bg_copy) == 0);
}

static void expect_text(const tg_layer_t *hud, int x, int y, const char *text, unsigned pal)
{
    for (int i = 0; text[i]; i++)
        CHECK(hud->map[y * 64 + x + i] == HUD_ENTRY((unsigned char)text[i], pal));
}

static bool row_is_blank(const tg_layer_t *hud, int y)
{
    for (int x = 0; x < 64; x++)
        if (hud->map[y * 64 + x])
            return false;
    return true;
}

static void test_hud(void)
{
    fresh();
    game.score = 1234;
    game.coins = 7;
    game.lives = 2;
    view_draw(&view, &game);
    expect_text(&view.g.layers[2], 4, 1, "SCORE 001234  COINS 07  LIVES 2", PAL_TEXT);
    for (int y = 2; y < 32; y++)
        CHECK(row_is_blank(&view.g.layers[2], y));           /* nothing else while playing */

    game.score = 999999;
    game.coins = 48;
    game.lives = 0;
    game.state = GS_GAMEOVER;
    view_draw(&view, &game);
    expect_text(&view.g.layers[2], 4, 1, "SCORE 999999  COINS 48  LIVES 0", PAL_TEXT);
    expect_text(&view.g.layers[2], 15, 12, "GAME OVER", PAL_TEXT_HI);
    expect_text(&view.g.layers[2], 14, 14, "PRESS START", PAL_TEXT);

    game.state = GS_WIN;
    view_draw(&view, &game);
    expect_text(&view.g.layers[2], 16, 12, "YOU WIN!", PAL_TEXT_HI);
    expect_text(&view.g.layers[2], 14, 14, "PRESS START", PAL_TEXT);
    game.state = GS_PLAYING;
    view_draw(&view, &game);                                  /* the messages go away again */
    CHECK(row_is_blank(&view.g.layers[2], 12) && row_is_blank(&view.g.layers[2], 14));
}

static const tg_sprite_t *spr(int slot) { return &view.g.sprites[slot]; }

static void test_sprites(void)
{
    fresh();
    game.camera_x = 40;
    game.p.b.x = (100 * 256) + 0;
    game.p.b.y = 150 << FP;
    game.p.b.on_ground = true;
    game.p.facing_left = false;
    view_draw(&view, &game);
    CHECK((spr(view.player_slot)->flags & TG_SPRITE_ON) && spr(view.player_slot)->x == 100 - 40 && spr(view.player_slot)->y == 150);
    CHECK(spr(view.player_slot)->tile == SPR_PLAYER_IDLE && !(spr(view.player_slot)->flags & TG_SPRITE_HFLIP));
    CHECK(spr(view.player_slot)->pal == PAL_PLAYER && spr(view.player_slot)->above == 2);

    game.p.facing_left = true;
    game.p.b.on_ground = false;
    view_draw(&view, &game);
    CHECK(spr(view.player_slot)->tile == SPR_PLAYER_JUMP && (spr(view.player_slot)->flags & TG_SPRITE_HFLIP));
    game.p.b.on_ground = true;
    game.p.anim = 1;
    view_draw(&view, &game);
    CHECK(spr(view.player_slot)->tile == SPR_PLAYER_RUN1);
    game.p.anim = 6;
    view_draw(&view, &game);
    CHECK(spr(view.player_slot)->tile == SPR_PLAYER_RUN2);
    game.state = GS_DYING;
    view_draw(&view, &game);
    CHECK((spr(view.player_slot)->flags & TG_SPRITE_VFLIP) && spr(view.player_slot)->tile == SPR_PLAYER_JUMP);
    game.state = GS_PLAYING;

    /* Enemies: shown when on screen, hidden when off screen, squashed after a stomp, gone when the squash is over. */
    fresh();
    game_init(&game);
    game.camera_x = 0;
    const int ex = (game.enemy[0].b.x >> FP);
    CHECK(ex < 320);                                          /* the first enemy starts on the first screen */
    view_draw(&view, &game);
    CHECK((spr(view.enemy_slot[0])->flags & TG_SPRITE_ON) && spr(view.enemy_slot[0])->x == ex);
    CHECK(spr(view.enemy_slot[0])->tile == SPR_ENEMY0 || spr(view.enemy_slot[0])->tile == SPR_ENEMY1);
    for (int i = 1; i < game.level.nenemies; i++)             /* the others are further along */
        if ((game.enemy[i].b.x >> FP) > 320)
            CHECK(!(spr(view.enemy_slot[i])->flags & TG_SPRITE_ON));
    for (int i = game.level.nenemies; i < MAX_ENEMIES; i++)
        CHECK(!(spr(view.enemy_slot[i])->flags & TG_SPRITE_ON));
    game.enemy[0].alive = false;
    game.enemy[0].squash = 10;
    view_draw(&view, &game);
    CHECK((spr(view.enemy_slot[0])->flags & TG_SPRITE_ON) && spr(view.enemy_slot[0])->tile == SPR_ENEMY_SQUASHED);
    game.enemy[0].squash = 0;
    view_draw(&view, &game);
    CHECK(!(spr(view.enemy_slot[0])->flags & TG_SPRITE_ON));
    game.enemy[0].alive = true;
    game.enemy[0].facing_left = false;
    view_draw(&view, &game);
    CHECK(spr(view.enemy_slot[0])->flags & TG_SPRITE_HFLIP);   /* the art looks left, so walking right mirrors it */

    /* Coins: the ones on screen get pool sprites, the taken ones do not. */
    fresh();
    game.camera_x = 0;
    int on_screen = 0;
    for (int i = 0; i < game.level.ncoins; i++)
        on_screen += game.level.coin[i].x * TILE >= -16 && game.level.coin[i].x * TILE <= 320;
    view_draw(&view, &game);
    int shown = 0;
    for (int i = 0; i < COIN_POOL; i++)
        shown += (spr(view.coin_slot[i])->flags & TG_SPRITE_ON) != 0;
    CHECK(shown == (on_screen < COIN_POOL ? on_screen : COIN_POOL));
    game.coin_taken[0] = true;
    view_draw(&view, &game);
    int shown2 = 0;
    for (int i = 0; i < COIN_POOL; i++)
        shown2 += (spr(view.coin_slot[i])->flags & TG_SPRITE_ON) != 0;
    CHECK(shown2 == shown - 1);
    for (int i = 0; i < COIN_POOL; i++)                       /* the visible ones come first in the pool, and are at their tiles */
        if (spr(view.coin_slot[i])->flags & TG_SPRITE_ON)
            CHECK(spr(view.coin_slot[i])->pal == PAL_COIN && spr(view.coin_slot[i])->tile >= SPR_COIN0 && spr(view.coin_slot[i])->tile <= SPR_COIN3);
}

/* ---- the picture ---- */

static bool solid_at_px(const game_t *g, int px, int py) { return tile_is_solid(level_tile(&g->level, px >> 4, py >> 4)); }

static plat_buttons_t bot(const game_t *g)
{
    plat_buttons_t held = PLAT_BTN_RIGHT | GAME_BTN_RUN;
    const body_t *b = &g->p.b;
    const int front = body_left(b) + b->w, top = body_top(b), bottom = top + b->h;
    const bool wall = solid_at_px(g, front + 3, bottom - 4) || solid_at_px(g, front + 3, top + 2);
    const bool edge = !solid_at_px(g, front + 1, bottom + 2) && !solid_at_px(g, front - 6, bottom + 2);
    bool enemy_ahead = false;
    for (int i = 0; i < g->level.nenemies; i++) {
        const enemy_t *e = &g->enemy[i];
        if (!e->alive)
            continue;
        const int ex = body_left(&e->b), ey = body_top(&e->b);
        if (ex > front && ex - front < 34 && ey - top < 16 && top - ey < 16)
            enemy_ahead = true;
    }
    if (b->on_ground && (wall || edge || enemy_ahead))
        held |= GAME_BTN_JUMP;
    else if (!b->on_ground && b->vy < 0)
        held |= GAME_BTN_JUMP;
    return held;
}

/* Through a whole playthrough, every pixel drawn has a colour that was defined (an undefined sub-palette entry would be black),
 * the picture is not blank, and no line has more sprites than the compositor will draw. */
static void test_whole_run_picture(void)
{
    fresh();
    plat_buttons_t prev = 0;
    int max_per_line = 0, frames_drawn = 0;
    unsigned long colours_seen[8] = {0};
    for (int f = 0; f < 1200 && game.state != GS_WIN; f++) {
        const plat_buttons_t held = bot(&game);
        game_update(&game, held, held & (plat_buttons_t)~prev);
        prev = held;
        if (f % 20)
            continue;
        view_draw(&view, &game);
        frames_drawn++;
        int nonbackdrop = 0;
        for (int y = 0; y < TG_HEIGHT; y++) {
            uint8_t idx[TG_WIDTH];
            tg_render_line_indices(&view.g, y, idx);
            for (int x = 0; x < TG_WIDTH; x++) {
                CHECK(view.g.palette[idx[x]] != 0);            /* a colour was defined for it */
                nonbackdrop += idx[x] != 0;
                colours_seen[idx[x] >> 5] |= 1ul << (idx[x] & 31);
            }
            int on_line = 0;
            for (int s = 0; s < view.g.sprite_count; s++) {
                const tg_sprite_t *sp = &view.g.sprites[view.g.sprite_order[s]];
                on_line += (sp->flags & TG_SPRITE_ON) && y >= sp->y && y < sp->y + 16 && sp->x > -16 && sp->x < 320;
            }
            max_per_line = on_line > max_per_line ? on_line : max_per_line;
        }
        CHECK(nonbackdrop > 20000);                              /* hills, ground and HUD: far more than a few stray pixels */
    }
    CHECK(frames_drawn > 30);
    CHECK(max_per_line <= TG_MAX_SPRITES_PER_LINE);
    /* All the sub-palettes were really used: tiles, background, player, enemy, coin and text. */
    int distinct = 0;
    for (int i = 0; i < 8; i++)
        for (int b = 0; b < 32; b++)
            distinct += (colours_seen[i] >> b) & 1;
    CHECK(distinct > 30);
    printf("  %d frames drawn over a run, %d distinct palette entries used, at most %d sprites on a line\n", frames_drawn, distinct, max_per_line);
}

int main(void)
{
    test_setup();
    test_level_columns_and_scroll();
    test_double_buffering();
    test_hud();
    test_sprites();
    test_whole_run_picture();
    printf("all %d checks passed\n", checks);
    return 0;
}
