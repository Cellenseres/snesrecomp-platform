#ifndef SNESRECOMP_PPU_OBJ_GEOMETRY_H
#define SNESRECOMP_PPU_OBJ_GEOMETRY_H
#include <stdbool.h>
#include <stdint.h>
#include "snesrecomp_platform/snes_ppu_raw_types.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { SNESRECOMP_OBJ_GEOMETRY_GROUPS = 40, SNESRECOMP_OBJ_GEOMETRY_RECTS = 4096 };
typedef struct SnesRecompObjRectangle { uint16_t x0, y0, x1, y1; } SnesRecompObjRectangle;
typedef struct SnesRecompObjGeometry {
    uint32_t first[SNESRECOMP_OBJ_GEOMETRY_GROUPS];
    uint32_t count[SNESRECOMP_OBJ_GEOMETRY_GROUPS];
    uint32_t rectangle_count;
} SnesRecompObjGeometry;
/* Each 8x8 block has an exact presence mask: bit priority*2+math-eligibility.
 * Pixels and sprite qualification already belong to the immutable frame.
 * Material groups 0..3 are main RGB/math, group 4 is subscreen. Within each
 * material, eight priority/eligibility classes are contiguous. Empty space
 * creates no geometry. A rectangle may include transparent texels, but must
 * never omit a selected class. Consumers retain their original alpha tests.
 * Rejection clears the executable counts; rectangles are scratch storage. */
bool snesrecomp_ppu_obj_geometry(const SnesRecompPpuRawRegion *regions,
    uint32_t region_count, const uint8_t *classes, uint16_t class_pitch,
    uint16_t width, uint16_t height, SnesRecompObjRectangle *rectangles,
    uint32_t capacity, SnesRecompObjGeometry *out);
#ifdef __cplusplus
}
#endif
#endif
