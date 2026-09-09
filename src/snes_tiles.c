#include "snesrecomp_platform/snes_tiles.h"

#include <string.h>

void snesrecomp_tile_cache_init(SnesRecompTileCache *cache,
                                uint8_t *atlas4, unsigned atlas4_pitch,
                                uint8_t *atlas2, unsigned atlas2_pitch) {
    memset(cache->shadow, 0, sizeof cache->shadow);
    cache->atlas4 = atlas4;
    cache->atlas2 = atlas2;
    cache->atlas4_pitch = atlas4_pitch;
    cache->atlas2_pitch = atlas2_pitch;
    cache->want4 = true;
    cache->want2 = true;
    cache->primed = false;
    cache->generation = 1;
}

void snesrecomp_tile_cache_set_depths(SnesRecompTileCache *cache,
                                      bool want4, bool want2) {
    if (cache->want4 == want4 && cache->want2 == want2)
        return;
    cache->want4 = want4;
    cache->want2 = want2;
    cache->primed = false;
}

/* One 4bpp tile: 8 rows, each built from four bitplanes. Planes 0/1 are
 * interleaved in the first 16 bytes and planes 2/3 in the next 16 -- the same
 * row's four bytes are therefore 16 apart, not adjacent. */
static void unpack_tile_4bpp(const uint8_t *src, uint8_t *dst, unsigned pitch) {
    for (unsigned y = 0; y < 8; y++) {
        const uint8_t p0 = src[y * 2 + 0];
        const uint8_t p1 = src[y * 2 + 1];
        const uint8_t p2 = src[16 + y * 2 + 0];
        const uint8_t p3 = src[16 + y * 2 + 1];
        uint8_t *row = dst + (size_t)y * pitch;

        /* Bit 7 is the leftmost pixel. */
        for (unsigned x = 0; x < 8; x++) {
            const unsigned bit = 7u - x;
            row[x] = (uint8_t)(((p0 >> bit) & 1u) |
                               (((p1 >> bit) & 1u) << 1) |
                               (((p2 >> bit) & 1u) << 2) |
                               (((p3 >> bit) & 1u) << 3));
        }
    }
}

static void unpack_tile_2bpp(const uint8_t *src, uint8_t *dst, unsigned pitch) {
    for (unsigned y = 0; y < 8; y++) {
        const uint8_t p0 = src[y * 2 + 0];
        const uint8_t p1 = src[y * 2 + 1];
        uint8_t *row = dst + (size_t)y * pitch;

        for (unsigned x = 0; x < 8; x++) {
            const unsigned bit = 7u - x;
            row[x] = (uint8_t)(((p0 >> bit) & 1u) |
                               (((p1 >> bit) & 1u) << 1));
        }
    }
}

static uint8_t *atlas_slot(uint8_t *atlas, unsigned pitch, unsigned tile) {
    const unsigned tx = tile % SNESRECOMP_ATLAS_TILES_X;
    const unsigned ty = tile / SNESRECOMP_ATLAS_TILES_X;
    return atlas + (size_t)ty * 8u * pitch + (size_t)tx * 8u;
}

unsigned snesrecomp_tile_cache_update(SnesRecompTileCache *cache,
                                      const uint8_t *vram,
                                      uint64_t *rows4_dirty,
                                      uint64_t *rows2_dirty) {
    unsigned redone = 0;

    for (unsigned chunk = 0; chunk < SNESRECOMP_VRAM_CHUNKS; chunk++) {
        const unsigned base = chunk * SNESRECOMP_VRAM_CHUNK;

        if (cache->primed &&
            memcmp(cache->shadow + base, vram + base,
                   SNESRECOMP_VRAM_CHUNK) == 0)
            continue;

        memcpy(cache->shadow + base, vram + base, SNESRECOMP_VRAM_CHUNK);
        redone++;

        /* A chunk covers a whole number of tiles at both depths, so neither
         * unpack has to deal with a tile straddling the boundary. */
        if (cache->atlas4 && cache->want4) {
            const unsigned first = base / 32u;
            if (rows4_dirty) {
                const unsigned r0 = first / SNESRECOMP_ATLAS_TILES_X;
                const unsigned r1 = (first + SNESRECOMP_VRAM_CHUNK / 32u - 1u) /
                                    SNESRECOMP_ATLAS_TILES_X;
                for (unsigned r = r0; r <= r1; r++)
                    *rows4_dirty |= (uint64_t)1u << r;
            }
            for (unsigned t = 0; t < SNESRECOMP_VRAM_CHUNK / 32u; t++)
                unpack_tile_4bpp(vram + base + t * 32u,
                                 atlas_slot(cache->atlas4, cache->atlas4_pitch,
                                            first + t),
                                 cache->atlas4_pitch);
        }
        if (cache->atlas2 && cache->want2) {
            const unsigned first = base / 16u;
            if (rows2_dirty) {
                const unsigned r0 = first / SNESRECOMP_ATLAS_TILES_X;
                const unsigned r1 = (first + SNESRECOMP_VRAM_CHUNK / 16u - 1u) /
                                    SNESRECOMP_ATLAS_TILES_X;
                for (unsigned r = r0; r <= r1; r++)
                    *rows2_dirty |= (uint64_t)1u << r;
            }
            for (unsigned t = 0; t < SNESRECOMP_VRAM_CHUNK / 16u; t++)
                unpack_tile_2bpp(vram + base + t * 16u,
                                 atlas_slot(cache->atlas2, cache->atlas2_pitch,
                                            first + t),
                                 cache->atlas2_pitch);
        }
    }

    cache->primed = true;
    if (redone)
        cache->generation++;
    return redone;
}
