#ifndef SNESRECOMP_PPU_MEMORY_H
#define SNESRECOMP_PPU_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "snesrecomp_platform/memory_compare.h"

/* Share the exact scalar-console comparison with consumer caches. Constant
 * memcpy of a merely halfword-aligned source becomes byte loads on ARM11;
 * the common helper proves alignment before issuing alias-safe word loads. */
static inline bool snesrecomp_ppu_words_equal(const uint16_t *a,
                                             const uint16_t *b, size_t count) {
    if (a == b) return true;
    return snesrecomp_u16_equal(a, b, count);
}

#endif
