#ifndef SNESRECOMP_PLATFORM_SNES_PPU_INDEXED_DRAW_H
#define SNESRECOMP_PLATFORM_SNES_PPU_INDEXED_DRAW_H
#include "snesrecomp_platform/snes_ppu_indexed_tiles.h"
#include "snesrecomp_platform/snes_ppu_indexed_projection.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Projection is intersected with the ORIGINAL BG tile or selected OBJ run,
 * and with raster materials. The GPU tests its index/winner/owner; this is
 * geometry only, never a CPU visibility or colour image. OBJ winner is the
 * original OAM slot, whereas rect.owner remains a consumer association. */
typedef struct SnesRecompIndexedDraw {
    SnesRecompIndexedProjectionRect rect;
    uint16_t priority,material,winner;
    uint8_t index,reserved;
} SnesRecompIndexedDraw;
SNESRECOMP_STATIC_ASSERT(sizeof(SnesRecompIndexedDraw)==28,"indexed draw ABI");
typedef struct SnesRecompIndexedDrawCache SnesRecompIndexedDrawCache;
typedef struct SnesRecompIndexedDrawPlan {
    const SnesRecompIndexedDraw *draws;
    uint64_t generation;
    uint32_t count,reused,candidates_examined;
    uint32_t first[5],counts[5];
    /* Exact content versions for each owned plane. A cursor/OBJ change must
     * not invalidate unchanged BG drawing geometry. Local to this cache. */
    uint64_t plane_generation[5];
    uint32_t background_reused;
} SnesRecompIndexedDrawPlan;
/* Allocate bounded storage once. Preparation never allocates, evaluates OAM,
 * reads pixel planes or executes guest code. Scene/projection views must come
 * from the same owned job and remain unchanged throughout preparation.
 * Output is double buffered; refusal leaves the last successful plan intact.
 * Plane grouping preserves object order and complete native Z within planes.
 * Different planes have disjoint original depth encodings. */
SnesRecompIndexedDrawCache *snesrecomp_ppu_indexed_draw_create(uint32_t capacity);
void snesrecomp_ppu_indexed_draw_destroy(SnesRecompIndexedDrawCache *cache);
void snesrecomp_ppu_indexed_draw_reset(SnesRecompIndexedDrawCache *cache);
SnesRecompPpuResult snesrecomp_ppu_indexed_draw_prepare(
    SnesRecompIndexedDrawCache *cache,const SnesRecompPpuJob *job,
    const SnesRecompIndexedScenePlan *scene,const SnesRecompIndexedProjectionPlan *projection,
    SnesRecompIndexedDrawPlan *out);
/* Bit m certifies that referenced SOURCE_COLOUR material m is exactly its
 * selected source palette colour. This conservative proof requires no own
 * colour math, no enabled main windows, unrestricted RGB, original enabled
 * source layers and complete source bounds. It never changes materials or
 * omits guest work. Other materials and indices >=32 have no certificate.
 * The result belongs to this immutable job, not to a later frame. */
uint32_t snesrecomp_ppu_indexed_palette_equivalent(const SnesRecompPpuJob *job);
#ifdef __cplusplus
}
#endif
#endif
