#ifndef SNESRECOMP_PLATFORM_FRAME_OVERSCAN_H
#define SNESRECOMP_PLATFORM_FRAME_OVERSCAN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Extends the last kept row over bottom rows a consumer set cropped. */
void SnesRecompFrameHideBottomRows(unsigned char *pixels, size_t pitch,
                                   unsigned width, unsigned height,
                                   unsigned rows);

#ifdef __cplusplus
}
#endif

#endif
