#ifndef SNESRECOMP_PLATFORM_SNES_PPU_FRAME_H
#define SNESRECOMP_PLATFORM_SNES_PPU_FRAME_H

#include "snesrecomp_platform/snes_ppu_obj.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Host-thread ownership; workers receive const views after preparation. */
typedef struct SnesRecompPpuFrame SnesRecompPpuFrame;
/* Content certificates derived from owned byte comparisons during freeze.
 * Owner identities never repeat; versions are meaningful only within owner.
 * objects certifies the qualified pixel plane, not the optional sliver list.
 * They are CPU cache hints, never proof that a GPU resource was uploaded. */
typedef struct SnesRecompPpuFrameVersions {
    uint64_t owner, vram, palette, objects;
} SnesRecompPpuFrameVersions;

/* Optional host-side certificate for equality between two owned snapshots.
 * No frame pointers are retained, so this cache cannot extend a GPU lifetime
 * or prevent the next freeze. It covers VRAM, palette history and qualified
 * OBJ pixels only; raster registers, layout and original OAM are separate. */
typedef struct SnesRecompPpuFrameMemoryPair {
    SnesRecompPpuFrameVersions first, second;
    uint16_t width, height;
    bool valid, equal;
} SnesRecompPpuFrameMemoryPair;

typedef enum SnesRecompPpuResult {
    SNESRECOMP_PPU_OK = 0,
    SNESRECOMP_PPU_INVALID,
    SNESRECOMP_PPU_INCOMPLETE_MEMORY,
    SNESRECOMP_PPU_INCOMPLETE_LAYOUT,
    SNESRECOMP_PPU_UNSUPPORTED,
    SNESRECOMP_PPU_RESOURCE_LIMIT,
    SNESRECOMP_PPU_BUSY,
    SNESRECOMP_PPU_BACKEND_ERROR,
    SNESRECOMP_PPU_RESULT_COUNT
} SnesRecompPpuResult;

enum {
    SNESRECOMP_PPU_MAX_WIDTH = 512,
    SNESRECOMP_PPU_RESOLVED_MAP_BYTES = 64 * 64 * 4
};

/* Allocate once. Freeze never allocates or consults live guest state. */
SnesRecompPpuFrame *snesrecomp_ppu_frame_create(void);
void snesrecomp_ppu_frame_retain(const SnesRecompPpuFrame *frame);
void snesrecomp_ppu_frame_release(const SnesRecompPpuFrame *frame);

/* An optional qualified OBJ list replaces portable sprite selection.
 * Neither path publishes status flags into the guest. The timeline owns that.
 * Failure leaves the previous snapshot intact. Retained frames are immutable. */
SnesRecompPpuResult snesrecomp_ppu_frame_freeze(
    SnesRecompPpuFrame *frame, const SnesPpuFrameCapture *capture,
    const SnesRecompObjFrame *qualified_obj, uint64_t sequence);
/* An owned raster view shares only an already immutable snapshot's memory.
 * Bands are copied and validated; all other capture inputs are inherited.
 * The storage owner remains retained, so it cannot be frozen while in use.
 * No live guest pointers, allocation, memory scans or sprite selection. */
SnesRecompPpuResult snesrecomp_ppu_frame_freeze_raster_view(
    SnesRecompPpuFrame *frame, const SnesRecompPpuFrame *source,
    const SnesPpuRasterBand *bands, uint16_t band_count, uint64_t sequence);
/* Release a derived storage dependency before capturing its source again.
 * Retained views return BUSY unchanged; ordinary owned frames are untouched. */
SnesRecompPpuResult snesrecomp_ppu_frame_discard_raster_view(SnesRecompPpuFrame *frame);
const SnesPpuFrameCapture *snesrecomp_ppu_frame_capture(
    const SnesRecompPpuFrame *frame);
const SnesRecompObjFrame *snesrecomp_ppu_frame_obj(
    const SnesRecompPpuFrame *frame);
/* One bit per eight canvas pixels, one 64-bit word per row. Derived from the
 * validated qualified plane, never guest sprite selection. NULL without it. */
const uint64_t *snesrecomp_ppu_frame_obj_coverage(const SnesRecompPpuFrame *frame);
const SnesRecompPpuFrameVersions *snesrecomp_ppu_frame_versions(const SnesRecompPpuFrame *frame);
bool snesrecomp_ppu_frame_same_memory(const SnesRecompPpuFrame *first,
    const SnesRecompPpuFrame *second, SnesRecompPpuFrameMemoryPair *cache);
uint64_t snesrecomp_ppu_frame_sequence(const SnesRecompPpuFrame *frame);
const char *snesrecomp_ppu_result_text(SnesRecompPpuResult result);

#ifdef __cplusplus
}
#endif
#endif
