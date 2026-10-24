#ifndef SNESRECOMP_PLATFORM_SNES_PPU_TRACE_H
#define SNESRECOMP_PLATFORM_SNES_PPU_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "snesrecomp_platform/snes_ppu_capture.h"
#include "snesrecomp_platform/snes_ppu_job.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One SNES frame on disk, self-contained: no pointers into a live emulator, so
 * the CPU reference renderer and a GPU backend can be handed the same bytes
 * and their outputs compared without the game running.
 *
 * Little-endian and serialised field by field rather than fwrite of a struct,
 * because padding and enum width are compiler decisions. Layout:
 *
 *   0   12  magic "SNESPPUTRACE"
 *   12  4   format_version
 *   16  4   header_bytes
 *   20  8   total_bytes
 *   ..      frame id, geometry, counts, hashes, flags
 *   header_bytes   band_count * SNES_PPU_TRACE_BAND_BYTES
 *   ..      65536 bytes VRAM, 512 CGRAM
 *   ..      512 OAM and 32 high OAM, v4 only and only when the header's
 *           OAM sizes are non-zero
 */

#define SNES_PPU_TRACE_MAGIC "SNESPPUTRACE"
#define SNES_PPU_TRACE_MAGIC_BYTES 12
/* 2: added the raw register block per band and the widescreen layout policy.
 * Version 1 could describe a frame but not replay it -- the software renderer
 * is driven by registers, not by decoded fields, and the layout policy lives
 * outside the register file entirely. A v1 file is refused rather than
 * guessed at.
 * 3: adds exactly two host layout bytes: wsWindowExpandLayers and
 * wsWindowExpandWindows. The reader retains explicit v2 compatibility and
 * reports format_version=2; absent v3 policy is zero, never guessed.
 * 4: adds OAM -- 512 bytes of low table and 32 of high -- with its own
 * payload hash, plus the ten host OBJ policy bytes. A v2/v3 file predates
 * sprite authority entirely: it reads back with a NULL OAM pointer and stays
 * refused for any frame with OBJ enabled, rather than being replayed against
 * whatever OAM the running process happens to hold. */
#define SNES_PPU_TRACE_VERSION_V2 2u
#define SNES_PPU_TRACE_VERSION_V3 3u
#define SNES_PPU_TRACE_VERSION_V4 4u
/* 5: raster palettes, resolved maps and qualified OBJ pixels; extra hash. */
#define SNES_PPU_TRACE_VERSION 5u
/* 6: owned, ordered post-composition images/rectangles and job sequence.
 * Capture-only readers explicitly refuse these files rather than omit art. */
#define SNES_PPU_TRACE_JOB_VERSION 6u
/* 7/8 add band-local elastic source/destination segments to capture/job traces.
 * Writers retain v5/v6 bytes when no remapping is present. */
#define SNES_PPU_TRACE_ELASTIC_VERSION 7u
#define SNES_PPU_TRACE_ELASTIC_JOB_VERSION 8u
/* 9/10: source brightness in the former zero overlay byte; 10 adds elastic bands. */
#define SNES_PPU_TRACE_BRIGHTNESS_JOB_VERSION 9u
#define SNES_PPU_TRACE_ELASTIC_BRIGHTNESS_JOB_VERSION 10u
/* 11/12: owned BG source rectangles before post overlays; former reserved
 * header word is source count. 12 additionally stores elastic bands. */
#define SNES_PPU_TRACE_SOURCE_JOB_VERSION 11u
#define SNES_PPU_TRACE_ELASTIC_SOURCE_JOB_VERSION 12u
/* 13/14: resolved window coordinates per band (plain/elastic capture).
 * 15/16 combine them with source/post jobs (plain/elastic bands). */
#define SNES_PPU_TRACE_WINDOW_VERSION 13u
#define SNES_PPU_TRACE_ELASTIC_WINDOW_VERSION 14u
#define SNES_PPU_TRACE_WINDOW_JOB_VERSION 15u
#define SNES_PPU_TRACE_ELASTIC_WINDOW_JOB_VERSION 16u
/* 17: primitive job envelope. Two complete capture records retain the displayed
 * base and original graphics context, including independent palette histories,
 * resolved windows/maps and qualified OBJ/sliver data. Ordered primitive,
 * material and image payloads are field-serialised and bounded. Nested job or
 * envelope records are refused; older capture/job formats remain unchanged. */
