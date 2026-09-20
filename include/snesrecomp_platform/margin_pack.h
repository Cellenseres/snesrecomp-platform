#ifndef SNESRECOMP_PLATFORM_MARGIN_PACK_H
#define SNESRECOMP_PLATFORM_MARGIN_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* SRMP v1 stores only non-transparent runs outside an asset's centre span.
 * Colours are lossless straight RGBA8888, either directly or through an
 * exact (at most 256-entry) palette. Multi-byte integers are little-endian. */
enum {
    SNESRECOMP_MARGIN_PACK_VERSION = 1,
    SNESRECOMP_MARGIN_ENCODING_INDEXED8 = 1,
    SNESRECOMP_MARGIN_ENCODING_RGBA8888 = 2,
};

typedef struct SnesRecompMarginPack {
    const uint8_t *data;
    size_t size;
    uint16_t asset_count;
    bool owns_data;
} SnesRecompMarginPack;

typedef struct SnesRecompMarginAsset {
    const SnesRecompMarginPack *pack;
    uint32_t id;
    uint16_t canvas_width;
    uint16_t canvas_height;
    uint16_t center_x;
    uint16_t center_width;
    uint8_t encoding;
    uint16_t palette_count;
    uint32_t palette_offset;
    uint32_t rows_offset;
    uint32_t data_offset;
    uint32_t data_size;
} SnesRecompMarginAsset;

typedef struct SnesRecompMarginComposite {
    uint8_t *pixels;
    size_t pitch;
    uint16_t width;
    uint16_t height;
    /* SNES INIDISP brightness, clamped to 0..15. */
    uint8_t brightness;
    /* When true, art is placed behind pixels other than backdrop_argb. */
    bool protect_non_backdrop;
    uint32_t backdrop_argb;
} SnesRecompMarginComposite;

bool snesrecomp_margin_pack_open(
    const void *data, size_t size, SnesRecompMarginPack *out,
    char *error, size_t error_size);

bool snesrecomp_margin_pack_load_file(
    const char *path, SnesRecompMarginPack *out,
    char *error, size_t error_size);

void snesrecomp_margin_pack_close(SnesRecompMarginPack *pack);

bool snesrecomp_margin_asset_find(
    const SnesRecompMarginPack *pack, uint32_t id,
    SnesRecompMarginAsset *out);

/* Composites only the sparse margin runs; the centre is never touched. */
bool snesrecomp_margin_asset_composite_argb8888(
    const SnesRecompMarginAsset *asset,
    const SnesRecompMarginComposite *destination);

#endif
