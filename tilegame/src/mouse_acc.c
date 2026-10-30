/* SPDX-License-Identifier: MIT */
#include "mouse_acc.h"

#include <string.h>

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

void mouse_acc_init(mouse_acc_t *m)
{
    memset(m, 0, sizeof *m);
    m->x = TG_WIDTH / 2;
    m->y = TG_HEIGHT / 2;
}

void mouse_acc_move(mouse_acc_t *m, int dx, int dy, int wheel)
{
    m->present = true;
    m->acc_dx = clamp32(m->acc_dx + dx, -32767, 32767);
    m->acc_dy = clamp32(m->acc_dy + dy, -32767, 32767);
    m->acc_wheel = clamp32(m->acc_wheel + wheel, -127, 127);
}

void mouse_acc_buttons(mouse_acc_t *m, uint8_t buttons)
{
    m->present = true;
    m->buttons = buttons & 7;
}

void mouse_acc_set_position(mouse_acc_t *m, int x, int y)
{
    m->x = clamp32(x, 0, TG_WIDTH - 1);
    m->y = clamp32(y, 0, TG_HEIGHT - 1);
}

void mouse_acc_frame(mouse_acc_t *m)
{
    m->x = clamp32(m->x + m->acc_dx, 0, TG_WIDTH - 1);
    m->y = clamp32(m->y + m->acc_dy, 0, TG_HEIGHT - 1);
    m->frame = (plat_mouse_t){.present = m->present,
                              .x = (int16_t)m->x, .y = (int16_t)m->y,
                              .dx = (int16_t)m->acc_dx, .dy = (int16_t)m->acc_dy, .wheel = (int8_t)m->acc_wheel,
                              .buttons = m->buttons,
                              .pressed = (uint8_t)(m->buttons & ~m->prev_buttons),
                              .released = (uint8_t)(m->prev_buttons & ~m->buttons)};
    m->prev_buttons = m->buttons;
    m->acc_dx = m->acc_dy = m->acc_wheel = 0;
}
