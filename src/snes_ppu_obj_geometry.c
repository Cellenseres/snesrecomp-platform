#include "snesrecomp_platform/snes_ppu_obj_geometry.h"
#include <string.h>

bool snesrecomp_ppu_obj_geometry(const SnesRecompPpuRawRegion *regions,
    uint32_t region_count, const uint8_t *classes, uint16_t pitch,
    uint16_t width, uint16_t height, SnesRecompObjRectangle *rects,
    uint32_t capacity, SnesRecompObjGeometry *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!regions || !classes || !rects || !region_count ||
        region_count > SNESRECOMP_RAW_REGION_LIMIT || !width || width > 512 ||
        !height || height > 240 || pitch < (width + 7u) / 8u) return false;
    for (unsigned i = 0; i < region_count; ++i) {
        const SnesRecompPpuRawRegion *r = regions + i;
        if (r->x0 >= r->x1 || r->x1 > width || r->y0 >= r->y1 || r->y1 > height ||
            (r->flags & ~63u)) return false;
        for (unsigned j = 0; j < i; ++j)
            if (r->x0 < regions[j].x1 && regions[j].x0 < r->x1 &&
                r->y0 < regions[j].y1 && regions[j].y0 < r->y1) return false;
    }
    SnesRecompObjGeometry plan = {0};
    unsigned used = 0;
    for (unsigned group = 0; group < SNESRECOMP_OBJ_GEOMETRY_GROUPS; ++group) {
        const unsigned material = group / 8u, bit = 1u << (group % 8u);
        plan.first[group] = used;
        for (unsigned ri = 0; ri < region_count; ++ri) {
            const SnesRecompPpuRawRegion *r = regions + ri;
            if (material == 4 ? !(r->flags & SNESRECOMP_RAW_OBJ_SUB) :
                (!(r->flags & SNESRECOMP_RAW_OBJ_MAIN) || material !=
                    (!!(r->flags & SNESRECOMP_RAW_MAIN_RGB) +
                     2u * !!(r->flags & SNESRECOMP_RAW_OBJ_MATH)))) continue;
            uint32_t previous[64];
            for (unsigned x = 0; x < 64; ++x) previous[x] = UINT32_MAX;
            for (unsigned by = r->y0 / 8u; by < (r->y1 + 7u) / 8u; ++by)
                for (unsigned bx = r->x0 / 8u; bx < (r->x1 + 7u) / 8u;) {
                    if (!(classes[by * pitch + bx] & bit)) { ++bx; continue; }
                    const unsigned first = bx++;
                    while (bx < (r->x1 + 7u) / 8u && (classes[by * pitch + bx] & bit)) ++bx;
                    const uint16_t x0 = first * 8u < r->x0 ? r->x0 : first * 8u;
                    const uint16_t x1 = bx * 8u > r->x1 ? r->x1 : bx * 8u;
                    const uint16_t y0 = by * 8u < r->y0 ? r->y0 : by * 8u;
                    const uint16_t y1 = (by + 1u) * 8u > r->y1 ? r->y1 : (by + 1u) * 8u;
                    const uint32_t old = previous[first];
                    if (old != UINT32_MAX && rects[old].x0 == x0 &&
                        rects[old].x1 == x1 && rects[old].y1 == y0) rects[old].y1 = y1;
                    else {
                        if (used == capacity) return false;
                        previous[first] = used;
                        rects[used++] = (SnesRecompObjRectangle){x0, y0, x1, y1};
                    }
                }
        }
        plan.count[group] = used - plan.first[group];
    }
    plan.rectangle_count = used;
    *out = plan;
    return true;
}
