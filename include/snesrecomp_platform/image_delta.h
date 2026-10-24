#ifndef SNESRECOMP_PLATFORM_IMAGE_DELTA_H
#define SNESRECOMP_PLATFORM_IMAGE_DELTA_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exact 8x8 upload regions, independent of texture layout and graphics SDK.
 * One bit per block, one word per block row; no hashes or allocations. */
enum { SNESRECOMP_IMAGE_MAX_WIDTH = 512, SNESRECOMP_IMAGE_MAX_HEIGHT = 240,
       SNESRECOMP_IMAGE_BLOCK_ROWS = 30 };
typedef struct SnesRecompImageDelta {
    uint64_t rows[SNESRECOMP_IMAGE_BLOCK_ROWS];
    uint32_t blocks;
    uint16_t width, height;
} SnesRecompImageDelta;

/* Pitches are in uint32_t pixels. Sources must be disjoint; delta must not
 * alias either image. Pixel storage has uint32_t alignment. Retained pixels
 * are ignored when invalidated. The output is empty on rejection. Prepare
 * never changes either image or acknowledges an upload. */
bool snesrecomp_image_delta_prepare(const uint32_t *pixels, uint32_t pitch,
    const uint32_t *retained, uint32_t retained_pitch, uint16_t width,
    uint16_t height, bool invalidated, SnesRecompImageDelta *delta);
/* Call only after successfully uploading all marked blocks, behind the
 * backend's resource fence. The prepared source must remain immutable until
 * this commit. On failure retained is unchanged. On upload failure, skip
 * commit: the next prepare still reports all unacknowledged changes. */
bool snesrecomp_image_delta_commit(const uint32_t *pixels, uint32_t pitch,
    uint32_t *retained, uint32_t retained_pitch, const SnesRecompImageDelta *delta);

#ifdef __cplusplus
}
#endif
#endif
