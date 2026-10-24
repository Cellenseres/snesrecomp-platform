#ifndef SNESRECOMP_PLATFORM_SNES_PPU_ELASTIC_H
#define SNESRECOMP_PLATFORM_SNES_PPU_ELASTIC_H
#include "snesrecomp_platform/snes_ppu_capture.h"
#include "snesrecomp_platform/snes_ppu_resample.h"
#include <stddef.h>

static inline bool snesrecomp_ppu_elastic_valid(const SnesPpuBgState *bg) {
    if (!bg || bg->elastic_count > SNES_PPU_ELASTIC_SEGMENTS) return false;
    int end = -32768;
    for (unsigned i = 0; i < bg->elastic_count; ++i) {
        const SnesPpuElasticSegment *s = &bg->elastic[i];
        if (s->source_begin < 0 || s->source_end > 256 ||
            s->source_end <= s->source_begin ||
            s->destination_end <= s->destination_begin ||
            s->destination_begin < end) return false;
        end = s->destination_end;
    }
    return true;
}

/* -1 denotes an uncovered destination. The DDA retains the segment's original
 * phase when its destination is clipped to the captured canvas. */
static inline bool snesrecomp_ppu_elastic_columns(const SnesPpuBgState *bg,
    unsigned width, unsigned extra, unsigned left, unsigned right, int16_t *out) {
    if (!out || !snesrecomp_ppu_elastic_valid(bg) || width < 256 ||
        extra > width - 256 || left > extra || right > width - extra - 256)
        return false;
    for (unsigned x = 0; x < width; ++x) out[x] = -1;
    for (unsigned i = 0; i < bg->elastic_count; ++i) {
        const SnesPpuElasticSegment *s = &bg->elastic[i];
        int begin = s->destination_begin, end = s->destination_end;
        if (begin < -(int)left) begin = -(int)left;
        if (end > 256 + (int)right) end = 256 + (int)right;
        if (begin >= end) continue;
        SnesRecompPpuResampleStep cursor = snesrecomp_ppu_resample_begin(
            (unsigned)(s->source_end-s->source_begin),
            (unsigned)(s->destination_end-s->destination_begin),
            (unsigned)(begin-s->destination_begin));
        for (int x = begin; x < end; ++x) {
            out[x+(int)extra]=(int16_t)(s->source_begin+cursor.index);
            snesrecomp_ppu_resample_next(&cursor);
        }
    }
    return true;
}
#endif
