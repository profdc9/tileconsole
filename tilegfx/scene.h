/* SPDX-License-Identifier: MIT */
#ifndef SCENE_H
#define SCENE_H

#include "tilegfx.h"

/* Build the palette, tile graphics and maps, and point the layers at them. */
void scene_setup(tg_t *g);

/* Move the layers and sprites for frame `frame`; `scroll` is the foreground x position in pixels. */
void scene_animate(tg_t *g, int frame, int scroll, bool use_wave);

#endif
