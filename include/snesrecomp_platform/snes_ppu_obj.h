#ifndef SNESRECOMP_PLATFORM_SNES_PPU_OBJ_H
#define SNESRECOMP_PLATFORM_SNES_PPU_OBJ_H

#include <stdbool.h>
#include <stdint.h>

#include "snesrecomp_platform/snes_ppu_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sprite evaluation, without sprite pixels.
 *
 * Per scanline the hardware first selects sprites -- in OAM order from a
 * rotating start, capped at 32 sprites and 34 eight-pixel slivers -- and then
 * draws the selected slivers into one OBJ line buffer. Within that buffer the
 * winner is OAM order alone: priority 0 loses to priority 3 at a lower OAM
 * index. Priority only places the resulting OBJ pixel among the backgrounds,
 * so splitting by priority before resolving OAM order is wrong.
 *
 * This header therefore stops at slivers. Drawing them in the order returned
 * here, with plain overwrite and no depth test, reproduces the line buffer.
 */

enum {
    /* 224 lines of the hardware's 34-sliver limit is 7,616. The margin above
     * that is for the host's optional no-sprite-limits mode, which removes
     * both caps; a frame that still exceeds this bound is refused rather than
     * truncated, because a truncated sprite list is a wrong frame. */
    SNESRECOMP_OBJ_MAX_SLIVERS = 8192,

    /* Hardware caps, applied unless the capture's render flags disable them. */
    SNESRECOMP_OBJ_LINE_SPRITES = 32,
    SNESRECOMP_OBJ_LINE_SLIVERS = 34,
};

/* One eight-pixel horizontal run of one sprite on one scanline: the unit the
 * hardware's tile fetch produces and the unit its 34-per-line limit counts. */
typedef struct SnesRecompObjSliver {
    int16_t screen_x;      /* SNES screen X of the left edge; may be negative */
    uint16_t line;         /* visible row */
    uint16_t tile;         /* 4bpp tile index, already wrapped into VRAM */
    uint8_t row;           /* 0..7, source row inside the tile, flip applied */
    uint8_t palette_base;  /* CGRAM index of the sprite's 16 colours */
    uint8_t priority;      /* 0..3, versus the backgrounds only */
    uint8_t flip_h;
    /* Which sprite this came from. Not needed to draw, but a mismatch is
     * otherwise reported as an anonymous pixel, and the first question about
     * a wrong sprite pixel is always which sprite it belongs to. */
    uint8_t oam_slot;
    /* Colour math reaches OBJ only through CGADSUB bit 4, and only for
     * palettes 4..7. The CPU renderer encodes the ineligible half as a
     * separate layer id rather than as a flag, which is why this travels with
     * the sliver instead of being recomputed from the palette downstream. */
    uint8_t math_eligible;
} SnesRecompObjSliver;

typedef struct SnesRecompObjFrame {
    SnesRecompObjSliver *slivers;
    unsigned capacity;
    unsigned count;

    /* Bit p set when the line carries at least one sliver of priority p. A
     * backend uses it to submit only the priority passes a line needs. */
    uint8_t *line_priority;
    unsigned line_capacity;

    /* The hardware overflow flags this evaluation would have raised. The
     * guest can read them back through $213E, so a GPU frame that skips the
     * software renderer still has to produce them. */
    bool range_over;
    bool time_over;
} SnesRecompObjFrame;

/* Evaluates every visible line of `cap` into `frame`. Reads only the capture:
 * no live PPU, no live OAM, no guest memory. Returns false when the capture
 * carries no OAM authority, when geometry does not fit, or when the sliver
 * bound is exceeded -- all of which mean "fall back", never "draw less". */
bool snesrecomp_ppu_obj_evaluate(const SnesPpuFrameCapture *cap,
                                 SnesRecompObjFrame *frame);

/* Clip-space geometry for the OBJ plane pass. */
typedef struct SnesRecompObjVertex {
    float x, y;        /* clip space */
    float u, v;        /* atlas pixels; H flip is a swapped U range */
    float palette;     /* CGRAM index of the sprite's palette */
    float flags;       /* priority + 4 * math_eligible */
} SnesRecompObjVertex;

#define SNESRECOMP_OBJ_SLIVER_VERTS 4u
#define SNESRECOMP_OBJ_SLIVER_INDICES 6u

/* Builds one quad per sliver, in evaluation order, so a single indexed draw
 * reproduces the line buffer's overwrite order. `scale_y` matches the
 * background strips: the semantic viewport's counter-scale that puts strip
 * edge N on target row N. Returns the vertex count, or 0 on any error. */
unsigned snesrecomp_ppu_obj_build_geometry(
    const SnesRecompObjFrame *frame,
    unsigned canvas_width, unsigned canvas_extra, unsigned lines,
    unsigned atlas_tiles_x, float scale_x, float scale_y,
    SnesRecompObjVertex *verts, uint16_t *indices, unsigned vert_capacity);

#ifdef __cplusplus
}
#endif

#endif
