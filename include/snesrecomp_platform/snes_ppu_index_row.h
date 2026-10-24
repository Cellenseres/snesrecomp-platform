#ifndef SNESRECOMP_PPU_INDEX_ROW_H
#define SNESRECOMP_PPU_INDEX_ROW_H

#include "snes_ppu_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnesRecompPpuIndexRow {
    uint16_t words[64];
    uint8_t pixels[512];
    unsigned scroll, row;
    bool has_visible_pixels; /* Any nonzero low-nibble pixel in columns 0..255. */
} SnesRecompPpuIndexRow;

/* Initialize to zero. Each column certifies its current map word and both
 * planar words, including borrowed VRAM and writes without dirty counters.
 * Palette indices remain reusable during CGRAM animation. No guest state. */
typedef struct SnesRecompPpuIndexRowCache {
    SnesRecompPpuIndexRow output;
    uint32_t planes[64];
    uint64_t valid, hits, decodes;
} SnesRecompPpuIndexRowCache;

/* Small 4bpp tiles only. source_y already includes scroll/mosaic resolution.
 * columns is 64, or 32 with zero horizontal scroll. The latter only defines
 * pixels/words 0..255/0..31. Output is borrowed until this cache is reused.
 * Validation failure leaves the cache unchanged and returns NULL. */
const SnesRecompPpuIndexRow *snesrecomp_ppu_index_row4(
    SnesRecompPpuIndexRowCache *cache, const uint16_t vram[32768],
    const SnesPpuBgState *bg, unsigned source_y, unsigned h_scroll,
    unsigned columns);

#ifdef __cplusplus
}
#endif
#endif
