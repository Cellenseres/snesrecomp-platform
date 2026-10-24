#ifndef SNESRECOMP_PLATFORM_SNES_PPU_INDEXED_LAYOUT_H
#define SNESRECOMP_PLATFORM_SNES_PPU_INDEXED_LAYOUT_H
#include "snesrecomp_platform/snes_ppu_repeat_axis.h"
#include "snesrecomp_platform/snes_ppu_obj_run.h"
#include "snesrecomp_platform/snes_ppu_job_types.h"

/* Source coverage and ordered association claims, independent of RGB pixels.
 * plane indexes source_layers; layers 0..3 are BGs, 4 is qualified OBJ.
 * An ownership-only claim does not add coverage. Later claims replace the
 * previous association, including claims belonging to hidden objects. */
typedef struct SnesRecompPpuIndexedClaim {
    uint16_t x,y,width,height,owner;
    uint8_t plane,coverage;
} SnesRecompPpuIndexedClaim;
typedef struct SnesRecompPpuIndexedObject {
    int32_t x,y;
    uint32_t x_first,y_first;
    uint16_t source_x,source_y,owner,x_count,y_count;
    uint8_t plane,reserved;
} SnesRecompPpuIndexedObject;
SNESRECOMP_STATIC_ASSERT(sizeof(SnesRecompPpuIndexedClaim)==12,"indexed claim ABI");
SNESRECOMP_STATIC_ASSERT(sizeof(SnesRecompPpuIndexedObject)==28,"indexed object ABI");
enum { SNESRECOMP_PPU_INDEXED_MAX_CLAIMS=8192,
       SNESRECOMP_PPU_INDEXED_MAX_OBJECTS=4096,
       SNESRECOMP_PPU_INDEXED_MAX_AXES=32768,
       SNESRECOMP_PPU_INDEXED_MAX_PLANES=5 };
/* Axes describe exact source texels relative to each source/destination origin.
 * step=0 repeats a texel; step=1 copies consecutive texels. They never resample.
 * Original source coordinates retain the colour/window/raster context after
 * relocation. Each axis list is contiguous and clipped to the destination.
 * OBJ owners are consumer associations for already selected OAM slots; 255
 * means unassigned. These do not select sprites or replace per-line slivers.
 * Job freeze copies every array, retaining the immutable graphics frame. */
typedef struct SnesRecompPpuIndexedLayout {
    const SnesRecompPpuIndexedClaim *claims;
    const SnesRecompPpuIndexedObject *objects;
    const SnesRecompPpuAxisRun *axes;
    const uint8_t *oam_owners; /* exactly 128 entries */
    uint32_t claim_count,object_count,axis_count;
    uint8_t source_layers[SNESRECOMP_PPU_INDEXED_MAX_PLANES];
    uint8_t plane_count;
    /* Bit p requires the original source owner to match a catalogued object.
     * Unset bits allow overlapping objects to use the same source pixels.
     * Uncatalogued pixels keep native positions; unassigned OBJ owners retain
     * their original positions even inside another object's claimed region. */
    uint8_t owner_planes,reserved;
    uint16_t width,height;
    const SnesRecompPpuObjRun *obj_runs;
    uint32_t obj_run_count;
    /* Exactly one original selection (or forced-blank omission) per row.
     * Empty selections have their bit set, too. Never infer authority from
     * the mere presence of an OAM snapshot. */
    uint32_t qualified_lines[8];
    const SnesRecompPpuMaterial *materials;
    /* Row-major [graphics-frame raster band][plane]. Source context follows
     * the owned band's original coordinates, including after relocation. */
    const uint16_t *band_materials;
    uint16_t material_count,band_count;
} SnesRecompPpuIndexedLayout;
#endif
