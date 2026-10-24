#ifndef SNESRECOMP_PLATFORM_SNES_PPU_INDEXED_PALETTE_H
#define SNESRECOMP_PLATFORM_SNES_PPU_INDEXED_PALETTE_H
#include "snesrecomp_platform/abi.h"
#include "snesrecomp_platform/snes_ppu_frame.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { SNESRECOMP_INDEXED_PALETTE_RAW_RGB5=1, SNESRECOMP_INDEXED_PALETTE_DISPLAY_RGB8=2 };
typedef struct SnesRecompIndexedPalette SnesRecompIndexedPalette;
typedef struct SnesRecompIndexedPaletteSpan { uint16_t y0,y1; } SnesRecompIndexedPaletteSpan;
SNESRECOMP_STATIC_ASSERT(sizeof(SnesRecompIndexedPaletteSpan)==4,"palette span ABI");
typedef struct SnesRecompIndexedPalettePlan {
    const uint32_t *argb; /* [original source row][256 CGRAM entries] */
    const uint64_t *generations; /* Per row; each fenced GPU slot tracks its own. */
    uint32_t rows_updated,texels_written;
    uint16_t row_count;
    uint8_t encoding,reserved;
    /* Equal resolved lookup rows, computed only after row changes/resize.
     * GPU adapters bind one palette per span instead of inspecting unchanged
     * palette histories every frame. Spans partition every original row. */
    const SnesRecompIndexedPaletteSpan *spans;
    uint16_t span_count,span_reserved;
    uint32_t spans_rebuilt;
} SnesRecompIndexedPalettePlan;
/* Persistent original-coordinate colour lookup; no tile or RGB frame rebuild.
 * RAW uses exact RGB5*8 channels for later GPU composition. DISPLAY applies
 * the original expanded-byte brightness, for admitted unmixed source colours.
 * Owned CGRAM histories are required when the frame has raster palettes.
 * Warm certificates avoid scanning palettes; brightness changes invalidate
 * display rows only. All views are cache-owned, never borrowed guest memory. */
SnesRecompIndexedPalette *snesrecomp_ppu_indexed_palette_create(uint8_t encoding);
void snesrecomp_ppu_indexed_palette_destroy(SnesRecompIndexedPalette *cache);
void snesrecomp_ppu_indexed_palette_reset(SnesRecompIndexedPalette *cache);
/* CPU lookup format change, before preparation. Invalidates colour rows only;
 * monotonic upload versions survive. No allocation or GPU memory access. */
bool snesrecomp_ppu_indexed_palette_set_encoding(SnesRecompIndexedPalette *cache,uint8_t encoding);
/* Effect qualifier, not complete GPU admission. Direct palette output is exact
 * when no active base pixel needs colour math, colour clipping or OBJ.
 * BG window visibility is already resolved in the admitted indexed geometry. */
bool snesrecomp_ppu_indexed_palette_base_direct(const SnesRecompPpuFrame *frame);
SnesRecompPpuResult snesrecomp_ppu_indexed_palette_prepare(
    SnesRecompIndexedPalette *cache,const SnesRecompPpuFrame *frame,SnesRecompIndexedPalettePlan *out);
#ifdef __cplusplus
}
#endif
#endif
