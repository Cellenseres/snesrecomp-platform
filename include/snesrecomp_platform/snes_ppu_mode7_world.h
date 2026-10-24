#ifndef SNESRECOMP_PLATFORM_SNES_PPU_MODE7_WORLD_H
#define SNESRECOMP_PLATFORM_SNES_PPU_MODE7_WORLD_H
#include "snesrecomp_platform/snes_ppu_frame.h"
#include "snesrecomp_platform/snes_ppu_mode7.h"
#include "snesrecomp_platform/snes_ppu_mode7_world_types.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { SNESRECOMP_MODE7_WORLD_SIDE = 1024, SNESRECOMP_MODE7_WORLD_CELLS = 128 * 128 };
typedef struct SnesRecompMode7WorldCache SnesRecompMode7WorldCache;
typedef struct SnesRecompMode7WorldPlan {
    /* Two precoloured planes: ordinary BG1, and exact fixed-colour math.
     * Alpha zero denotes transparent palette index zero. No guest state is
     * queried by a backend. Versions remain monotonic after reset/cancel.
     * Identical planes may alias; consumers must keep both views read-only. */
    const uint32_t *pixels[2];
    const uint64_t *versions[2];
    const SnesRecompMode7Line *lines;
    uint32_t changed_cells[2];
    uint64_t resource_generation[2];
    /* Raw plans carry 8*n RGB5 components instead of display colours. */
    uint32_t raw_rgb5;
    SnesPpuUnsupported unsupported;
    /* NULL denotes a complete plane. View preparation certifies only cells
     * whose bits are set, including conservative nearest-filter padding. */
    const uint32_t *resident_cells[2];
    uint32_t materialized_cells[2];
    /* Optional context-owned footprint certificate for exactly the returned
     * immutable geometry generation. Adapters may reuse it instead of deriving
     * identical padded cells again; residency must still be checked each time.
     * NULL/zero denotes a legacy plan without this certificate. */
    const uint32_t *sampled_cells[2];
    uint64_t sampled_geometry_generation;
} SnesRecompMode7WorldPlan;
typedef struct SnesRecompMode7WorldGeometry {
    const SnesRecompMode7WorldSpan *spans;
    unsigned count;
    uint64_t generation;
} SnesRecompMode7WorldGeometry;

/* Allocate before admission. This is a resource plan, not a finished image
 * or permission to omit the independent OBJ/backdrop/window compositor. */
SnesRecompMode7WorldCache *snesrecomp_mode7_world_create(void);
void snesrecomp_mode7_world_destroy(SnesRecompMode7WorldCache *cache);
void snesrecomp_mode7_world_reset(SnesRecompMode7WorldCache *cache);
SnesRecompPpuResult snesrecomp_mode7_world_prepare(SnesRecompMode7WorldCache *cache,
    const SnesRecompPpuFrame *frame, SnesRecompMode7WorldPlan *out);
/* Unshaded full-brightness input for an exact native main/sub compositor.
 * Both variants contain the same raw colours; geometry retains eligibility.
 * Unsupported brightness or palette histories expose no resource pointers. */
SnesRecompPpuResult snesrecomp_mode7_world_prepare_raw(SnesRecompMode7WorldCache *cache,
    const SnesRecompPpuFrame *frame, SnesRecompMode7WorldPlan *out);
/* Prepare only cells sampled by this exact captured geometry. Unresident
 * cells must never be sampled; adapters verify resident_cells at admission.
 * Later views repair newly visible cells, and full preparation repairs all.
 * Captures and geometry retain their original semantics and ownership. */
SnesRecompPpuResult snesrecomp_mode7_world_prepare_view(SnesRecompMode7WorldCache *cache,
    const SnesRecompPpuFrame *frame, bool raw, SnesRecompMode7WorldPlan *out,
    SnesRecompMode7WorldGeometry *geometry);
/* Bounded, integer-prepared affine strips. Sampling bias preserves every
 * source texel while leaving >=1/16 source pixel of precision reserve.
 * Adapters must use point filtering and bound accumulated error below that
 * reserve. A failed build has zero executable spans. */
SnesRecompPpuResult snesrecomp_mode7_world_spans(const SnesRecompPpuFrame *frame,
    const SnesRecompMode7WorldPlan *world, SnesRecompMode7WorldSpan *spans,
    unsigned capacity, unsigned *count);
/* Retained context-owned geometry, independent of VRAM/palette contents.
 * Cancellation does not acknowledge its GPU upload. */
SnesRecompPpuResult snesrecomp_mode7_world_cached_spans(SnesRecompMode7WorldCache *cache,
    const SnesRecompPpuFrame *frame, const SnesRecompMode7WorldPlan *world,
    SnesRecompMode7WorldGeometry *geometry);
#ifdef __cplusplus
}
#endif
#endif
