#ifndef SNESRECOMP_PLATFORM_REFRESH_CLOCK_H
#define SNESRECOMP_PLATFORM_REFRESH_CLOCK_H
#include <stdint.h>

/* The runner exempts a jump exceeding 4096 scanlines and clears its phase.
 * Values above that threshold return the same teleport sentinel; no caller
 * may use that sentinel to add refresh clocks. Unsigned phase wrap remains
 * the runner's original phase += delta operation before this conversion. */
static inline uint32_t snesrecomp_refresh_charge_lines(uint64_t phase) {
    if(phase<1364u) return 0;
    if(phase>=UINT64_C(4097)*1364u) return 4097;
    return (uint32_t)phase/1364u;
}
#endif
