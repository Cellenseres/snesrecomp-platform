#ifndef SNESRECOMP_PLATFORM_APU_CLOCK_H
#define SNESRECOMP_PLATFORM_APU_CLOCK_H
#include <stdint.h>

/* Preserve the runner's 17088/357368 frame-clock ratio exactly. Reducing by
 * eight keeps a bounded within-frame numerator below 2^32, so ARMv6 uses a
 * constant 32-bit quotient instead of a 64-bit division at every port sync.
 * Longer 32-bit intervals split into whole quotients and a bounded remainder.
 * Offsets beyond that domain retain the original expression and unsigned wrap.
 * This computes a host clock conversion only; it never advances the APU. */
static inline uint64_t snesrecomp_apu_frame_offset(uint64_t master_offset) {
    if (master_offset < UINT64_C(357368))
        return ((uint32_t)master_offset * UINT32_C(2136)) / UINT32_C(44671);
    if (master_offset <= UINT32_MAX) {
        const uint32_t whole = (uint32_t)master_offset / UINT32_C(44671);
        const uint32_t remainder = (uint32_t)master_offset % UINT32_C(44671);
        return whole * UINT32_C(2136) + remainder * UINT32_C(2136) / UINT32_C(44671);
    }
    return master_offset * UINT64_C(17088) / UINT64_C(357368);
}
#endif
