#include "snesrecomp_platform/snes_bg_upload.h"

#include <stddef.h>

/* A tilemap entry:
 *   bits 0-9    tile index
 *   bits 10-12  palette number
 *   bit  13     priority
 *   bit  14     horizontal flip
 *   bit  15     vertical flip
 */
enum {
    ENTRY_TILE_MASK = 0x03FFu,
    ENTRY_PALETTE_SHIFT = 10,
    ENTRY_PALETTE_MASK = 0x7u,
    ENTRY_PRIORITY = 0x2000u,
    ENTRY_HFLIP = 0x4000u,
    ENTRY_VFLIP = 0x8000u,
};

void snesrecomp_bg_pack_entry(uint8_t *texel, uint16_t entry) {
    const unsigned tile = entry & ENTRY_TILE_MASK;
    texel[0] = (uint8_t)(tile & 0xFFu);
    texel[1] = (uint8_t)(tile >> 8);
    texel[2] = (uint8_t)((entry >> ENTRY_PALETTE_SHIFT) & ENTRY_PALETTE_MASK);
    texel[3] = (uint8_t)(((entry & ENTRY_HFLIP) ? 1u : 0u) |
                         ((entry & ENTRY_VFLIP) ? 2u : 0u) |
                         ((entry & ENTRY_PRIORITY) ? 4u : 0u));
}

void snesrecomp_bg_build_tilemap(uint8_t *dst_rgba,
                                 const uint16_t *vram,
                                 unsigned tilemap_word_addr,
                                 bool wide, bool tall) {
    for (unsigned y = 0; y < SNESRECOMP_TILEMAP_DIM; y++) {
        for (unsigned x = 0; x < SNESRECOMP_TILEMAP_DIM; x++) {
            /* Mirror missing screens into the full 64x64 output. */
            const unsigned sx = wide ? (x >> 5) & 1u : 0u;
            const unsigned sy = tall ? (y >> 5) & 1u : 0u;
            const unsigned screen = sy * (wide ? 2u : 1u) + sx;
            const unsigned within = ((y & 31u) << 5) | (x & 31u);
            const uint16_t entry =
                vram[(tilemap_word_addr + screen * 0x400u + within) & 0x7FFFu];

            snesrecomp_bg_pack_entry(
                dst_rgba + ((size_t)y * SNESRECOMP_TILEMAP_DIM + x) * 4u,
                entry);
        }
    }
}

void snesrecomp_bg_build_palette(uint8_t *dst_rgba, const uint16_t *cgram,
                                 unsigned brightness) {
    if (brightness > 15u)
        brightness = 15u;
    for (unsigned i = 0; i < SNESRECOMP_CGRAM_ENTRIES; i++) {
        const uint16_t c = cgram[i];
        const unsigned r = c & 0x1Fu;
        const unsigned g = (c >> 5) & 0x1Fu;
        const unsigned b = (c >> 10) & 0x1Fu;
        uint8_t *texel = dst_rgba + (size_t)i * 4u;

        /* Expand to 8 bits and apply integer master brightness. */
        texel[0] = (uint8_t)((((r << 3) | (r >> 2)) * brightness) / 15u);
        texel[1] = (uint8_t)((((g << 3) | (g >> 2)) * brightness) / 15u);
        texel[2] = (uint8_t)((((b << 3) | (b >> 2)) * brightness) / 15u);
        texel[3] = 255u;
    }
}

void snesrecomp_bg_build_palette_levels(uint8_t *dst_rgba,
                                        const uint16_t *cgram) {
    for (unsigned brightness = 0;
         brightness < SNESRECOMP_BRIGHTNESS_LEVELS; brightness++)
        snesrecomp_bg_build_palette(
            dst_rgba + (size_t)brightness * SNESRECOMP_PALETTE_LEVEL_BYTES,
            cgram, brightness);
}
