#ifndef SNESRECOMP_PPU_ROW_MEMORY_H
#define SNESRECOMP_PPU_ROW_MEMORY_H

#include "snes_ppu_capture.h"
#include "memory_compare.h"
#include <string.h>

enum { SNESRECOMP_PPU_MEMORY_PAGE_WORDS = 64,
       SNESRECOMP_PPU_MEMORY_PAGES = 512,
       SNESRECOMP_PPU_MEMORY_MASK_WORDS = 16 };

typedef struct SnesRecompPpuMemoryMask {
    uint32_t pages[SNESRECOMP_PPU_MEMORY_MASK_WORDS];
} SnesRecompPpuMemoryMask;

static inline void snesrecomp_ppu_memory_mark(SnesRecompPpuMemoryMask *mask,
                                            unsigned word) {
    unsigned page = (word & 32767u) / SNESRECOMP_PPU_MEMORY_PAGE_WORDS;
    mask->pages[page >> 5] |= UINT32_C(1) << (page & 31u);
}

static inline bool snesrecomp_ppu_memory_intersects(
    const SnesRecompPpuMemoryMask *a, const SnesRecompPpuMemoryMask *b) {
    for (unsigned i = 0; i < SNESRECOMP_PPU_MEMORY_MASK_WORDS; ++i)
        if (a->pages[i] & b->pages[i]) return true;
    return false;
}

/* Exact changed pages; dirty counters are never correctness certificates. */
static inline void snesrecomp_ppu_memory_changes(uint16_t previous[32768],
    const uint16_t current[32768], SnesRecompPpuMemoryMask *changed) {
    memset(changed, 0, sizeof *changed);
    for (unsigned page = 0; page < SNESRECOMP_PPU_MEMORY_PAGES; ++page) {
        unsigned at = page * SNESRECOMP_PPU_MEMORY_PAGE_WORDS;
        if (!snesrecomp_u16_equal(previous + at, current + at,
                                  SNESRECOMP_PPU_MEMORY_PAGE_WORDS)) {
            changed->pages[page >> 5] |= UINT32_C(1) << (page & 31u);
            memcpy(previous + at, current + at,
                   SNESRECOMP_PPU_MEMORY_PAGE_WORDS * sizeof *previous);
        }
    }
}

/* Conservative horizontal coverage, exact vertical source row. This admits
 * horizontal windows, scroll, mirror, repeat and elastic remapping without
 * depending on a viewport heuristic. Callers resolve mosaic before entry.
 * Only Mode-1 2bpp/4bpp backgrounds; OBJ/OPT/Mode-7 use separate contracts. */
static inline unsigned snesrecomp_ppu_mode1_row_memory(
    const uint16_t vram[32768], const SnesPpuBgState *bg, unsigned source_y,
    SnesRecompPpuMemoryMask *mask) {
    unsigned shift = bg->big_tiles ? 4u : 3u;
    unsigned ty = (source_y >> shift) & (bg->tall ? 63u : 31u);
    unsigned py = source_y & (bg->big_tiles ? 15u : 7u);
    unsigned palettes = 0;
    for (unsigned tx = 0; tx < (bg->wide ? 64u : 32u); ++tx) {
        unsigned screen = (ty >> 5) * (bg->wide ? 2u : 1u) + (tx >> 5);
        unsigned at = (bg->tilemap_word_addr + screen * 1024u +
                       (ty & 31u) * 32u + (tx & 31u)) & 32767u;
        snesrecomp_ppu_memory_mark(mask, at);
        uint16_t entry = vram[at];
        palettes |= 1u << ((entry >> 10) & 7u);
        unsigned row = entry & 0x8000u ? (bg->big_tiles ? 15u : 7u) - py : py;
        unsigned tile = entry & 1023u;
        for (unsigned col = 0; col < (bg->big_tiles ? 2u : 1u); ++col) {
            unsigned character = (tile + col + ((row >> 3) << 4)) & 1023u;
            unsigned word = (bg->char_word_addr + character * bg->bpp * 4u +
                             (row & 7u)) & 32767u;
            for (unsigned plane = 0; plane < bg->bpp / 2u; ++plane)
                snesrecomp_ppu_memory_mark(mask, word + plane * 8u);
        }
    }
    return palettes;
}

#endif
