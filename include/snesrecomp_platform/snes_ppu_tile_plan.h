#ifndef SNESRECOMP_PLATFORM_SNES_PPU_TILE_PLAN_H
#define SNESRECOMP_PLATFORM_SNES_PPU_TILE_PLAN_H
#include "snesrecomp_platform/snes_ppu_frame.h"
#include "snesrecomp_platform/snes_ppu_tile_types.h"
#include "snesrecomp_platform/snes_ppu_job.h"
#include "snesrecomp_platform/snes_ppu_semantic_gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SNESRECOMP_COLOUR_TILE_SLOTS = 8192,
    SNESRECOMP_TILE_PLAN_QUADS = 16384,
    SNESRECOMP_TILE_PLAN_PASSES = 7
};
typedef struct SnesRecompTilePlan {
    const SnesRecompTileQuad *quads;
    const SnesRecompTileBackdrop *backdrops;
    const uint32_t *tiles; /* Contiguous 8x8 ARGB tiles, zero alpha transparent. */
    const uint8_t *dirty; /* One byte per slot, retained until acknowledged. */
    const uint64_t *generations; /* Slot content versions for rotating GPU resources. */
    const uint32_t *obj_pixels; /* ARGB, alpha encodes priority as 63/127/191/255. */
    const uint64_t *obj_coverage; /* Conservative qualified eight-pixel blocks per row. */
    uint32_t quad_count, backdrop_count, tiles_decoded, cache_hits;
    uint32_t geometry_reused;
    /* Mode 7's bounded sparse subscreen composition uses reserved OBJ alpha
     * 15 for final exact pixels. A backend must draw that rung last. */
    uint32_t obj_overlay_pixels;
    /* Monotonic within this cache, including reset and cancelled preparation. */
    uint64_t geometry_generation, object_generation;
    uint16_t width, height;
    SnesPpuUnsupported unsupported;
} SnesRecompTilePlan;
typedef struct SnesRecompColourTileCache SnesRecompColourTileCache;

SnesRecompColourTileCache *snesrecomp_colour_tiles_create(void);
void snesrecomp_colour_tiles_destroy(SnesRecompColourTileCache *cache);
void snesrecomp_colour_tiles_reset(SnesRecompColourTileCache *cache);
void snesrecomp_colour_tiles_uploaded(SnesRecompColourTileCache *cache);
/* No GPU writes or display side effects; partial plans are never executable. */
SnesRecompPpuResult snesrecomp_ppu_tile_plan(SnesRecompColourTileCache *cache,
    const SnesRecompPpuFrame *frame, SnesRecompTilePlan *out);
/* Unshaded Mode-1 BG tiles for exact GPU main/sub composition. RGB stores
 * 8*RGB5, opaque source texels have alpha255. Pass buckets use RAW_TILE_*.
 * OBJ and backdrops are supplied by the corresponding raw-region plan.
 * This does not generate a finished CPU image. Initial subset: brightness15,
 * no mosaic/direct colour; ordinary conservative plans remain unchanged. */
SnesRecompPpuResult snesrecomp_ppu_mode1_raw_tiles(SnesRecompColourTileCache *cache,
    const SnesRecompPpuFrame *frame, SnesRecompTilePlan *out);
/* The same raw plane/material plan, with owned art replacing resolved BG
 * regions before sprite priority and exact main/sub colour composition. */
SnesRecompPpuResult snesrecomp_ppu_mode1_source_tiles(SnesRecompColourTileCache *cache,
    const SnesRecompPpuJob *job, SnesRecompTilePlan *out);
SnesPpuUnsupported snesrecomp_ppu_mode1_raw_supports(const SnesPpuFrameCapture *capture);
/* Resolve one logical opaque pixel from the recorded Mode-1 subset, including
 * the genuine subscreen operand and exact brightness/colour mathematics.
 * Used for consumer condition keys without rendering a CPU frame or reading
 * back the GPU. No allocation, cache mutation or guest/sprite evaluation.
 * Unsupported effects and invalid coordinates leave argb unchanged. */
SnesRecompPpuResult snesrecomp_ppu_mode1_sample(const SnesRecompPpuFrame *frame,
    uint16_t x, uint16_t y, uint32_t *argb);
/* Original, unshaded subscreen operand. Genuine is based on palette index,
 * never RGB black: an opaque black tile still permits subscreen halving. */
SnesRecompPpuResult snesrecomp_ppu_mode1_sub_sample(const SnesRecompPpuFrame *frame,
    uint16_t x, uint16_t y, uint16_t *rgb5, bool *genuine);
/* Uncomposed, isolated native BG sample for resolved source placement. The
 * priority bit is meaningful only when opaque is true. No OBJ qualification,
 * cache mutation, colour composition or guest execution. */
SnesRecompPpuResult snesrecomp_ppu_mode1_layer_sample(const SnesPpuFrameCapture *capture,
    const SnesPpuRasterBand *band, uint8_t layer, bool sub, uint16_t x,
    uint16_t y, bool *opaque, uint8_t *priority);
/* Batch consumers calculate windows once per immutable raster band. */
SnesRecompPpuResult snesrecomp_ppu_mode1_layer_sample_windows(const SnesPpuFrameCapture *capture,
    const SnesPpuRasterBand *band, const SnesRecompWindowSpan *windows, unsigned window_count,
    uint8_t layer, bool sub, uint16_t x, uint16_t y, bool *opaque, uint8_t *priority);
/* Shared exact backdrop/qualified-OBJ plan for the separate Mode 7 layer.
 * It does not claim that a background has been drawn or omitted safely. */
SnesRecompPpuResult snesrecomp_ppu_mode7_colour_plan(SnesRecompColourTileCache *cache,
    const SnesRecompPpuFrame *frame, SnesRecompTilePlan *out);
#ifdef __cplusplus
}
#endif
#endif