#define SNES_PPU_TRACE_PRIMITIVE_JOB_VERSION 17u
/* 18: owned indexed layout/axes, original qualified/clipped OBJ runs, source
 * materials, row authority and independent displayed/graphics captures. */
#define SNES_PPU_TRACE_INDEXED_JOB_VERSION 18u

enum {
    SNES_PPU_TRACE_VRAM_BYTES = 0x10000,   /* 64 KiB, the whole of VRAM */
    SNES_PPU_TRACE_CGRAM_BYTES = 0x200,    /* 256 entries of BGR555 */
    SNES_PPU_TRACE_OAM_BYTES = 0x200,      /* 128 sprites of four bytes */
    SNES_PPU_TRACE_HIGH_OAM_BYTES = 0x20,  /* size and X bit 8, two per slot */
};

/* Set when the frame's margins are drawn from the widescreen shadow, whose
 * contents this version does not serialise. Such a trace is reproducible only
 * against the same shadow state, so the reference corpus should prefer frames
 * without it until the shadow is captured too. Recorded rather than hidden. */
#define SNES_PPU_TRACE_FLAG_NEEDS_WS_SHADOW 0x1u
#define SNES_PPU_TRACE_FLAG_RASTER_VRAM      0x2u
#define SNES_PPU_TRACE_FLAG_RASTER_CGRAM     0x4u
#define SNES_PPU_TRACE_FLAG_RASTER_OAM       0x8u
#define SNES_PPU_TRACE_FLAG_RASTER_UNKNOWN   0x10u

typedef struct SnesPpuTrace {
    uint32_t format_version;
    uint32_t frame_id;
    uint32_t flags;

    /* The capture, with `bands`, `vram` and `cgram` pointing into the buffers
     * below. Valid until snesrecomp_ppu_trace_free(). */
    SnesPpuFrameCapture capture;

    /* What the support predicate said when the trace was taken. Stored so a
     * later run can tell "the renderer changed its mind" from "the trace is
     * different". */
    uint32_t accepted;
    uint32_t reject_reason;      /* SnesPpuUnsupported */

    uint64_t vram_hash;
    uint64_t cgram_hash;
    uint64_t capture_hash;
    uint64_t oam_hash;           /* zero when the file carries no OAM */

    /* Owned by the trace. `oam` and `high_oam` are NULL below v4. */
    SnesPpuRasterBand *bands;
    uint16_t *vram;
    uint16_t *cgram;
    uint16_t *oam;
    uint8_t *high_oam;
    uint16_t *cgram_lines;
    uint16_t *obj_pixels;
    uint8_t *resolved_bg[SNES_PPU_BG_COUNT];
    SnesPpuResolvedWindow *resolved_windows;
} SnesPpuTrace;

/* A stable 64-bit hash. FNV-1a: not cryptographic, but deterministic across
 * compilers and platforms, which is the only property a diagnostic needs. */
uint64_t snesrecomp_ppu_trace_hash(const void *data, size_t bytes);

/* Writes `capture` and the memory it references to `path`.
 *
 * `vram` and `cgram` must be the contents as of this capture -- a snapshot
 * from the following frame describes a frame that never existed. Returns false
 * and leaves no file behind on any error. */
bool snesrecomp_ppu_trace_write(const char *path,
                                const SnesPpuFrameCapture *capture,
                                uint32_t frame_id,
                                uint32_t accepted,
                                uint32_t reject_reason);

/* Reads a trace. On success `out` owns its buffers and must be released with
 * snesrecomp_ppu_trace_free(). On failure `out` is zeroed and nothing is
 * allocated. `error` may be NULL; otherwise it receives a short reason. */
bool snesrecomp_ppu_trace_read(const char *path, SnesPpuTrace *out,
                               char *error, size_t error_bytes);

bool snesrecomp_ppu_trace_write_job(const char *path,const SnesRecompPpuJob *job,
    uint32_t frame_id,uint32_t accepted,uint32_t reject_reason);
/* Also reads older v2-v5 captures into empty post jobs, subject to their
 * existing incomplete-memory restrictions. On success the trace and job have
 * independent ownership; release either first. Failure zeroes both outputs. */
bool snesrecomp_ppu_trace_read_job(const char *path,SnesPpuTrace *out,
    SnesRecompPpuJob **job,char *error,size_t error_bytes);

void snesrecomp_ppu_trace_free(SnesPpuTrace *trace);

#ifdef __cplusplus
}
#endif

#endif
