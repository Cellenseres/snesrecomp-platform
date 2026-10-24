#include "snesrecomp_platform/image_delta.h"
#include <stddef.h>
#include <string.h>

static bool ImagesValid(const uint32_t *pixels, uint32_t pitch,
    const uint32_t *retained, uint32_t retained_pitch, unsigned w, unsigned h) {
    if (!pixels || !retained || !w || w > SNESRECOMP_IMAGE_MAX_WIDTH ||
        !h || h > SNESRECOMP_IMAGE_MAX_HEIGHT || pitch < w || retained_pitch < w)
        return false;
    const uint64_t a = ((uint64_t)(h - 1) * pitch + w) * sizeof(uint32_t);
    const uint64_t b = ((uint64_t)(h - 1) * retained_pitch + w) * sizeof(uint32_t);
    const uintptr_t x = (uintptr_t)pixels, y = (uintptr_t)retained;
    if (a > UINTPTR_MAX - x || b > UINTPTR_MAX - y) return false;
    return x + (uintptr_t)a <= y || y + (uintptr_t)b <= x;
}
static uint64_t RowMask(unsigned width) {
    const unsigned blocks = (width + 7) / 8;
    return blocks == 64 ? UINT64_MAX : (UINT64_C(1) << blocks) - 1;
}
/* Word comparisons avoid bytewise libc memcmp implementations on consoles.
 * Every bit participates; this remains an exact comparison, not a checksum. */
static bool EqualPixels(const uint32_t *a, const uint32_t *b, unsigned count) {
    while (count >= 4) {
        if ((a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3])) return false;
        a += 4; b += 4; count -= 4;
    }
    while (count--) if (*a++ != *b++) return false;
    return true;
}
bool snesrecomp_image_delta_prepare(const uint32_t *pixels, uint32_t pitch,
    const uint32_t *retained, uint32_t retained_pitch, uint16_t width,
    uint16_t height, bool invalidated, SnesRecompImageDelta *delta) {
    if (!delta) return false;
    memset(delta, 0, sizeof *delta);
    if (!ImagesValid(pixels, pitch, retained, retained_pitch, width, height)) return false;
    delta->width = width; delta->height = height;
    for (unsigned by = 0; by < height; by += 8) {
        const unsigned count = height - by < 8 ? height - by : 8;
        bool changed = invalidated;
        for (unsigned y = by; !changed && y < by + count; ++y)
            changed = !EqualPixels(pixels + (size_t)y * pitch,
                retained + (size_t)y * retained_pitch, width);
        if (!changed) continue;
        uint64_t bits = 0;
        for (unsigned bx = 0; bx < width; bx += 8) {
            const unsigned columns = width - bx < 8 ? width - bx : 8;
            changed = invalidated;
            for (unsigned y = by; !changed && y < by + count; ++y)
                changed = !EqualPixels(pixels + (size_t)y * pitch + bx,
                    retained + (size_t)y * retained_pitch + bx, columns);
            if (changed) { bits |= UINT64_C(1) << (bx / 8); ++delta->blocks; }
        }
        delta->rows[by / 8] = bits;
    }
    return true;
}
bool snesrecomp_image_delta_commit(const uint32_t *pixels, uint32_t pitch,
    uint32_t *retained, uint32_t retained_pitch, const SnesRecompImageDelta *delta) {
    if (!delta || !ImagesValid(pixels, pitch, retained, retained_pitch,
                              delta->width, delta->height)) return false;
    const unsigned w = delta->width, h = delta->height;
    const uint64_t mask = RowMask(w);
    for (unsigned row = 0; row < SNESRECOMP_IMAGE_BLOCK_ROWS; ++row)
        if (delta->rows[row] & ~(row * 8 < h ? mask : UINT64_C(0))) return false;
    for (unsigned by = 0; by < h; by += 8) {
        const uint64_t bits = delta->rows[by / 8];
        const unsigned count = h - by < 8 ? h - by : 8;
        if (!bits) continue;
        if (bits == mask) {
            for (unsigned y = by; y < by + count; ++y)
                memcpy(retained + (size_t)y * retained_pitch,
                       pixels + (size_t)y * pitch, w * sizeof(uint32_t));
        } else for (unsigned bx = 0; bx < w; bx += 8) {
            if (!(bits & (UINT64_C(1) << (bx / 8)))) continue;
            const unsigned columns = w - bx < 8 ? w - bx : 8;
            for (unsigned y = by; y < by + count; ++y)
                memcpy(retained + (size_t)y * retained_pitch + bx,
                    pixels + (size_t)y * pitch + bx, columns * sizeof(uint32_t));
        }
    }
    return true;
}
