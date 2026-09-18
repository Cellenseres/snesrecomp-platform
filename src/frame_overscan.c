#include "snesrecomp_platform/frame_overscan.h"

#include <string.h>

void SnesRecompFrameHideBottomRows(unsigned char *pixels, size_t pitch,
                                   unsigned width, unsigned height,
                                   unsigned rows) {
    if (!pixels || !width || rows >= height)
        return;

    const unsigned keep = height - rows;
    const unsigned char *src = pixels + (size_t)(keep - 1) * pitch;
    const size_t row_bytes = (size_t)width * 4u;

    for (unsigned y = keep; y < height; y++)
        memcpy(pixels + (size_t)y * pitch, src, row_bytes);
}
