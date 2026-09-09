#ifndef SNESRECOMP_PLATFORM_SNES_BG_UPLOAD_H
#define SNESRECOMP_PLATFORM_SNES_BG_UPLOAD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Converts packed tilemap and CGRAM data into GPU-friendly textures. */

enum {
    /* Tilemaps are 32x32 entries per screen and up to 2x2 screens. Always
     * emitting 64x64 lets the shader wrap with repeat addressing instead of
     * needing the map's real size as a uniform: a 32-wide map is written twice
     * across, exactly as the hardware mirrors it. */
    SNESRECOMP_TILEMAP_DIM = 64,
    SNESRECOMP_TILEMAP_TEXELS = SNESRECOMP_TILEMAP_DIM * SNESRECOMP_TILEMAP_DIM,

    SNESRECOMP_CGRAM_ENTRIES = 256,
    SNESRECOMP_BRIGHTNESS_LEVELS = 16,
    SNESRECOMP_PALETTE_LEVEL_BYTES = SNESRECOMP_CGRAM_ENTRIES * 4,
};

/* Per-texel layout of the tilemap texture, matching bg_layer_f.cg:
 *   r  tile index, low 8 bits
 *   g  tile index, high 2 bits            (r + g*256 recovers all 10)
 *   b  palette number
 *   a  bit 0 = horizontal flip, bit 1 = vertical flip, bit 2 = priority
 *
 * Priority selects which of two draws a tile belongs to. It travels in the
 * texel so the fragment shader can drop tiles that belong to the other pass;
 * the alternative would be two tilemaps per layer. */
/* One tilemap texel, packed the way bg_layer_f.cg unpacks it. Exposed so a
 * caller that sources entries from somewhere other than plain VRAM -- a
 * widescreen margin reconstructed from map data, say -- writes the identical
 * layout rather than a second copy of these shifts. */
void snesrecomp_bg_pack_entry(uint8_t *texel, uint16_t entry);

void snesrecomp_bg_build_tilemap(uint8_t *dst_rgba,
                                 const uint16_t *vram,
                                 unsigned tilemap_word_addr,
                                 bool wide, bool tall);

/* CGRAM is BGR555 with 5 bits per channel; the shader wants straight RGBA8.
 * Replicating the top bits into the low ones (x << 3 | x >> 2) spans the full
 * 0..255 range, so white stays white instead of landing on 248.
 *
 * `brightness` is the hardware's 0..15 master brightness, applied here rather
 * than per pixel. Raster backends can precompute all 16 tables with the helper
 * below and select among exact integer results per scanline group. */
void snesrecomp_bg_build_palette(uint8_t *dst_rgba, const uint16_t *cgram,
                                 unsigned brightness);

/* All exact master-brightness results, row-major from level 0 through 15.
 * Raster brightness can then select a precomputed row without uploading a
 * full palette at every scanline transition. */
void snesrecomp_bg_build_palette_levels(uint8_t *dst_rgba,
                                        const uint16_t *cgram);

#ifdef __cplusplus
}
#endif

#endif
