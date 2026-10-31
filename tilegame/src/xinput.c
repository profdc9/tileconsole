/* SPDX-License-Identifier: MIT */
#include "xinput.h"

bool xinput_decode(const uint8_t *r, size_t length, plat_buttons_t *out)
{
    if (!r || !out || length < 14 || r[0] != 0x00)   /* other message types (connection, headset...) are not input */
        return false;
    plat_buttons_t b = 0;
    const uint8_t lo = r[2], hi = r[3];
    if (lo & 0x01) b |= PLAT_BTN_UP;
    if (lo & 0x02) b |= PLAT_BTN_DOWN;
    if (lo & 0x04) b |= PLAT_BTN_LEFT;
    if (lo & 0x08) b |= PLAT_BTN_RIGHT;
    if (lo & 0x10) b |= PLAT_BTN_START;
    if (lo & 0x20) b |= PLAT_BTN_SELECT;
    if (hi & 0x01) b |= PLAT_BTN_L;
    if (hi & 0x02) b |= PLAT_BTN_R;
    if (hi & 0x10) b |= PLAT_BTN_A;   /* the buttons keep the names printed on the pad; a game decides what each one does */
    if (hi & 0x20) b |= PLAT_BTN_B;
    if (hi & 0x40) b |= PLAT_BTN_X;
    if (hi & 0x80) b |= PLAT_BTN_Y;
    const int16_t lx = (int16_t)(r[6] | (r[7] << 8)), ly = (int16_t)(r[8] | (r[9] << 8));
    if (lx < -XINPUT_STICK_THRESHOLD) b |= PLAT_BTN_LEFT;
    if (lx > XINPUT_STICK_THRESHOLD) b |= PLAT_BTN_RIGHT;
    if (ly > XINPUT_STICK_THRESHOLD) b |= PLAT_BTN_UP;      /* XInput has Y positive upwards */
    if (ly < -XINPUT_STICK_THRESHOLD) b |= PLAT_BTN_DOWN;
    *out = b;
    return true;
}
