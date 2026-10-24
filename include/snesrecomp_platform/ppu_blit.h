#ifndef SNESRECOMP_PLATFORM_PPU_BLIT_H
#define SNESRECOMP_PLATFORM_PPU_BLIT_H
#include "snesrecomp_platform/snes_ppu_job_types.h"
#include "snesrecomp_platform/abi.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
enum { SNESRECOMP_BLIT_MAX_IMAGES = 8, SNESRECOMP_BLIT_MAX_WIDTH = 512,
       SNESRECOMP_BLIT_MAX_HEIGHT = 240, SNESRECOMP_BLIT_MAX_RECTS = 8192 };
#define SNESRECOMP_BLIT_EMPTY UINT32_MAX
/* A resolved source sample; no guest state, game addresses or priority rules.
 * Image: 3 bits, X: 9 bits, Y: 8 bits. Reserved high bits must be zero. */
static inline uint32_t snesrecomp_blit_sample(unsigned image, unsigned x, unsigned y) {
    return image < 8u && x < 512u && y < 240u ?
        (image << 17) | (y << 9) | x : SNESRECOMP_BLIT_EMPTY;
}
typedef struct SnesRecompBlitRect {
    uint16_t x, y, width, height, source_x, source_y;
    uint8_t image, reserved[3];
} SnesRecompBlitRect;
SNESRECOMP_STATIC_ASSERT(sizeof(SnesRecompBlitRect) == 16, "resolved blit SDK ABI");
SNESRECOMP_STATIC_ASSERT(offsetof(SnesRecompBlitRect, image) == 12, "resolved blit image ABI");
/* Immutable for the synchronous prepare/submit operation. A backend that
 * defers submission must own or pin every source, rectangle and this view.
 * Only opaque RGB replacement is supported. Source alpha is ignored; output
 * alpha is 255. Visibility/effects have already been resolved by the consumer.
 * Empty map samples preserve the base canvas. */
typedef struct SnesRecompBlitPlan {
    const SnesRecompPpuJobImage *images;
    const SnesRecompBlitRect *rects;
    uint32_t rect_count;
    uint16_t image_count, width, height, reserved;
} SnesRecompBlitPlan;
/* Compiles disjoint horizontal runs and joins compatible runs vertically.
 * Bounded scratch storage, no allocations. On failure *count is zero;
 * scratch rectangles must not be published. Sources are only inspected. */
bool snesrecomp_blit_compile(const uint32_t *map, uint32_t pitch,
    const SnesRecompPpuJobImage *images, uint16_t image_count,
    uint16_t width, uint16_t height, SnesRecompBlitRect *rects,
    uint32_t capacity, uint32_t *count);
/* Persistent horizontal runs. The caller certifies unchanged map rows with
 * dirty[y] == false; the first call and recovery after rejection read all rows.
 * Image dimensions are validated on every call. No allocation during compile.
 * Output is identical to blit_compile, including vertical joins and ordering. */
typedef struct SnesRecompBlitCache SnesRecompBlitCache;
SnesRecompBlitCache *snesrecomp_blit_cache_create(uint16_t width, uint16_t height);
void snesrecomp_blit_cache_destroy(SnesRecompBlitCache *cache);
bool snesrecomp_blit_cache_compile(SnesRecompBlitCache *cache,
    const uint32_t *map, uint32_t pitch, const bool *dirty,
    const SnesRecompPpuJobImage *images, uint16_t image_count,
    SnesRecompBlitRect *rects, uint32_t capacity, uint32_t *count);
bool snesrecomp_blit_validate(const SnesRecompBlitPlan *plan);
/* Exact CPU oracle/fallback. Validates the complete plan and source/destination
 * separation before any write; never updates guest time or publishes flags. */
bool snesrecomp_blit_apply(const SnesRecompBlitPlan *plan,
    uint32_t *argb, uint32_t pitch);
/* Packed-pixel CPU composition using the same resolved plan, preserving all
 * source bits (including alpha). Validation and failure are transactional as
 * for apply. Empty samples retain the destination. */
bool snesrecomp_blit_copy(const SnesRecompBlitPlan *plan,
    uint32_t *argb, uint32_t pitch);
#ifdef __cplusplus
}
#endif
#endif
