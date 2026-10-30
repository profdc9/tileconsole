/* SPDX-License-Identifier: MIT */
/*
 * What both backends need to turn a stream of mouse events into plat_mouse(): movement and wheel add up between frames,
 * buttons give pressed and released edges, and a pointer position follows the movement and stays on the screen.
 */
#ifndef MOUSE_ACC_H
#define MOUSE_ACC_H

#include <stdbool.h>
#include <stdint.h>

#include "platform.h"

typedef struct {
    int32_t acc_dx, acc_dy, acc_wheel;   /* since the last frame */
    uint8_t buttons;                     /* held now */
    uint8_t prev_buttons;
    bool present;
    int32_t x, y;
    plat_mouse_t frame;                  /* the snapshot taken at the last frame */
} mouse_acc_t;

void mouse_acc_init(mouse_acc_t *m);
void mouse_acc_move(mouse_acc_t *m, int dx, int dy, int wheel);   /* marks a mouse as present */
void mouse_acc_buttons(mouse_acc_t *m, uint8_t buttons);          /* the buttons held now; marks a mouse as present */
void mouse_acc_set_position(mouse_acc_t *m, int x, int y);
void mouse_acc_frame(mouse_acc_t *m);                              /* take the snapshot for this frame and start adding up again */

#endif
