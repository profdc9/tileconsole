/* SPDX-License-Identifier: MIT */
#include "hid_pad.h"

#include <string.h>

#define MAX_USAGES 64
#define MAX_IDS 8
#define MAX_RECORDED 128
#define MAX_GLOBAL_STACK 4

typedef struct {
    uint32_t page;
    int32_t lmin, lmax_signed;
    uint32_t lmax_unsigned;
    uint32_t size, count;
    uint8_t id;
} globals_t;

typedef struct {
    hid_field_t f;
    uint8_t slot;   /* which report ID */
} recorded_t;

/* The kind of field a usage is, for the ones a gamepad uses. */
static unsigned classify(uint32_t page, uint32_t usage, uint8_t *index)
{
    *index = 0;
    if (page == 0x09) {   /* Button */
        *index = usage > 255 ? 255 : (uint8_t)usage;
        return usage ? HF_BUTTON : 0;
    }
    if (page == 0x01) {   /* Generic Desktop */
        switch (usage) {
        case 0x30: return HF_AXIS_X;
        case 0x31: return HF_AXIS_Y;
        case 0x39: return HF_HAT;
        case 0x90: return HF_DPAD_UP;
        case 0x91: return HF_DPAD_DOWN;
        case 0x92: return HF_DPAD_RIGHT;
        case 0x93: return HF_DPAD_LEFT;
        default: return 0;
        }
    }
    return 0;
}

static int32_t sign_extend(uint32_t value, unsigned bytes)
{
    switch (bytes) {
    case 1: return (int8_t)value;
    case 2: return (int16_t)value;
    case 4: return (int32_t)value;
    default: return 0;
    }
}

