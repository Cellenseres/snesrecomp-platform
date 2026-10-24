#ifndef SNESRECOMP_PLATFORM_SNES_PPU_JOB_H
#define SNESRECOMP_PLATFORM_SNES_PPU_JOB_H

#include "snesrecomp_platform/snes_ppu_frame.h"
#include "snesrecomp_platform/snes_ppu_job_types.h"
#include "snesrecomp_platform/ppu_blit.h"
#include "snesrecomp_platform/snes_ppu_indexed_layout.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnesRecompPpuJob SnesRecompPpuJob;
enum {
    SNESRECOMP_PPU_JOB_MAX_PIXELS = SNESRECOMP_PPU_MAX_WIDTH * SNES_PPU_MAX_BANDS,
    SNESRECOMP_PPU_JOB_MAX_BLIT_PIXELS = SNESRECOMP_PPU_JOB_MAX_PIXELS * SNESRECOMP_BLIT_MAX_IMAGES
};

/* Host-resolved, unscaled ARGB8888 art. No game addresses or guest callbacks.
 * Padding is never copied, compared, uploaded or interpreted as pixels. */

/* Ordered one-to-one rectangles, already clipped by the consumer. Conditions
 * observe the logical destination after earlier overlays. KEY_OR_BLACK means
 * exact opaque ARGB equality to key_argb, or RGB zero. Blending is rounded /255
 * on an opaque canvas; PREMULTIPLIED requires every source RGB byte <= alpha.
 * All operations happen before final presentation scaling/filtering. */

typedef struct SnesRecompPpuJobView {
    const SnesRecompPpuFrame *frame;
    const SnesRecompPpuJobImage *images;
    const SnesRecompPpuOverlay *overlays;
    const uint64_t *image_versions;
    uint64_t owner, geometry_version, sequence;
    uint16_t image_count, overlay_count;
    const SnesRecompPpuSource *sources;
    uint16_t source_count;
    /* Optional opaque composition after PPU colour resolution, before scaling.
     * The job owns every image and rectangle until submit/cancel releases it. */
    const SnesRecompBlitPlan *blits;
    uint64_t blit_geometry_version;
    /* Separate immutable source context; the displayed base may omit UI BGs.
     * Owned references and geometry survive until submission/cancellation. */
    const SnesRecompPpuFrame *graphics_frame;
    const SnesRecompPpuPrimitive *primitives;
    const SnesRecompPpuMaterial *materials;
    uint64_t primitive_geometry_version;
    uint32_t primitive_count;
    uint16_t material_count;
    const SnesRecompPpuIndexedLayout *indexed_layout;
    uint64_t indexed_geometry_version;
    uint64_t indexed_obj_version;
} SnesRecompPpuJobView;

/* Two bounded payload buffers are allocated once, to permit safe re-freezing
 * from the job's own previous views, including image permutations. Freeze has
 * no allocation or guest side effects and changes nothing on failure. */
SnesRecompPpuJob *snesrecomp_ppu_job_create(uint32_t pixel_capacity);
/* Opt-in bounded storage for several full primitive images. Ordinary jobs
 * retain their original capacity limit and allocation size. No freeze allocates. */
SnesRecompPpuJob *snesrecomp_ppu_job_create_blits(uint32_t pixel_capacity,
    uint32_t rectangle_capacity);
SnesRecompPpuJob *snesrecomp_ppu_job_create_primitives(uint32_t pixel_capacity,
    uint32_t primitive_capacity);
/* Bounded, allocation-free layout freezes; no CPU index/colour images. A
 * backend must explicitly admit this payload before any legacy work is omitted. */
SnesRecompPpuJob *snesrecomp_ppu_job_create_indexed(uint32_t claim_capacity,
    uint32_t object_capacity,uint32_t axis_capacity);
SnesRecompPpuResult snesrecomp_ppu_job_freeze_indexed(SnesRecompPpuJob *job,
    const SnesRecompPpuFrame *frame,const SnesRecompPpuFrame *graphics_frame,
    const SnesRecompPpuIndexedLayout *layout,uint64_t sequence);
/* Transactional, allocation-free freeze. Geometry and assets become owned;
 * both frames are retained. No live guest pointers survive this operation. */
SnesRecompPpuResult snesrecomp_ppu_job_freeze_primitives(SnesRecompPpuJob *job,
    const SnesRecompPpuFrame *frame, const SnesRecompPpuFrame *graphics_frame,
    const SnesRecompPpuJobImage *images, uint16_t image_count,
    const SnesRecompPpuPrimitive *primitives, uint32_t primitive_count,
    const SnesRecompPpuMaterial *materials, uint16_t material_count,
    uint64_t sequence);
void snesrecomp_ppu_job_retain(const SnesRecompPpuJob *job);
void snesrecomp_ppu_job_release(const SnesRecompPpuJob *job);
/* After submission/cancellation, release the frame while retaining owned art
 * and monotonic cache versions. The public view is unavailable until the next
 * successful freeze. Busy jobs remain unchanged. No allocation or GPU wait. */
SnesRecompPpuResult snesrecomp_ppu_job_detach_frame(SnesRecompPpuJob *job);
SnesRecompPpuResult snesrecomp_ppu_job_freeze(SnesRecompPpuJob *job,
    const SnesRecompPpuFrame *frame, const SnesRecompPpuJobImage *images,
    uint16_t image_count, const SnesRecompPpuOverlay *overlays,
    uint16_t overlay_count, uint64_t sequence);
/* Own a complete resolved plan and PPU base atomically. Image pitches are
 * compacted without copying padding. Previous owned views may be input. */
SnesRecompPpuResult snesrecomp_ppu_job_freeze_blits(SnesRecompPpuJob *job,
    const SnesRecompPpuFrame *frame, const SnesRecompBlitPlan *plan, uint64_t sequence);
/* As freeze, also owning resolved BG source placement. Neither borrowed
 * metadata nor pixels are accessed after success. No allocation on freeze. */
SnesRecompPpuResult snesrecomp_ppu_job_freeze_sources(SnesRecompPpuJob *job,
    const SnesRecompPpuFrame *frame, const SnesRecompPpuJobImage *images,
    uint16_t image_count, const SnesRecompPpuOverlay *overlays,
    uint16_t overlay_count, const SnesRecompPpuSource *sources,
    uint16_t source_count, uint64_t sequence);
/* Reuse the already owned art, rectangles and conditions with another complete
 * frame of identical canvas dimensions. This deliberately does not consult
 * borrowed art or change a backdrop key. Use freeze when any overlay changes.
 * Constant work, no allocation or pixel comparison. Failure is transactional;
 * a presenter-retained job remains busy until submission/cancellation. */
SnesRecompPpuResult snesrecomp_ppu_job_rebind(SnesRecompPpuJob *job,
    const SnesRecompPpuFrame *frame, uint64_t sequence);
const SnesRecompPpuJobView *snesrecomp_ppu_job_view(const SnesRecompPpuJob *job);

/* Pure fallback/reference pixel work. Invalid dimensions, nonopaque logical
 * input or overlap with owned source art fail before changing the destination.
 * Never advances guest time, qualifies sprites, publishes flags or allocates. */
SnesRecompPpuResult snesrecomp_ppu_job_apply(const SnesRecompPpuJob *job,
    uint32_t *argb, uint16_t width, uint16_t height, uint32_t pitch);

#ifdef __cplusplus
}
#endif
#endif
