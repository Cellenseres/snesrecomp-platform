#include "snesrecomp_platform/margin_pack.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    HEADER_SIZE = 16,
    ENTRY_SIZE = 32,
};

static uint16_t Read16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t Read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void SetError(char *error, size_t size, const char *format, ...) {
    if (!error || !size)
        return;
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
}

static bool Span(size_t size, uint32_t offset, size_t bytes) {
    return (size_t)offset <= size && bytes <= size - (size_t)offset;
}

static bool ReadAsset(
    const SnesRecompMarginPack *pack, unsigned index,
    SnesRecompMarginAsset *out) {
    if (!pack || !out || index >= pack->asset_count)
        return false;
    const uint8_t *entry = pack->data + HEADER_SIZE + (size_t)index * ENTRY_SIZE;
    out->pack = pack;
    out->id = Read32(entry);
    out->canvas_width = Read16(entry + 4);
    out->canvas_height = Read16(entry + 6);
    out->center_x = Read16(entry + 8);
    out->center_width = Read16(entry + 10);
    out->encoding = entry[12];
    out->palette_count = Read16(entry + 14);
    out->palette_offset = Read32(entry + 16);
    out->rows_offset = Read32(entry + 20);
    out->data_offset = Read32(entry + 24);
    out->data_size = Read32(entry + 28);
    return true;
}

static bool ValidateAsset(
    const SnesRecompMarginPack *pack, unsigned index,
    char *error, size_t error_size) {
    SnesRecompMarginAsset asset;
    if (!ReadAsset(pack, index, &asset))
        return false;
    const uint32_t center_end =
        (uint32_t)asset.center_x + asset.center_width;
    const size_t rows_bytes = ((size_t)asset.canvas_height + 1u) * 4u;
    const size_t pixel_bytes =
        asset.encoding == SNESRECOMP_MARGIN_ENCODING_RGBA8888 ? 4u : 1u;

    if (!asset.canvas_width || !asset.canvas_height ||
        center_end > asset.canvas_width || !asset.center_width) {
        SetError(error, error_size, "margin asset %u has invalid dimensions", index);
        return false;
    }
    if (asset.encoding != SNESRECOMP_MARGIN_ENCODING_INDEXED8 &&
        asset.encoding != SNESRECOMP_MARGIN_ENCODING_RGBA8888) {
        SetError(error, error_size, "margin asset %u has unknown encoding", index);
        return false;
    }
    if (asset.encoding == SNESRECOMP_MARGIN_ENCODING_INDEXED8) {
        if (!asset.palette_count || asset.palette_count > 256u ||
            !Span(pack->size, asset.palette_offset,
                  (size_t)asset.palette_count * 4u)) {
            SetError(error, error_size, "margin asset %u has invalid palette", index);
            return false;
        }
    } else if (asset.palette_count != 0u) {
        SetError(error, error_size, "margin asset %u has a palette in RGBA mode", index);
        return false;
    }
    if (!Span(pack->size, asset.rows_offset, rows_bytes) ||
        !Span(pack->size, asset.data_offset, asset.data_size)) {
        SetError(error, error_size, "margin asset %u points outside the pack", index);
        return false;
    }

    const uint8_t *rows = pack->data + asset.rows_offset;
    const uint8_t *data = pack->data + asset.data_offset;
    for (unsigned y = 0; y < asset.canvas_height; y++) {
        size_t position = Read32(rows + (size_t)y * 4u);
        const size_t end = Read32(rows + ((size_t)y + 1u) * 4u);
        if (position > end || end > asset.data_size) {
            SetError(error, error_size, "margin asset %u has invalid row offsets", index);
            return false;
        }
        while (position < end) {
            if (end - position < 4u) {
                SetError(error, error_size, "margin asset %u has a short run", index);
                return false;
            }
            const unsigned x = Read16(data + position);
            const unsigned length = Read16(data + position + 2u);
            position += 4u;
            if (!length || x > asset.canvas_width ||
                length > (unsigned)asset.canvas_width - x ||
                !((x + length <= asset.center_x) || (x >= center_end)) ||
                (size_t)length > (end - position) / pixel_bytes) {
                SetError(error, error_size, "margin asset %u has an invalid run", index);
                return false;
            }
            if (asset.encoding == SNESRECOMP_MARGIN_ENCODING_INDEXED8) {
                for (unsigned i = 0; i < length; i++) {
                    if (data[position + i] >= asset.palette_count) {
                        SetError(error, error_size,
                                 "margin asset %u has an invalid palette index", index);
                        return false;
                    }
                }
            }
            position += (size_t)length * pixel_bytes;
        }
    }
    return true;
}

bool snesrecomp_margin_pack_open(
    const void *data, size_t size, SnesRecompMarginPack *out,
    char *error, size_t error_size) {
    if (out)
        memset(out, 0, sizeof *out);
    if (!out || !data || size < HEADER_SIZE) {
        SetError(error, error_size, "margin pack is too short");
        return false;
    }
    const uint8_t *bytes = (const uint8_t *)data;
    const uint16_t version = Read16(bytes + 4);
    const uint16_t count = Read16(bytes + 6);
    if (memcmp(bytes, "SRMP", 4) != 0 ||
        version != SNESRECOMP_MARGIN_PACK_VERSION) {
        SetError(error, error_size, "not a supported SRMP margin pack");
        return false;
    }
    if ((size_t)count > (size - HEADER_SIZE) / ENTRY_SIZE) {
        SetError(error, error_size, "margin pack index is truncated");
        return false;
    }
    out->data = bytes;
    out->size = size;
    out->asset_count = count;
    out->owns_data = false;
    for (unsigned i = 0; i < count; i++) {
        if (!ValidateAsset(out, i, error, error_size)) {
            memset(out, 0, sizeof *out);
            return false;
        }
    }
    return true;
}