bool hid_pad_parse_descriptor(const uint8_t *d, size_t len, hid_pad_t *out)
{
    if (!d || !out)
        return false;

    globals_t g;
    memset(&g, 0, sizeof g);
    globals_t stack[MAX_GLOBAL_STACK];
    unsigned sp = 0;

    uint32_t usages[MAX_USAGES];
    unsigned nusages = 0;
    bool have_range = false;
    uint32_t umin = 0, umax = 0;

    static recorded_t rec[MAX_RECORDED];   /* static: this can run on a small stack */
    unsigned nrec = 0;

    uint8_t ids[MAX_IDS] = {0};
    uint32_t id_bits[MAX_IDS] = {0};
    unsigned nids = 1;                      /* slot 0 is "no report ID" */
    bool uses_ids = false;

    unsigned depth = 0;
    bool in_pad = false;                    /* inside the top-level collection of a gamepad or joystick */

    size_t i = 0;
    while (i < len) {
        const uint8_t prefix = d[i++];
        if (prefix == 0xFE) {               /* a long item: skip it */
            if (i + 2 > len)
                return false;
            i += 2 + (size_t)d[i];
            if (i > len)
                return false;
            continue;
        }
        unsigned size = prefix & 3;
        if (size == 3)
            size = 4;
        const unsigned type = (prefix >> 2) & 3, tag = prefix >> 4;
        if (i + size > len)
            return false;
        uint32_t u = 0;
        for (unsigned k = 0; k < size; k++)
            u |= (uint32_t)d[i + k] << (8 * k);
        const int32_t s = sign_extend(u, size);
        i += size;

        if (type == 1) {                    /* global */
            switch (tag) {
            case 0: g.page = u & 0xFFFF; break;
            case 1: g.lmin = s; break;
            case 2: g.lmax_signed = s; g.lmax_unsigned = u; break;
            case 7: g.size = u; break;
            case 8:
                g.id = (uint8_t)u;
                uses_ids = true;
                break;
            case 9: g.count = u; break;
            case 0xA:
                if (sp < MAX_GLOBAL_STACK)
                    stack[sp++] = g;
                break;
            case 0xB:
                if (sp > 0)
                    g = stack[--sp];
                break;
            default: break;
            }
        } else if (type == 2) {             /* local */
            switch (tag) {
            case 0:
                if (nusages < MAX_USAGES)
                    usages[nusages++] = (size == 4) ? u : (u & 0xFFFF);
                break;
            case 1: umin = u; have_range = true; break;
            case 2: umax = u; have_range = true; break;
            default: break;
            }
        } else if (type == 0) {             /* main */
            if (tag == 0xA) {               /* collection */
                if (depth == 0) {
                    uint32_t page = g.page, usage = nusages ? usages[0] : (have_range ? umin : 0);
                    if (usage >> 16) {
                        page = usage >> 16;
                        usage &= 0xFFFF;
                    }
                    in_pad = u == 1 && page == 0x01 && (usage == 0x04 || usage == 0x05 || usage == 0x08);
                }
                depth++;
            } else if (tag == 0xC) {        /* end collection */
                if (depth > 0)
                    depth--;
                if (depth == 0)
                    in_pad = false;
            } else if (tag == 8) {          /* input */
                unsigned slot = 0;
                if (uses_ids) {
                    slot = MAX_IDS;
                    for (unsigned k = 1; k < nids; k++)
                        if (ids[k] == g.id)
                            slot = k;
                    if (slot == MAX_IDS && nids < MAX_IDS) {
                        ids[nids] = g.id;
                        slot = nids++;
                    }
                }
                if (slot < MAX_IDS && g.size > 0 && g.size <= 32 && g.count > 0 && g.count <= 4096) {
                    const bool constant = (u & 1) != 0, variable = (u & 2) != 0;
                    if (!constant && variable && in_pad) {
                        for (uint32_t k = 0; k < g.count && nrec < MAX_RECORDED; k++) {
                            uint32_t usage = 0;
                            if (nusages)
                                usage = usages[k < nusages ? k : nusages - 1];
                            else if (have_range)
                                usage = (umin + k > umax) ? umax : umin + k;
                            uint32_t page = g.page;
                            if (usage >> 16) {
                                page = usage >> 16;
                                usage &= 0xFFFF;
                            }
                            uint8_t index;
                            const unsigned kind = classify(page, usage, &index);
                            const uint32_t bit = id_bits[slot] + k * g.size;
                            if (kind && bit + g.size <= 0xFFFF) {
                                int32_t max = g.lmax_signed;
                                if (g.lmin >= 0 && g.lmax_unsigned > (uint32_t)INT32_MAX)
                                    max = INT32_MAX;
                                else if (g.lmin >= 0)
                                    max = (int32_t)g.lmax_unsigned;
                                rec[nrec].f = (hid_field_t){.bit_offset = (uint16_t)bit, .bit_size = (uint8_t)g.size,
                                                            .kind = (uint8_t)kind, .index = index, .min = g.lmin, .max = max};
                                rec[nrec].slot = (uint8_t)slot;
                                nrec++;
                            }
                        }
                    }
                    if ((uint64_t)id_bits[slot] + (uint64_t)g.count * g.size < 0x100000)
                        id_bits[slot] += g.count * g.size;
                }
            }
            nusages = 0;                    /* local items last until the next main item */
            have_range = false;
        }
    }

    /* Choose the report ID with the most pad fields: that is the pad's main report. */
    unsigned best = MAX_IDS, best_count = 0;
    for (unsigned slot = 0; slot < nids; slot++) {
        unsigned n = 0;
        for (unsigned k = 0; k < nrec; k++)
            n += rec[k].slot == slot;
        if (n > best_count) {
            best = slot;
            best_count = n;
        }
    }
    if (best == MAX_IDS)
        return false;

    hid_pad_t pad;
    memset(&pad, 0, sizeof pad);
    pad.has_id = uses_ids;
    pad.report_id = ids[best];
    bool buttons = false, hat = false, ax = false, ay = false, dpad = false;
    for (unsigned k = 0; k < nrec && pad.nfields < HID_PAD_MAX_FIELDS; k++) {
        if (rec[k].slot != best)
            continue;
        pad.f[pad.nfields++] = rec[k].f;
        switch (rec[k].f.kind) {
        case HF_BUTTON: buttons = true; break;
        case HF_HAT: hat = true; break;
        case HF_AXIS_X: ax = true; break;
        case HF_AXIS_Y: ay = true; break;
        default: dpad = true; break;
        }
    }
    if (!(buttons || hat || (ax && ay) || dpad))
        return false;
    *out = pad;
    return true;
}

static uint32_t get_bits(const uint8_t *data, unsigned offset, unsigned size)
{
    uint32_t v = 0;
    for (unsigned k = 0; k < size; k++)
        v |= (uint32_t)((data[(offset + k) >> 3] >> ((offset + k) & 7)) & 1u) << k;
    return v;
}

