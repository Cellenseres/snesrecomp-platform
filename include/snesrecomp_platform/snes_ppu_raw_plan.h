#ifndef SNESRECOMP_PLATFORM_SNES_PPU_RAW_PLAN_H
#define SNESRECOMP_PLATFORM_SNES_PPU_RAW_PLAN_H
#include "snesrecomp_platform/snes_ppu_frame.h"
#include "snesrecomp_platform/abi.h"
#include "snesrecomp_platform/snes_ppu_raw_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Unshaded RGB5 levels stored as 8*n in ARGB8888. These are GPU composition
 * inputs, never a finished display image. All flags cross SDK boundaries as
 * fixed-width fields, not enums. */
typedef struct SnesRecompPpuRawCache SnesRecompPpuRawCache;
typedef struct SnesRecompPpuRawPlan {
    const SnesRecompPpuRawRegion *regions;
    /* OBJ alpha is (priority+1)*64-2 + original palette math eligibility:
     * 62/63,126/127,190/191,254/255. Zero is transparent. The main renderer
     * must retain eligibility separately from final opacity; sub alpha must
     * distinguish a genuine operand from the fixed-colour backdrop. */
    const uint32_t *objects;
    const uint64_t *object_coverage;
    uint32_t region_count;
    uint16_t width,height;
    uint64_t region_generation,object_generation;
    SnesPpuUnsupported unsupported;
    /* Derived during existing OBJ colour conversion, retained across frames.
     * Eight exact priority/eligibility bits per 8x8 block. No guest work. */
    const uint8_t *object_classes;
    uint16_t object_class_pitch;
} SnesRecompPpuRawPlan;

SnesRecompPpuRawCache *snesrecomp_ppu_raw_create(void);
void snesrecomp_ppu_raw_destroy(SnesRecompPpuRawCache *cache);
void snesrecomp_ppu_raw_reset(SnesRecompPpuRawCache *cache);
/* Initial exact subset: accepted Mode7 semantics, captured qualified OBJ and
 * captured master brightness. Window/material regions merge independently of
 * affine raster changes. Rejection exposes no executable partial plan.
 * Cache generations are CPU content proofs, never upload acknowledgements. */
SnesRecompPpuResult snesrecomp_ppu_mode7_raw_plan(SnesRecompPpuRawCache *cache,
    const SnesRecompPpuFrame *frame,SnesRecompPpuRawPlan *out);
/* Mode-1 region/material and qualified OBJ plane. BG pixels come from cached
 * mode1_raw_tiles geometry; both inputs are needed for an executable frame. */
SnesRecompPpuResult snesrecomp_ppu_mode1_raw_plan(SnesRecompPpuRawCache *cache,
    const SnesRecompPpuFrame *frame,SnesRecompPpuRawPlan *out);
/* Regions only, for qualified indexed OBJ geometry rendered on the GPU.
 * Never reads/converts the captured OBJ plane or allocates pixel resources.
 * objects/coverage/classes and object_generation are always absent. This is
 * material preparation only; it does not certify an executable OBJ frame. */
SnesRecompPpuResult snesrecomp_ppu_mode1_raw_regions(SnesRecompPpuRawCache *cache,
    const SnesRecompPpuFrame *frame,SnesRecompPpuRawPlan *out);

#ifdef __cplusplus
}
#endif
#endif
