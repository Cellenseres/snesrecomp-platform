#ifndef SNESRECOMP_PLATFORM_SNES_PPU_RESAMPLE_H
#define SNESRECOMP_PLATFORM_SNES_PPU_RESAMPLE_H

/* Exact nearest-neighbour pixel-centre stepping:
 * floor((2 * offset + 1) * source_width / (2 * destination_width)).
 * Positive spans, source <=256, destination <=65535, offset < destination.
 * Division is confined to initialization; advancing uses adds and one carry.
 * Clipping starts at its original destination offset, never a new phase. */
typedef struct SnesRecompPpuResampleStep {
    unsigned index, remainder, denominator, step, carry;
} SnesRecompPpuResampleStep;

static inline SnesRecompPpuResampleStep snesrecomp_ppu_resample_begin(
    unsigned source_width, unsigned destination_width, unsigned offset) {
    const unsigned denominator = 2u * destination_width;
    const unsigned numerator = (2u * offset + 1u) * source_width;
    SnesRecompPpuResampleStep result = {
        numerator / denominator, numerator % denominator, denominator,
        source_width / destination_width, 2u * (source_width % destination_width)
    };
    return result;
}

static inline void snesrecomp_ppu_resample_next(SnesRecompPpuResampleStep *s) {
    s->index += s->step;
    s->remainder += s->carry;
    if (s->remainder >= s->denominator) {
        s->remainder -= s->denominator;
        ++s->index;
    }
}

#endif