bool hid_pad_decode(const hid_pad_t *pad, const uint8_t *report, size_t length, hid_pad_state_t *out)
{
    if (!pad || !report || !out)
        return false;
    const uint8_t *data = report;
    size_t n = length;
    if (pad->has_id) {
        if (length < 1 || report[0] != pad->report_id)
            return false;
        data++;
        n--;
    }
    if (n == 0)
        return false;

    hid_pad_state_t st = {0, 0};
    for (unsigned k = 0; k < pad->nfields; k++) {
        const hid_field_t *f = &pad->f[k];
        if ((size_t)f->bit_offset + f->bit_size > n * 8)
            continue;   /* beyond the end of a short report */
        uint32_t raw = get_bits(data, f->bit_offset, f->bit_size);
        int64_t v = raw;
        if (f->min < 0 && f->bit_size < 32 && (raw >> (f->bit_size - 1)) & 1u)
            v = (int64_t)raw - ((int64_t)1 << f->bit_size);        /* a signed field */
        else if (f->min < 0 && f->bit_size == 32)
            v = (int32_t)raw;

        switch (f->kind) {
        case HF_BUTTON:
            if (v != 0 && f->index >= 1 && f->index <= 32)
                st.buttons |= 1u << (f->index - 1);
            break;
        case HF_DPAD_UP:    if (v) st.dpad |= HID_DPAD_UP; break;
        case HF_DPAD_DOWN:  if (v) st.dpad |= HID_DPAD_DOWN; break;
        case HF_DPAD_LEFT:  if (v) st.dpad |= HID_DPAD_LEFT; break;
        case HF_DPAD_RIGHT: if (v) st.dpad |= HID_DPAD_RIGHT; break;
        case HF_AXIS_X:
        case HF_AXIS_Y: {
            const int64_t range = (int64_t)f->max - f->min;
            if (range <= 0)
                break;
            const int64_t lo = f->min + range * 3 / 10, hi = f->max - range * 3 / 10;   /* the stick has to go well off centre */
            const uint8_t neg = f->kind == HF_AXIS_X ? HID_DPAD_LEFT : HID_DPAD_UP;
            const uint8_t pos = f->kind == HF_AXIS_X ? HID_DPAD_RIGHT : HID_DPAD_DOWN;
            if (v <= lo)
                st.dpad |= neg;
            else if (v >= hi)
                st.dpad |= pos;
            break;
        }
        case HF_HAT: {
            if (v < f->min || v > f->max)
                break;                                              /* the "nothing pressed" value is outside the range */
            const int64_t range = (int64_t)f->max - f->min + 1, idx = v - f->min;
            static const uint8_t eight[8] = {HID_DPAD_UP, HID_DPAD_UP | HID_DPAD_RIGHT, HID_DPAD_RIGHT, HID_DPAD_DOWN | HID_DPAD_RIGHT,
                                             HID_DPAD_DOWN, HID_DPAD_DOWN | HID_DPAD_LEFT, HID_DPAD_LEFT, HID_DPAD_UP | HID_DPAD_LEFT};
            static const uint8_t four[4] = {HID_DPAD_UP, HID_DPAD_RIGHT, HID_DPAD_DOWN, HID_DPAD_LEFT};
            if (range == 8)
                st.dpad |= eight[idx];
            else if (range == 4)
                st.dpad |= four[idx];
            break;
        }
        default:
            break;
        }
    }
    *out = st;
    return true;
}

/* ---- the buttons ---- */

static const hid_pad_map_t default_map = {.a = 1, .b = 2, .x = 3, .y = 4, .l = 5, .r = 6, .select = 7, .start = 8};

/* Sony pads number their buttons Square, Cross, Circle, Triangle, L1, R1, L2, R2, Share, Options: the bottom button, Cross, is 2.
 * (From the usual numbering of these pads' HID reports; not tried on the hardware here.) */
static const hid_pad_map_t sony_map = {.a = 2, .b = 3, .x = 1, .y = 4, .l = 5, .r = 6, .select = 9, .start = 10};

static const struct {
    uint16_t vid, pid;
    const hid_pad_map_t *map;
} known[] = {
    {0x054C, 0x05C4, &sony_map},   /* DualShock 4 */
    {0x054C, 0x09CC, &sony_map},   /* DualShock 4 (second version) */
    {0x054C, 0x0CE6, &sony_map},   /* DualSense */
};

const hid_pad_map_t *hid_pad_map_for(uint16_t vendor_id, uint16_t product_id)
{
    for (size_t i = 0; i < sizeof known / sizeof *known; i++)
        if (known[i].vid == vendor_id && known[i].pid == product_id)
            return known[i].map;
    return &default_map;
}

plat_buttons_t hid_pad_to_buttons(const hid_pad_state_t *st, const hid_pad_map_t *map)
{
    plat_buttons_t b = 0;
    if (st->dpad & HID_DPAD_UP) b |= PLAT_BTN_UP;
    if (st->dpad & HID_DPAD_DOWN) b |= PLAT_BTN_DOWN;
    if (st->dpad & HID_DPAD_LEFT) b |= PLAT_BTN_LEFT;
    if (st->dpad & HID_DPAD_RIGHT) b |= PLAT_BTN_RIGHT;
#define PICK(field, flag) if (map->field && map->field <= 32 && (st->buttons >> (map->field - 1) & 1u)) b |= flag
    PICK(a, PLAT_BTN_A);
    PICK(b, PLAT_BTN_B);
    PICK(x, PLAT_BTN_X);
    PICK(y, PLAT_BTN_Y);
    PICK(l, PLAT_BTN_L);
    PICK(r, PLAT_BTN_R);
    PICK(select, PLAT_BTN_SELECT);
    PICK(start, PLAT_BTN_START);
#undef PICK
    return b;
}
