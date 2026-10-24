#ifndef SNESRECOMP_PLATFORM_SNES_PPU_OBJ_RUN_H
#define SNESRECOMP_PLATFORM_SNES_PPU_OBJ_RUN_H
#include "snesrecomp_platform/snes_ppu_obj.h"
#include "snesrecomp_platform/abi.h"
/* Original tile-fetch order after sprite/sliver limits, before pixel decoding.
 * begin/end are screen-space offsets 0..8, independent of horizontal flip.
 * Native coverage subtracts the overlay interval only with REMOVE_OVERLAY.
 * An empty interval draws nothing. Transparent atlas indices never overwrite
 * another OAM winner. Colour math/priority are applied after winner resolution.
 * owner is the consumer association at this original selection instant. */
enum { SNESRECOMP_OBJ_RUN_OVERLAY=1u,
       SNESRECOMP_OBJ_RUN_REMOVE_OVERLAY=2u,
       SNESRECOMP_OBJ_RUN_EFFECT=4u,
       SNESRECOMP_OBJ_RUN_MAX_ROW_WIDTH=1024 };
typedef struct SnesRecompPpuObjRun {
    SnesRecompObjSliver sliver;
    uint16_t owner;
    uint8_t begin,end,overlay_begin,overlay_end,flags,reserved;
} SnesRecompPpuObjRun;
SNESRECOMP_STATIC_ASSERT(sizeof(SnesRecompPpuObjRun)==20,"qualified OBJ run ABI");
#ifdef __cplusplus
extern "C" {
#endif
/* Pure recovery from a completed recorded row. Outputs use complete native
 * unsigned Z (including index), with transparent backdrop0x0500. The row
 * slice contains only this line, in original overwrite order. No OAM reads,
 * selection, status flags, live PPU access or allocation. Invalid inputs and
 * overlapping buffers fail before changing outputs. Owner output is optional.
 * A recovery row may include spare CPU margin storage; it does not expand a
 * backend's independently enforced logical canvas dimensions. */
bool snesrecomp_ppu_obj_run_row(const uint16_t *vram,
    const SnesRecompPpuObjRun *runs,uint32_t count,uint16_t line,
    uint16_t width,uint16_t extra,uint16_t *native,uint16_t *overlay,uint16_t *owners);
#ifdef __cplusplus
}
#endif
#endif
