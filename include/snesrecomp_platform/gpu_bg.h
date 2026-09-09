#ifndef SNESRECOMP_PLATFORM_GPU_BG_H
#define SNESRECOMP_PLATFORM_GPU_BG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "snesrecomp_platform/snes_ppu_capture.h"
#include "snesrecomp_platform/snes_ppu_mode7.h"
#include "snesrecomp_platform/snes_ppu_obj.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native GPU interface for SNES backgrounds. Upload before begin(), draw
 * layers back to front, then end(). begin()/end() own and present the scene. */

typedef struct SnesRecompGpuBgLayer {
    unsigned tilemap_word_addr;   /* PPU_bgTilemapAdr */
    unsigned tile_word_addr;      /* PPU_bgTileAdr */
    bool wide;                    /* 64 tiles across rather than 32 */
    bool tall;
    unsigned bpp;                 /* 2 or 4 */
    unsigned palette_base;        /* first CGRAM entry the layer's palettes use */

    /* Which cached tilemap texture this draw samples. A SNES layer is drawn
     * once per priority and both draws read the same tilemap, so the slot is
     * named rather than implied by draw order. */
    unsigned tilemap_slot;
    unsigned priority;            /* 0 or 1; tiles of the other one drop out */

    /* How far this layer reaches past the native 256-pixel window, per side.
     * Zero is normal and not a special case: widescreen is decided per layer,
     * and a layer excluded from the widen mask draws 256 columns while its
     * neighbours extend. */
    unsigned margin_left;
    unsigned margin_right;

    /* One entry per scanline, as the scroll registers stood when that line was
     * rendered. HDMA rewrites them mid-frame, which is why this is an array
     * and not two numbers. */
    const uint16_t *scroll_x;
    const uint16_t *scroll_y;

    /* Optional caller-built 64x64 RGBA tilemap, used for widened margins.
     * Texel (x,y) represents world tile (X,Y) modulo 64. */
    const uint8_t *tilemap_rgba;

    /* Content version. Each rotating GPU set tracks it independently. */
    uint32_t tilemap_serial;
} SnesRecompGpuBgLayer;

typedef struct SnesRecompGpuSemanticFrame {
    const SnesPpuFrameCapture *capture;
    SnesRecompGpuBgLayer layer[3];
    unsigned present_mask;

    /* Non-NULL selects the exact basic Mode 7 BG1 path. The portable
     * compiler owns all affine and admission semantics; the backend only
     * uploads the already-compiled lines and captured VRAM. `present_mask`
     * must then contain only BG1. */
    const SnesRecompMode7Line *mode7_lines;
    unsigned mode7_line_count;

    /* Sprite work the CPU has already qualified: selection, per-line limits,
     * OAM order, addressing and flip are settled, and what remains is eight
     * texels per sliver. NULL or an empty frame means this frame draws no
     * sprites; it never means "decide for yourself". */
    const SnesRecompObjFrame *obj;

    uint32_t *readback_pixels;
    size_t readback_pitch_bytes;
} SnesRecompGpuSemanticFrame;

/* True once the native presenter is up and the background programs have been
 * built. False on SDL, and false if the shaders were not compiled in. */
bool snesrecomp_gpu_bg_available(void);

/* True only when the native presenter also registered the optional Mode 7
 * fragment program. Ordinary Mode 1 rendering remains available if that
 * shader is absent or rejected by the driver. */
bool snesrecomp_gpu_mode7_available(void);

bool snesrecomp_gpu_bg_begin(unsigned width, unsigned lines,
                             unsigned extra_left);

/* Diagnostic-only logical target. Reference equivalence is measured before
 * display scaling or filtering; the current trace corpus is 342x224. */
bool snesrecomp_gpu_bg_begin_readback(unsigned width, unsigned lines,
                                      unsigned extra_left);

/* Unpacks whatever changed in VRAM into the tile atlases and rebuilds the 16
 * exact master-brightness palette levels. Call outside a scene. Returns the
 * number of 512-byte VRAM chunks that had to be redone -- zero on a frame that
 * only scrolled. */
