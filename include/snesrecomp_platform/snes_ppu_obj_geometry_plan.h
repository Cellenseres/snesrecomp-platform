#ifndef SNESRECOMP_PLATFORM_SNES_PPU_OBJ_GEOMETRY_PLAN_H
#define SNESRECOMP_PLATFORM_SNES_PPU_OBJ_GEOMETRY_PLAN_H
#include "snesrecomp_platform/snes_ppu_obj_run.h"
#include "snesrecomp_platform/snes_ppu_frame.h"

/* Qualified selection is an immutable input. Geometry generation neither
 * selects sprites nor reads pixels, palettes, OAM or live guest state. */
typedef struct SnesRecompPpuObjGeometryQuad {
    int16_t x0,x1;
    uint16_t line,tile;
    uint8_t u0,u1,row,palette_base,priority,math_eligible,winner,reserved;
    uint32_t run;
} SnesRecompPpuObjGeometryQuad;
SNESRECOMP_STATIC_ASSERT(sizeof(SnesRecompPpuObjGeometryQuad)==20,"OBJ geometry ABI");

/* Canvas-space final visibility, AFTER the original OBJ winner is resolved.
 * Hiding a winning sprite must not reveal another sprite underneath it.
 * NULL permits the whole canvas. Windows/main/sub remain backend materials. */
typedef struct SnesRecompPpuObjGeometryClip { int16_t x0,x1; } SnesRecompPpuObjGeometryClip;
SNESRECOMP_STATIC_ASSERT(sizeof(SnesRecompPpuObjGeometryClip)==4,"OBJ visibility ABI");
typedef struct SnesRecompPpuObjGeometryPlan {
    uint32_t winner_count,colour_count;
    uint16_t width,height;
    uint8_t winner_groups,reserved[3];
} SnesRecompPpuObjGeometryPlan;

#ifdef __cplusplus
extern "C" {
#endif
/* Two transactionally preflighted streams: original overwrite order for a
 * stencil/winner pass; final visible fragments for ordinary BG interleave.
 * The winner key is the row-local accepted-sprite ordinal, 1..128. Grouping
 * original-stream quads by ascending winner is safe even with OAM rotation
 * changing between rows. It does not reorder columns inside a sprite.
 *
 * A slot must be contiguous within its row, with disjoint increasing columns.
 * These are actual tile-fetch invariants, not assumptions about arbitrary
 * synthetic sliver lists. Violations refuse before changing either stream or
 * the output plan. Output/input arrays must not overlap. No allocation. */
SnesRecompPpuResult snesrecomp_ppu_obj_geometry_plan(
    const SnesRecompPpuObjRun *runs,uint32_t count,
    const SnesRecompPpuObjGeometryClip *clips,
    uint16_t width,uint16_t height,uint16_t extra,
    SnesRecompPpuObjGeometryQuad *winners,uint32_t winner_capacity,
    SnesRecompPpuObjGeometryQuad *colours,uint32_t colour_capacity,
    SnesRecompPpuObjGeometryPlan *out);
#ifdef __cplusplus
}
#endif
#endif
