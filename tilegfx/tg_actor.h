/*
 * SPDX-License-Identifier: MIT
 * tg_actor: game objects in world coordinates (a pool the caller provides), drawn as sprites only while they are on screen. See WORLD.md.
 *
 * An actor is a grid of sprite tiles (a metasprite) at a position in global pixels, in 24.8 fixed point. Once a frame:
 *
 *     tg_actors_step(&p, frames);                          the velocities, for the frames that went by
 *     tg_actor_move(&p, id, &world, dx, dy);               moves one, stopping at solid tiles
 *     tg_actors_draw(&p, &g, cam_x, cam_y);                gives sprite slots to the ones on screen
 *
 * tg_actors_draw takes sprite slots from g (tg_sprite_alloc) and gives them back at the next call; other sprites in g are left alone.
 */
#ifndef TG_ACTOR_H
#define TG_ACTOR_H

#include "tg_world.h"

#define TG_ACTOR_ACTIVE 1                      /* set by tg_actor_alloc, cleared by tg_actor_free */
#define TG_ACTOR_HIDDEN 2
#define TG_ACTOR_HFLIP 4                       /* mirrors the whole grid, and each tile */
#define TG_ACTOR_VFLIP 8

#define TG_HIT_LEFT 1                          /* tg_actor_move: the sides that stopped it */
#define TG_HIT_RIGHT 2
#define TG_HIT_UP 4
#define TG_HIT_DOWN 8

#define TG_ACTORS_YSORT 1                      /* tg_actors_t.flags: the lower on the screen, the nearer (prio is not used) */

typedef struct {                               /* a tile that stopped an actor, as it was then (the map may change after) */
    int32_t tx, ty;                            /* its tile coordinates (outside the map, those of the border tile that stopped it) */
    uint16_t tile;                             /* its tile number */
    uint8_t flags;                             /* its flags (TG_TILE_*) */
} tg_touch_t;

typedef struct {
    int32_t x, y;                              /* top left, global pixels, 24.8 fixed point (1/256 pixel) */
    int32_t vx, vy;                            /* per frame, 24.8; added by tg_actors_step */
    uint16_t tile;                             /* the first sprite tile; the others follow row by row: tile + row * cols + col */
    uint8_t cols, rows;                        /* 1..4 sprite tiles each way (a sprite tile is 8 or 16 pixels: g->sprite_tile_log2) */
    uint8_t flags;                             /* TG_ACTOR_* */
    uint8_t pal, above;                        /* as tg_sprite_t */
    uint8_t prio;                              /* 0 in front; actors with the same prio are in front in order of their number */
    uint8_t hit_x, hit_y, hit_w, hit_h;        /* the box for collisions, in pixels from the top left; hit_w or hit_h 0: the whole grid */
    uint16_t user;                             /* the program's: a type, a state, an animation counter */
    tg_touch_t touch[4];                       /* the last tile that stopped it on each side, set by tg_actor_move; [0] left, [1] right, [2] up, [3] down (the TG_HIT_* bits in order);
                                                  kept until the next stop on that side, all 0 for a new actor: the return of tg_actor_move says whether this move stopped */
} tg_actor_t;

typedef struct {
    tg_actor_t *a;                             /* the pool: n actors; an actor's number (id) is its index */
    uint16_t *order;                           /* scratch for tg_actors_draw: 4 * n entries (an actor can be seen twice across and twice down) */
    uint16_t n;                                /* at most 16383 */
    int32_t wrap_w, wrap_h;                    /* the world's size in pixels where it repeats, 0: it does not (tg_actors_set_wrap) */
    uint8_t flags;                             /* TG_ACTORS_* */
    uint8_t sprite_log2;                       /* the size of a sprite tile, as g->sprite_tile_log2; tg_actors_draw takes it from g */
    uint16_t hist[256];                        /* tg_actors_draw's work space */
    uint8_t used[TG_MAX_SPRITES];              /* the sprite slots taken at the last draw */
    uint8_t used_count;
} tg_actors_t;

/* The storage is the caller's: `storage` holds n actors and `order` 4 * n entries. */
void tg_actors_init(tg_actors_t *p, tg_actor_t *storage, uint16_t n, uint16_t *order);

/*
 * A world that repeats (TG_BORDER_WRAP, see tg_world_wrap_px), for infinite scrolling: an actor's position is kept in 0 .. size by
 * tg_actors_step and tg_actor_move, an actor near the seam is drawn on the side of the screen it is seen on (twice, if both its ends are
 * on screen), and tg_actors_overlap works across the seam. The width must be 192 pixels or more and 8388607 at most, the height 160 or
 * more (so that no actor is seen more than twice an axis); 0 turns an axis off. Returns false, and changes nothing, if a size is not allowed.
 */
bool tg_actors_set_wrap(tg_actors_t *p, int32_t width_px, int32_t height_px);
int tg_actor_alloc(tg_actors_t *p);            /* the lowest free number: a cleared actor, ACTIVE, 1 x 1; -1 if the pool is full */
void tg_actor_free(tg_actors_t *p, int id);
tg_actor_t *tg_actor(tg_actors_t *p, int id);  /* NULL for a number that is not an active actor */

/* x += vx * frames, y += vy * frames for every active actor (modulo the world's size if it wraps). */
void tg_actors_step(tg_actors_t *p, int frames);

/*
 * Move by (dx, dy) in 24.8, the x first and then the y, stopping flush against tiles that have TG_TILE_SOLID (the box of the actor is looked up
 * with tg_world_flags_box, so it also stops at the map's border if that is solid). TG_TILE_ONEWAY stops it only when moving down, from above.
 * An actor that stops loses the fraction of a pixel. Returns the TG_HIT_* sides that stopped it, and records in a->touch[] for each of them the tile
 * (coordinates, number and flags at that moment) that stopped it: the first one along the edge (the top one for left and right, the left one for up and down). An actor that starts inside a solid tile
 * is not moved out of it.
 */
uint8_t tg_actor_move(tg_actors_t *p, int id, const tg_world_t *w, int32_t dx, int32_t dy);

/* True if the collision boxes of two active actors overlap. */
bool tg_actors_overlap(const tg_actors_t *p, int a, int b);

/*
 * Give sprites to the active, not hidden actors that touch the screen (the camera is the global pixel at its top left), front-most first; one
 * sprite for each tile of the grid that is on screen. When the sprites run out, the back-most actors go without, and an actor whose tiles
 * do not all fit is left out. The actor's prio (or, with TG_ACTORS_YSORT, its bottom edge on the screen) becomes the sprite priority, so
 * it is also what other sprites are ordered against. Returns the number of actors drawn (one that is seen twice counts twice).
 */
int tg_actors_draw(tg_actors_t *p, tg_t *g, int32_t cam_x, int32_t cam_y);

#endif
