#ifndef SNESRECOMP_PLATFORM_SNES_BG_STRIPS_H
#define SNESRECOMP_PLATFORM_SNES_BG_STRIPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-line clip-space geometry carrying raster scroll in one draw call. */

typedef struct SnesRecompBgStripVertex {
    float x, y;     /* clip space, -1..1 */
    float wx, wy;   /* SNES world pixels at this corner */
} SnesRecompBgStripVertex;

/* Four vertices and two triangles per scanline. */
#define SNESRECOMP_BG_STRIP_VERTS 4u
#define SNESRECOMP_BG_STRIP_INDICES 6u

/* Scroll arrays contain one entry per line. Margins set each layer's extent
 * around the native 256-pixel window; scale places it in the output viewport.
 * Returns lines*4 vertices and writes lines*6 indices. */
unsigned snesrecomp_bg_build_strips(SnesRecompBgStripVertex *verts,
                                    uint16_t *indices,
                                    const uint16_t *scroll_x,
                                    const uint16_t *scroll_y,
                                    unsigned lines,
                                    unsigned canvas_width,
                                    unsigned canvas_extra,
                                    unsigned margin_left,
                                    unsigned margin_right,
                                    float scale_x,
                                    float scale_y);

#ifdef __cplusplus
}
#endif

#endif
