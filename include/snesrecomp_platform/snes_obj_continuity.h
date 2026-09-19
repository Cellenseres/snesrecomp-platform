#ifndef SNESRECOMP_PLATFORM_SNES_OBJ_CONTINUITY_H
#define SNESRECOMP_PLATFORM_SNES_OBJ_CONTINUITY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SNES_OBJ_SLOTS = 128,
    SNES_OBJ_X_MODULUS = 512,
    SNES_OBJ_Y_MODULUS = 256,
};

typedef struct SnesObjInterpolation {
    /* Bit per slot: this sprite may be moved between the two frames. */
    uint8_t safe[SNES_OBJ_SLOTS / 8];
    int16_t dx[SNES_OBJ_SLOTS];
    int16_t dy[SNES_OBJ_SLOTS];
    unsigned matched;
} SnesObjInterpolation;

/* Shortest signed step; X is 9 bits, Y is 8. */
int SnesObjDeltaX(unsigned from, unsigned to);
int SnesObjDeltaY(unsigned from, unsigned to);

/* Position at alpha, wrapped back into range. */
unsigned SnesObjPositionAt(unsigned from, int delta, float alpha,
                           unsigned modulus);

bool SnesObjSlotSafe(const SnesObjInterpolation *interp, unsigned slot);

/* A slot is not an entity id; match on size, palette,
   priority, flip and a plausible step. The tile may animate. */
void SnesObjCompare(const uint16_t *prev_oam, const uint8_t *prev_high,
                    const uint16_t *cur_oam, const uint8_t *cur_high,
                    unsigned max_step_px, SnesObjInterpolation *out);

#ifdef __cplusplus
}
#endif

#endif