bool snesrecomp_margin_pack_load_file(
    const char *path, SnesRecompMarginPack *out,
    char *error, size_t error_size) {
    if (out)
        memset(out, 0, sizeof *out);
    if (!path || !out) {
        SetError(error, error_size, "no margin pack path");
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        SetError(error, error_size, "cannot open %s", path);
        return false;
    }
    bool ok = fseek(file, 0, SEEK_END) == 0;
    const long length = ok ? ftell(file) : -1;
    ok = ok && length >= 0 && fseek(file, 0, SEEK_SET) == 0;
    uint8_t *data = ok && length ? (uint8_t *)malloc((size_t)length) : NULL;
    ok = ok && length > 0 && data &&
         fread(data, 1, (size_t)length, file) == (size_t)length;
    fclose(file);
    if (!ok) {
        free(data);
        SetError(error, error_size, "cannot read %s", path);
        return false;
    }
    if (!snesrecomp_margin_pack_open(
            data, (size_t)length, out, error, error_size)) {
        free(data);
        return false;
    }
    out->owns_data = true;
    return true;
}

void snesrecomp_margin_pack_close(SnesRecompMarginPack *pack) {
    if (!pack)
        return;
    if (pack->owns_data)
        free((void *)pack->data);
    memset(pack, 0, sizeof *pack);
}

bool snesrecomp_margin_asset_find(
    const SnesRecompMarginPack *pack, uint32_t id,
    SnesRecompMarginAsset *out) {
    if (out)
        memset(out, 0, sizeof *out);
    if (!pack || !out)
        return false;
    for (unsigned i = 0; i < pack->asset_count; i++) {
        SnesRecompMarginAsset candidate;
        if (ReadAsset(pack, i, &candidate) && candidate.id == id) {
            *out = candidate;
            return true;
        }
    }
    return false;
}

static uint32_t LoadArgb(const uint8_t *p) {
    uint32_t value;
    memcpy(&value, p, sizeof value);
    return value;
}

static void StoreArgb(uint8_t *p, uint32_t value) {
    memcpy(p, &value, sizeof value);
}

static uint32_t Composite(
    const uint8_t *rgba, uint32_t destination, unsigned brightness) {
    const unsigned r = (unsigned)rgba[0] * brightness / 15u;
    const unsigned g = (unsigned)rgba[1] * brightness / 15u;
    const unsigned b = (unsigned)rgba[2] * brightness / 15u;
    const unsigned a = rgba[3];
    const unsigned inverse = 255u - a;
    const unsigned da = destination >> 24;
    const unsigned dr = (destination >> 16) & 0xffu;
    const unsigned dg = (destination >> 8) & 0xffu;
    const unsigned db = destination & 0xffu;
    const unsigned oa = a + (da * inverse + 127u) / 255u;
    const unsigned or_ = (r * a + dr * inverse + 127u) / 255u;
    const unsigned og = (g * a + dg * inverse + 127u) / 255u;
    const unsigned ob = (b * a + db * inverse + 127u) / 255u;
    return (oa << 24) | (or_ << 16) | (og << 8) | ob;
}

bool snesrecomp_margin_asset_composite_argb8888(
    const SnesRecompMarginAsset *asset,
    const SnesRecompMarginComposite *destination) {
    if (!asset || !asset->pack || !destination || !destination->pixels ||
        destination->width != asset->canvas_width ||
        destination->height != asset->canvas_height ||
        destination->pitch < (size_t)destination->width * 4u)
        return false;
    unsigned brightness = destination->brightness;
    if (brightness > 15u)
        brightness = 15u;
    if (!brightness)
        return true;

    const uint8_t *rows = asset->pack->data + asset->rows_offset;
    const uint8_t *data = asset->pack->data + asset->data_offset;
    const uint8_t *palette = asset->pack->data + asset->palette_offset;
    const size_t pixel_bytes =
        asset->encoding == SNESRECOMP_MARGIN_ENCODING_RGBA8888 ? 4u : 1u;
    for (unsigned y = 0; y < asset->canvas_height; y++) {
        size_t position = Read32(rows + (size_t)y * 4u);
        const size_t end = Read32(rows + ((size_t)y + 1u) * 4u);
        while (position < end) {
            const unsigned x = Read16(data + position);
            const unsigned length = Read16(data + position + 2u);
            position += 4u;
            for (unsigned i = 0; i < length; i++) {
                uint8_t *at = destination->pixels +
                    (size_t)y * destination->pitch + (size_t)(x + i) * 4u;
                const uint32_t there = LoadArgb(at);
                const uint8_t *rgba = asset->encoding ==
                        SNESRECOMP_MARGIN_ENCODING_INDEXED8
                    ? palette + (size_t)data[position + i] * 4u
                    : data + position + (size_t)i * 4u;
                if (!destination->protect_non_backdrop ||
                    there == destination->backdrop_argb ||
                    (there >> 24) == 0u) {
                    StoreArgb(at, Composite(rgba, there, brightness));
                }
            }
            position += (size_t)length * pixel_bytes;
        }
    }
    return true;
}
