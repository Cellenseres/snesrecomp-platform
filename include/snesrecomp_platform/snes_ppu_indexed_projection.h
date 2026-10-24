#ifndef SNESRECOMP_PLATFORM_SNES_PPU_INDEXED_PROJECTION_H
#define SNESRECOMP_PLATFORM_SNES_PPU_INDEXED_PROJECTION_H
#include "snesrecomp_platform/snes_ppu_job.h"
#ifdef __cplusplus
extern "C" {
#endif
/* A compact projection of an already resolved original source plane. Source
 * step zero repeats one texel; step one copies without filtering. Owner masks
 * are GPU inputs, not evaluated CPU pixel maps. Original colour/window context
 * is retained through source coordinates. Order follows the consumer layout. */
typedef struct SnesRecompIndexedProjectionRect {
    uint16_t x0,y0,x1,y1,source_x,source_y,owner,object;
    uint8_t plane,x_step,y_step,owner_required;
} SnesRecompIndexedProjectionRect;
SNESRECOMP_STATIC_ASSERT(sizeof(SnesRecompIndexedProjectionRect)==20,"indexed projection ABI");
typedef struct SnesRecompIndexedProjection SnesRecompIndexedProjection;
typedef struct SnesRecompIndexedProjectionPlan {
    const SnesRecompIndexedProjectionRect *rects;
    uint64_t generation;
    uint32_t count,reused;
} SnesRecompIndexedProjectionPlan;
/* Allocate once; preparation never allocates, reads pixels or runs the guest.
 * A warm layout ignores palette, VRAM, cursor character and OBJ-pixel changes.
 * Views live until the next preparation/reset/destruction. Refusal leaves the
 * previous storage intact and returns an empty output, never a partial frame. */
SnesRecompIndexedProjection *snesrecomp_ppu_indexed_projection_create(uint32_t capacity);
void snesrecomp_ppu_indexed_projection_destroy(SnesRecompIndexedProjection *cache);
void snesrecomp_ppu_indexed_projection_reset(SnesRecompIndexedProjection *cache);
SnesRecompPpuResult snesrecomp_ppu_indexed_projection_prepare(
    SnesRecompIndexedProjection *cache,const SnesRecompPpuJob *job,
    SnesRecompIndexedProjectionPlan *out);
/* Intersect with an original tile/sliver/material rectangle, preserving exact
 * repeated texels. This produces constant-priority GPU pieces without a CPU
 * visibility image. Returns 1 for a piece, 0 for no intersection, -1 invalid;
 * refusal/empty leaves output unchanged. In-place clipping is supported. */
int32_t snesrecomp_ppu_indexed_projection_clip(const SnesRecompIndexedProjectionRect *rect,
    uint16_t source_x0,uint16_t source_y0,uint16_t source_x1,uint16_t source_y1,
    SnesRecompIndexedProjectionRect *out);
#ifdef __cplusplus
}
#endif
#endif
