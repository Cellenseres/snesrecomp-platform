#include "snesrecomp_platform/snes_obj_continuity.h"

#include <string.h>

/* vhoopppN, without the name table bit: that belongs to the tile. */
enum { SNES_OBJ_ATTR_SIGNATURE = 0xfe };

static int ShortestStep(unsigned from, unsigned to, unsigned modulus) {
    int delta = (int)((to - from) & (modulus - 1u));
    if (delta > (int)(modulus / 2u))
        delta -= (int)modulus;
    return delta;
}

int SnesObjDeltaX(unsigned from, unsigned to) {
    return ShortestStep(from, to, SNES_OBJ_X_MODULUS);
}

int SnesObjDeltaY(unsigned from, unsigned to) {
    return ShortestStep(from, to, SNES_OBJ_Y_MODULUS);
}

unsigned SnesObjPositionAt(unsigned from, int delta, float alpha,
                           unsigned modulus) {
    if (alpha <= 0.0f)
        return from & (modulus - 1u);
    if (alpha >= 1.0f)
        alpha = 1.0f;

    const float step = (float)delta * alpha;
    const int rounded = (int)(step >= 0.0f ? step + 0.5f : step - 0.5f);
    return (unsigned)(((int)from + rounded) & (int)(modulus - 1u));
}

bool SnesObjSlotSafe(const SnesObjInterpolation *interp, unsigned slot) {
    if (!interp || slot >= SNES_OBJ_SLOTS)
        return false;
    return (interp->safe[slot >> 3] & (1u << (slot & 7))) != 0;
}

static unsigned DecodeX(const uint16_t *oam, const uint8_t *high,
                        unsigned index) {
    return (oam[index] & 0xffu) |
           (unsigned)(((high[index >> 3] >> (index & 7u)) & 1u) << 8);
}

static unsigned DecodeSize(const uint8_t *high, unsigned index) {
    return (high[index >> 3] >> ((index & 7u) + 1u)) & 1u;
}

void SnesObjCompare(const uint16_t *prev_oam, const uint8_t *prev_high,
                    const uint16_t *cur_oam, const uint8_t *cur_high,
                    unsigned max_step_px, SnesObjInterpolation *out) {
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    if (!prev_oam || !prev_high || !cur_oam || !cur_high)
        return;

    for (unsigned slot = 0; slot < SNES_OBJ_SLOTS; slot++) {
        const unsigned index = slot * 2u;

        if (DecodeSize(prev_high, index) != DecodeSize(cur_high, index))
            continue;
        const unsigned prev_attr = (prev_oam[index + 1u] >> 8) &
                                   SNES_OBJ_ATTR_SIGNATURE;
        const unsigned cur_attr = (cur_oam[index + 1u] >> 8) &
                                  SNES_OBJ_ATTR_SIGNATURE;
        if (prev_attr != cur_attr)
            continue;

        const int dx = SnesObjDeltaX(DecodeX(prev_oam, prev_high, index),
                                     DecodeX(cur_oam, cur_high, index));
        const int dy = SnesObjDeltaY(prev_oam[index] >> 8,
                                     cur_oam[index] >> 8);
        const unsigned reach_x = (unsigned)(dx < 0 ? -dx : dx);
        const unsigned reach_y = (unsigned)(dy < 0 ? -dy : dy);
        if (reach_x > max_step_px || reach_y > max_step_px)
            continue;

        out->dx[slot] = (int16_t)dx;
        out->dy[slot] = (int16_t)dy;
        out->safe[slot >> 3] |= (uint8_t)(1u << (slot & 7));
        out->matched++;
    }
}
