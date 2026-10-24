#ifndef SNESRECOMP_PLATFORM_SNES_PPU_SOURCE_COMPACT_H
#define SNESRECOMP_PLATFORM_SNES_PPU_SOURCE_COMPACT_H

#include "snes_ppu_job.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { SNESRECOMP_PPU_SOURCE_COMPACT_EDGE_TILES = 1u };

/* Remove unreferenced columns/rows from one source-only image. Rectangles
 * retain contiguous sampling and edge clamps. The image must have no post
 * overlay consumers. Input pixels are borrowed only during this call.
 * packed must be independent; output_sources may equal sources. No allocation.
 * EDGE_TILES keeps outer texels/all rows and eight-pixel horizontal boundaries,
 * so scrolling edge views retain their texture grid. Zero crops both axes.
 * All outputs stay unchanged on failure. UNSUPPORTED means no useful crop.
 * On success the caller owns packed and freezes the returned image/rectangles. */
SnesRecompPpuResult snesrecomp_ppu_source_image_compact(
    const SnesRecompPpuJobImage *image,uint16_t image_index,
    const SnesRecompPpuSource *sources,uint16_t source_count,
    uint32_t *packed,uint32_t pixel_capacity,uint32_t flags,
    SnesRecompPpuJobImage *output_image,SnesRecompPpuSource *output_sources);

#ifdef __cplusplus
}
#endif
#endif