unsigned snesrecomp_gpu_bg_upload(const uint16_t *vram,
                                  const uint16_t *cgram);

/* Paints CGRAM[0] over the picture area. This is the hardware's backdrop and
 * also the render target's clear: the backend has no clear call, and the layers blend,
 * so without it they would composite onto a two-frame-old display buffer. */
bool snesrecomp_gpu_bg_clear(void);

/* Starts from exact black, then paints CGRAM[0] on each non-blank scanline at
 * that line's exact 0..15 master brightness. The implementation groups lines
 * by the bounded brightness domain; it does not submit or upload per line.
 * The same mask is retained for subsequent layer draws. */
bool snesrecomp_gpu_bg_clear_raster(const uint8_t *brightness,
                                    const uint8_t *forced_blank);

/* Fills the picture area with black. Forced blank is not the backdrop colour:
 * the software renderer zeroes the row and ignores both CGRAM[0] and master
 * brightness, so a frame blanked for a scene change must go black regardless
 * of what the palette holds. */
bool snesrecomp_gpu_bg_clear_black(void);

bool snesrecomp_gpu_bg_draw(const SnesRecompGpuBgLayer *layer);

/* Exact TASK-04 Main/Sub semantic pipeline. It owns all scene transitions:
 * Main -> Sub -> final compositor -> display/readback. */
bool snesrecomp_gpu_bg_render_semantic(
    const SnesRecompGpuSemanticFrame *frame);

bool snesrecomp_gpu_bg_end(void);

/* EndScene, then a diagnostic-only finish and RGB-normalized CPU readback.
 * Production never calls this and therefore never gains a hard GPU wait. */
bool snesrecomp_gpu_bg_end_readback(uint32_t *pixels, size_t pitch_bytes);

/* Microseconds spent inside the calls above during the last frame, split into
 * the CPU-side unpacking and the GPU submission. */
void snesrecomp_gpu_bg_timings(unsigned *unpack_us, unsigned *submit_us);

/* Per-stage averages since the last read. Reading resets the accumulators.
 * flip_us includes display pacing. Returns false when no GPU frame completed. */
typedef struct SnesRecompGpuBgStages {
    unsigned frames;      /* frames these averages are over */
    unsigned upload_us;   /* atlas rows and palette into GPU memory */
    unsigned build_us;    /* tilemap and strip geometry, CPU side */
    unsigned begin_us;    /* begin the backend's render pass */
    unsigned submit_us;   /* uniforms, textures, draw calls */
    unsigned end_us;      /* finish and submit the render pass */
    unsigned flip_us;     /* enqueue the finished frame */
} SnesRecompGpuBgStages;

bool snesrecomp_gpu_bg_stages(SnesRecompGpuBgStages *out);

/* Diagnostic summary of the most recently completed GPU frame. */
typedef struct SnesRecompGpuBgProbe {
    unsigned dirty_chunks;      /* VRAM chunks re-unpacked last upload */
    unsigned tilemap_rebuilds;  /* tilemap textures rebuilt since the last probe */
    unsigned atlas_rows_copied; /* 4 KB atlas rows refreshed for this frame's copy */
    unsigned atlas4_nonzero;    /* non-zero pixels in the 4bpp atlas */
    unsigned tilemap_nonzero;   /* tilemap texels naming a tile other than 0 */
    unsigned palette_nonblack;  /* CGRAM entries that are not black */
    uint32_t backdrop;          /* CGRAM[0] as 0xRRGGBB */
    uint16_t first_tile;        /* tile index at tilemap texel 0 */
} SnesRecompGpuBgProbe;

void snesrecomp_gpu_bg_probe(SnesRecompGpuBgProbe *out);

/* Bumped whenever the tile cache re-unpacked anything, so a caller can key its
 * own derived data on VRAM having actually changed rather than on the frame
 * having advanced. */
uint32_t snesrecomp_gpu_bg_vram_generation(void);

#ifdef __cplusplus
}
#endif

#endif
