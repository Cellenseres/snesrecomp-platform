#ifndef SNESRECOMP_PLATFORM_SNES_PPU_INDEXED_TILES_H
#define SNESRECOMP_PLATFORM_SNES_PPU_INDEXED_TILES_H

#include "snesrecomp_platform/snes_ppu_tile_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnesRecompIndexedTileRequest {
    uint16_t word;
    uint8_t bpp, reserved;
} SnesRecompIndexedTileRequest;
SNESRECOMP_STATIC_ASSERT(sizeof(SnesRecompIndexedTileRequest)==4,"indexed tile request ABI");

/* A distinct view prevents treating palette indices as finished colours.
 * Each texel is ARGB with R=A=the low 2/4-bit index, G=B=0. Transparent
 * index zero remains zero. Palette family, priority and owner belong to the
 * geometry, so palette animation never changes these atlas contents.
 * Only slots listed here have this encoding. Other resident cache slots
 * can belong to an ordinary colour plan and must not be uploaded as indices.
 * Data stays cache-owned until its next preparation, reset or destruction.
 * A backend copies dirty resources to a fenced slot before releasing it. */
typedef struct SnesRecompIndexedTilePlan {
    const uint16_t *slots; /* One slot per request, including duplicates. */
    const uint32_t *tiles;
    const uint64_t *generations;
    const uint8_t *dirty;
    uint32_t count, tiles_decoded, cache_hits;
    /* Nonzero index classes used by each listed slot; bit zero is excluded.
     * Decode misses compute these once. Backends skip absent class geometry. */
    const uint16_t *classes;
} SnesRecompIndexedTilePlan;

/* Preparation reads only the immutable frame's owned VRAM. No palette,
 * pixel-plane, sprite selection or guest execution is involved. Inputs are
 * preflighted before cache mutation. Failure exposes no executable plan.
 * The bounded batch shares the existing persistent tile cache and version
 * tracking; colour/index encodings cannot alias even for black palettes.
 * Contiguous 2/4-bpp tiles are supported; wrapped source blocks are rejected.
 * This prepares atlas data, not a finished frame or backend admission. */
SnesRecompPpuResult snesrecomp_ppu_indexed_tiles(
    SnesRecompColourTileCache *cache, const SnesRecompPpuFrame *frame,
    const SnesRecompIndexedTileRequest *requests, uint32_t count,
    SnesRecompIndexedTilePlan *out);

enum {
    SNESRECOMP_INDEXED_RGB_VISIBLE=1,
    SNESRECOMP_INDEXED_COLOUR_MATH=2
};
/* Original BG geometry, before consumer relocation/ownership. screens is
 * one (main) or two (sub); priority is the original BG low/high bit.
 * Complete native depth combines the consumer's layer/priority encoding,
 * palette_base and the nonzero atlas index. Material colour and windows
 * follow these original canvas coordinates. All fields are fixed-width. */
typedef struct SnesRecompIndexedTileQuad {
    int16_t x0,y0,x1,y1;
    uint16_t slot;
    uint8_t u0,v0,u1,v1;
    uint8_t palette_base,layer,screens,priority,flags,reserved;
} SnesRecompIndexedTileQuad;
SNESRECOMP_STATIC_ASSERT(sizeof(SnesRecompIndexedTileQuad)==20,"indexed BG quad ABI");
typedef struct SnesRecompIndexedBackgroundPlan {
    const SnesRecompIndexedTileQuad *quads;
    const uint32_t *tiles;
    const uint64_t *generations;
    const uint8_t *dirty;
    uint64_t geometry_generation;
    uint32_t quad_count,tiles_decoded,cache_hits,geometry_reused,unsupported;
    uint16_t width,height;
    const uint16_t *classes;
} SnesRecompIndexedBackgroundPlan;

/* Reuse the existing Mode-1 raster/window/tilemap projection, but generate
 * only index tiles and compact BG quads. Palette changes never decode tiles
 * or split/rebuild this geometry. The consumer retains the immutable frame
 * for palette/material histories. No OBJ work or finished CPU image.
 * This shares the ordinary planner's conservative unsupported-state checks;
 * backend effect/resource admission is separate and still mandatory. */
SnesRecompPpuResult snesrecomp_ppu_mode1_indexed_backgrounds(
    SnesRecompColourTileCache *cache,const SnesRecompPpuFrame *frame,
    SnesRecompIndexedBackgroundPlan *out);
/* BG/material eligibility with OBJ output supplied independently by qualified
 * indexed runs. Absence of a CPU OBJ colour/index plane is intentional. This
 * does not certify OBJ authority, its clip policy or a complete GPU frame. */
SnesPpuUnsupported snesrecomp_ppu_mode1_indexed_supports(const SnesPpuFrameCapture *capture);

typedef struct SnesRecompIndexedScenePlan {
    SnesRecompIndexedBackgroundPlan background;
    const uint16_t *obj_slots; /* One for each owned, qualified job OBJ run. */
    uint32_t obj_count,obj_tiles_decoded,obj_cache_hits;
} SnesRecompIndexedScenePlan;
/* One coherent atlas preparation. BG slots are pinned while qualified OBJ
 * tiles are appended; a reused BG mesh cannot lose tiles to OBJ collisions.
 * OBJ source changes are checked every preparation, independently of whether
 * BG geometry changed. All data is borrowed until the next cache preparation;
 * no guest memory, sprite selection or CPU colour plane is accessed. */
SnesRecompPpuResult snesrecomp_ppu_mode1_indexed_scene(
    SnesRecompColourTileCache *cache,const SnesRecompPpuJob *job,
    SnesRecompIndexedScenePlan *out);

typedef struct SnesRecompIndexedFramePlan {
    SnesRecompIndexedBackgroundPlan base;
    SnesRecompIndexedScenePlan source;
} SnesRecompIndexedFramePlan;
/* Displayed background and original UI source may have different register AND
 * memory snapshots. Their atlases must remain independent until submission.
 * Passing the same cache is rejected before mutation. Failure exposes neither
 * partial plan. The caller owns both caches and fences each resource slot. */
SnesRecompPpuResult snesrecomp_ppu_mode1_indexed_frame(
    SnesRecompColourTileCache *base_cache,SnesRecompColourTileCache *source_cache,
    const SnesRecompPpuJob *job,SnesRecompIndexedFramePlan *out);

#ifdef __cplusplus
}
#endif
#endif
