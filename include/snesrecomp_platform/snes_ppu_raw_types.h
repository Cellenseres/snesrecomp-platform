#ifndef SNESRECOMP_PLATFORM_SNES_PPU_RAW_TYPES_H
#define SNESRECOMP_PLATFORM_SNES_PPU_RAW_TYPES_H
#include <stdint.h>
#include "snesrecomp_platform/abi.h"
/* Fixed-width materials shared with SDK adapters. RGB5 uses 8*n components. */
#define SNESRECOMP_RAW_MAIN_RGB 1u
#define SNESRECOMP_RAW_HALF 2u
#define SNESRECOMP_RAW_SUBTRACT 4u
#define SNESRECOMP_RAW_OBJ_MAIN 8u
#define SNESRECOMP_RAW_OBJ_SUB 16u
#define SNESRECOMP_RAW_OBJ_MATH 32u
enum { SNESRECOMP_RAW_REGION_LIMIT=128 };
typedef struct SnesRecompPpuRawRegion {
    uint16_t x0,y0,x1,y1;
    uint32_t main_backdrop,sub_backdrop;
    /* 15 minus INIDISP brightness. Zero preserves full-brightness callers. */
    uint8_t flags,brightness_reduction,reserved[2];
} SnesRecompPpuRawRegion;
SNESRECOMP_STATIC_ASSERT(sizeof(SnesRecompPpuRawRegion)==20,"raw region SDK boundary");
#endif
