#ifndef SNESRECOMP_PLATFORM_SNES_PPU_TILE_TYPES_H
#define SNESRECOMP_PLATFORM_SNES_PPU_TILE_TYPES_H
#include <stdint.h>
#include "snesrecomp_platform/abi.h"

/* Fixed-layout SDK boundary; no enum, pointer ownership or SDK types. */
typedef struct SnesRecompTileQuad {
    int16_t x0, y0, x1, y1;
    uint16_t slot;
    uint8_t u0, v0, u1, v1;
    uint8_t pass, reserved;
} SnesRecompTileQuad;
typedef struct SnesRecompTileBackdrop {
    uint16_t x0, y0, x1, y1;
    uint32_t argb;
} SnesRecompTileBackdrop;
SNESRECOMP_STATIC_ASSERT(sizeof(SnesRecompTileQuad) == 16, "tile quad ABI");
SNESRECOMP_STATIC_ASSERT(sizeof(SnesRecompTileBackdrop) == 12, "tile backdrop ABI");
/* Raw Mode-1 geometry uses separate stable main/material and sub buckets.
 * Main pass = native priority rung * 2 + colour-math eligibility (0..13).
 * Sub pass = 14 + native priority rung (14..20). Reserved remains zero.
 * Ordinary finished-colour plans continue to use their seven native rungs. */
#define SNESRECOMP_RAW_TILE_MAIN_PASSES 14u
#define SNESRECOMP_RAW_TILE_PASSES 21u
#endif
