#ifndef SNESRECOMP_PLATFORM_SNES_PPU_MODE7_H
#define SNESRECOMP_PLATFORM_SNES_PPU_MODE7_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "snesrecomp_platform/snes_ppu_capture.h"
#include "snesrecomp_platform/snes_bg_strips.h"
#include "snesrecomp_platform/snes_ppu_obj.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-output-line affine state for the exact, basic SNES Mode 7 subset.
 *
 * Coordinates are 8.8 fixed point. The native compiler reduces them modulo
 * the 1024x1024 Mode 7 plane (18 significant bits); a validated host source
 * may lift them into a larger logical plane afterwards. A GPU backend
 * advances them by step_x/step_y for every native SNES screen pixel. Keeping
 * this compiler portable makes the delicate signed 13-bit scroll and flip
 * rules testable on a desktop; the console adapter only uploads and consumes
 * the result. */
typedef struct SnesRecompMode7Line {
    uint32_t start_x;
    uint32_t start_y;
    int32_t step_x;
    int32_t step_y;
} SnesRecompMode7Line;

/* Optional logical tile-number plane for recompilation enhancements that can
 * see more source data than the SNES' 128x128 Mode 7 VRAM ring. Character
 * pixels and palette data still come from the captured VRAM/CGRAM, so this
 * replaces only the low-byte tile-number lookup. Coordinates wrap at the
 * declared plane dimensions, in 8.8 fixed point. */
typedef struct SnesRecompMode7MapSource {
    const uint8_t *tiles;
    unsigned width_tiles;
    unsigned height_tiles;
} SnesRecompMode7MapSource;

enum {
    SNESRECOMP_MODE7_COORD_MASK = 0x3ffffu,
    SNESRECOMP_MODE7_TEXTURE_DIM = 128u,
    SNESRECOMP_MODE7_TEXTURE_TEXELS = 128u * 128u,
};

/* Exact first production subset:
 *   - Mode 7 BG1 on the main screen, optionally with captured OBJ
 *   - per-band matrices, center/scroll and X/Y flip
 *   - bounded, non-repeating BG1 canvas margins
 *   - backdrop and master brightness through the semantic compositor
 *
 * BG1 main-screen windows use the captured per-band SNES geometry plus the
 * captured widescreen window-expansion policy; OBJ uses the corresponding
 * captured per-pixel main-screen permission after OAM-order resolution.
 * OBJ on the subscreen and the standard SNES colour-math path are supported,
 * including colour windows, fixed colour, add/subtract and half colour.
 * Direct-colour Mode 7, large-field/character-fill, effective mosaic, ExtBG,
 * non-OBJ subscreens and raster-time memory writes fail closed. */
SnesPpuUnsupported snesrecomp_ppu_mode7_supports(
    const SnesPpuFrameCapture *cap);

SnesPpuUnsupported snesrecomp_ppu_mode7_backend_supports(
    const SnesPpuFrameCapture *cap, bool backend_available);

/* Compile one entry per visible output line. Capture row zero represents PPU
 * scanline one, matching the software renderer and the existing semantic GXM
 * mapping. Call only for captures accepted by snesrecomp_ppu_mode7_supports. */
bool snesrecomp_ppu_mode7_compile_lines(
    const SnesPpuFrameCapture *cap, SnesRecompMode7Line *lines,
    unsigned capacity);

/* Build one full native-width quad per line. `wx/wy` carry 8.8 fixed-point
 * Mode 7 coordinates, not ordinary BG pixel coordinates. Values at the strip
 * edges are biased by half a pixel so interpolation at fragment centres lands
 * on the exact integer start value for screen X=0. The fragment backend wraps
 * them with SNESRECOMP_MODE7_COORD_MASK after interpolation. */
unsigned snesrecomp_ppu_mode7_build_strips(
    SnesRecompBgStripVertex *verts, uint16_t *indices,
    const SnesRecompMode7Line *lines, unsigned line_count,
    unsigned canvas_width, unsigned canvas_extra,
    float scale_x, float scale_y);

/* Split the Mode 7 interpretation of VRAM into two compact U8 textures.
 * `map_tex[i]` is the low byte used as the tile number; `char_tex[i]` is the
 * high byte used as the palette index at tile*64 + row*8 + column. They are
 * separate views of the same first 16K VRAM words, exactly as the PPU reads
 * them. Either destination may be NULL when a backend only refreshes one. */
bool snesrecomp_ppu_mode7_unpack_vram(const uint16_t *vram,
                                      uint8_t *map_tex,
                                      uint8_t *char_tex);

/* Slow, deterministic layer-resolved palette-index reference for backend
 * validation. Colour math is intentionally not representable in this U8
 * result; use the ARGB8888 reference below for finished pixels. It follows
 * the HD fragment-centre convention used by the OpenGL path and chooses the
 * matching native raster line for every subrow. `pixels` is a
 * canvas_width*scale by visible_height*scale U8 buffer; pitch is in bytes.
 * Negative and greater-than-255 screen X values inside each band's declared
 * margins use the same affine pixel-centre rules as the native viewport.
 * Coordinates use the same 18-bit wrap rules as the native viewport. Forced
 * blank is palette index zero. */
bool snesrecomp_ppu_mode7_render_reference(
    const SnesPpuFrameCapture *cap,
    const SnesRecompMode7Line *lines,
    unsigned line_count,
    const SnesRecompObjFrame *obj,
    unsigned scale,
    uint8_t *pixels,
    size_t pitch);

/* Source-aware form used by host enhancements. A NULL source is exactly the
 * native 128x128 low-byte VRAM map used by the compatibility wrapper above. */
bool snesrecomp_ppu_mode7_render_reference_with_map(
    const SnesPpuFrameCapture *cap,
    const SnesRecompMode7Line *lines,
    unsigned line_count,
    const SnesRecompObjFrame *obj,
    const SnesRecompMode7MapSource *map_source,
    unsigned scale,
    uint8_t *pixels,
    size_t pitch);

/* Fully composited CPU reference. Output bytes are B,G,R,A, the in-memory
 * representation consumed by the platform's SNESRECOMP_PIXEL_FORMAT_ARGB8888
 * presenter path. This is the authoritative fallback for colour math, where
 * palette indices alone can no longer represent the finished pixel. `pitch`
 * is in bytes. */
bool snesrecomp_ppu_mode7_render_reference_argb8888_with_map(
    const SnesPpuFrameCapture *cap,
    const SnesRecompMode7Line *lines,
    unsigned line_count,
    const SnesRecompObjFrame *obj,
    const SnesRecompMode7MapSource *map_source,
    unsigned scale,
    uint8_t *pixels,
    size_t pitch);

bool snesrecomp_ppu_mode7_map_source_valid(
    const SnesRecompMode7MapSource *map_source);

#ifdef __cplusplus
}
#endif

#endif
