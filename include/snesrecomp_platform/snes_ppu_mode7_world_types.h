#ifndef SNESRECOMP_PLATFORM_SNES_PPU_MODE7_WORLD_TYPES_H
#define SNESRECOMP_PLATFORM_SNES_PPU_MODE7_WORLD_TYPES_H
#include <stdint.h>
#include "snesrecomp_platform/abi.h"

/* Fixed-width fields only, including across console SDK enum ABIs. */
typedef struct SnesRecompMode7WorldSpan {
    uint16_t x0, x1, y;
    uint8_t variant, solid;
    int32_t start_x, start_y, step_x, step_y; /* 8.8, at first pixel centre. */
    uint32_t argb; /* Shaded solid RGB; raw RGB5 plans clip to black instead. */
} SnesRecompMode7WorldSpan;
SNESRECOMP_STATIC_ASSERT(sizeof(SnesRecompMode7WorldSpan) == 28, "Mode 7 span SDK boundary");
#endif
