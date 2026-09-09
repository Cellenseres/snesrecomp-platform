#ifndef SNESRECOMP_PLATFORM_SNES_TILES_H
#define SNESRECOMP_PLATFORM_SNES_TILES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Incrementally unpacks SNES bitplane tiles into indexed GPU atlases. */

enum {
    SNESRECOMP_VRAM_BYTES = 0x10000,

    /* A 4bpp tile is 32 bytes, a 2bpp tile 16, so the same VRAM yields
     * different tile counts depending on how a layer reads it. Both atlases
     * exist because a frame can use both depths at once -- Mode 1 has 4bpp
     * backgrounds and a 2bpp one. */
    SNESRECOMP_TILES_4BPP = SNESRECOMP_VRAM_BYTES / 32,   /* 2048 */
    SNESRECOMP_TILES_2BPP = SNESRECOMP_VRAM_BYTES / 16,   /* 4096 */

    /* Atlas layout, in tiles. Width is a power of two so the shader can turn a
     * tile index into a position with a multiply and a floor. */
    SNESRECOMP_ATLAS_TILES_X = 64,
    SNESRECOMP_ATLAS_4BPP_TILES_Y = SNESRECOMP_TILES_4BPP / SNESRECOMP_ATLAS_TILES_X,
    SNESRECOMP_ATLAS_2BPP_TILES_Y = SNESRECOMP_TILES_2BPP / SNESRECOMP_ATLAS_TILES_X,

    SNESRECOMP_ATLAS_W = SNESRECOMP_ATLAS_TILES_X * 8,
    SNESRECOMP_ATLAS_4BPP_H = SNESRECOMP_ATLAS_4BPP_TILES_Y * 8,
    SNESRECOMP_ATLAS_2BPP_H = SNESRECOMP_ATLAS_2BPP_TILES_Y * 8,

    /* Dirty granularity. 512 bytes is 16 tiles at 4bpp and 32 at 2bpp: small
     * enough that a single tile write does not force a large re-unpack, large
     * enough that the comparison stays a handful of word loads. */
    SNESRECOMP_VRAM_CHUNK = 512,
    SNESRECOMP_VRAM_CHUNKS = SNESRECOMP_VRAM_BYTES / SNESRECOMP_VRAM_CHUNK,
};

typedef struct SnesRecompTileCache {
    uint8_t shadow[SNESRECOMP_VRAM_BYTES];
    /* Destinations are supplied by the caller so they can live in GPU memory
     * and be sampled without a further copy. */
    uint8_t *atlas4;            /* SNESRECOMP_ATLAS_W * SNESRECOMP_ATLAS_4BPP_H */
    uint8_t *atlas2;            /* SNESRECOMP_ATLAS_W * SNESRECOMP_ATLAS_2BPP_H */
    unsigned atlas4_pitch;
    unsigned atlas2_pitch;
    bool want4, want2;          /* unpack only the depths a frame will sample */
    bool primed;                /* false until the first, unconditional pass */
    uint32_t generation;        /* bumped whenever a chunk was re-unpacked */
} SnesRecompTileCache;

void snesrecomp_tile_cache_init(SnesRecompTileCache *cache,
                                uint8_t *atlas4, unsigned atlas4_pitch,
                                uint8_t *atlas2, unsigned atlas2_pitch);

/* Which depths to keep current. Unpacking a depth no layer samples is pure
 * waste, and it is the larger half of the cost: the 2bpp atlas holds twice as
 * many tiles as the 4bpp one, so maintaining both when a Mode 1 frame only
 * reads 4bpp triples the work. Changing the selection re-primes, because a
 * depth that was switched off has been missing every write since. */
void snesrecomp_tile_cache_set_depths(SnesRecompTileCache *cache,
                                      bool want4, bool want2);

/* Unpacks whatever changed since the last call. Returns the number of 512-byte
 * chunks that had to be redone, which is the honest cost signal: a frame that
 * uploads a new tileset will report hundreds, a frame that only scrolls zero.
 *
 * `rows4_dirty` and `rows2_dirty`, when given, receive a bit per atlas tile-row
 * that was written (bit n = pixel rows n*8 .. n*8+7). A row is 4 KB of
 * contiguous pixels, which is the unit worth copying to the GPU: the caller
 * keeps its own copies and needs to know what to refresh without comparing
 * 128 KB to find out. Bits are OR-ed in, never cleared, so a caller can
 * accumulate across several frames. */
unsigned snesrecomp_tile_cache_update(SnesRecompTileCache *cache,
                                      const uint8_t *vram,
                                      uint64_t *rows4_dirty,
                                      uint64_t *rows2_dirty);

#ifdef __cplusplus
}
#endif

#endif
