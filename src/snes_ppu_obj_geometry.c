#include "snesrecomp_platform/snes_ppu_obj_geometry.h"
#include <string.h>

/* Nonzero bit scans use fixed-width arithmetic on both ARM and desktop C. */
static unsigned ObjFirst32(uint32_t word) {
    static const uint8_t positions[32]={0,1,28,2,29,14,24,3,30,22,20,15,25,17,4,8,31,27,13,23,21,19,16,7,26,12,18,6,11,5,10,9};
    return positions[(uint32_t)((word & (0u-word))*UINT32_C(0x077cb531))>>27];
}
static unsigned ObjFirst64(uint64_t word) {
    const uint32_t low=(uint32_t)word;
    return low?ObjFirst32(low):32u+ObjFirst32((uint32_t)(word>>32));
}
static uint64_t ObjColumns(unsigned begin,unsigned end) {
    return (UINT64_MAX<<begin)&(end==64u?UINT64_MAX:(UINT64_C(1)<<end)-1u);
}

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
    /* Read each needed class cell once. Partial-region callers need not
     * provide class rows outside the original active regions' read set. */
    uint64_t needed[30]={0},occupied[8][30]={{0}};
    for(unsigned i=0;i<region_count;++i) {
        const SnesRecompPpuRawRegion *r=regions+i;
        if(!(r->flags&(SNESRECOMP_RAW_OBJ_MAIN|SNESRECOMP_RAW_OBJ_SUB)))continue;
        const uint64_t columns=ObjColumns(r->x0/8u,(r->x1+7u)/8u);
        for(unsigned y=r->y0/8u;y<(r->y1+7u)/8u;++y)needed[y]|=columns;
    }
    for(unsigned y=0;y<(height+7u)/8u;++y) {
        uint64_t cells=needed[y];
        while(cells) {
            const unsigned x=ObjFirst64(cells);cells&=cells-1u;
            unsigned mask=classes[y*pitch+x];
            while(mask) {
                const unsigned bit=ObjFirst32(mask);mask&=mask-1u;
                occupied[bit][y]|=UINT64_C(1)<<x;
            }
        }
    }
    SnesRecompObjGeometry plan = {0};
    unsigned used = 0;
    for (unsigned group = 0; group < SNESRECOMP_OBJ_GEOMETRY_GROUPS; ++group) {
        const unsigned material = group / 8u, bit_index=group%8u;
        plan.first[group] = used;
        for (unsigned ri = 0; ri < region_count; ++ri) {
            const SnesRecompPpuRawRegion *r = regions + ri;
            if (material == 4 ? !(r->flags & SNESRECOMP_RAW_OBJ_SUB) :
                (!(r->flags & SNESRECOMP_RAW_OBJ_MAIN) || material !=
                    (!!(r->flags & SNESRECOMP_RAW_MAIN_RGB) +
                     2u * !!(r->flags & SNESRECOMP_RAW_OBJ_MATH)))) continue;
            uint32_t previous[64];
            for (unsigned x = 0; x < 64; ++x) previous[x] = UINT32_MAX;
            const uint64_t columns=ObjColumns(r->x0/8u,(r->x1+7u)/8u);
            for (unsigned by = r->y0 / 8u; by < (r->y1 + 7u) / 8u; ++by) {
                uint64_t row=occupied[bit_index][by]&columns;
                while(row) {
                    const unsigned first=ObjFirst64(row);
                    const uint64_t inverse=~(row>>first);
                    const unsigned bx=first+(inverse?ObjFirst64(inverse):64u);
                    row=bx==64u?0:row&(UINT64_MAX<<bx);
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
        }
        plan.count[group] = used - plan.first[group];
    }
    plan.rectangle_count = used;
    *out = plan;
    return true;
}
