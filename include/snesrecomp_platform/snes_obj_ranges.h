#ifndef SNESRECOMP_PLATFORM_SNES_OBJ_RANGES_H
#define SNESRECOMP_PLATFORM_SNES_OBJ_RANGES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Vertical candidates only. The authoritative evaluator still applies X/HUD
 * policy, rotating OAM order, range/sliver limits and all status side effects.
 * Rows use the original uint8_t subtraction, including wrap at 255. Zero-
 * initialize before use; treat cache fields as private except for zero-reset. */
typedef struct SnesRecompObjRangeCache {
    uint32_t rows[256][4];
    uint16_t oam[256];
    uint8_t high_oam[32];
    uint8_t size_mode;
    bool interlace, valid;
} SnesRecompObjRangeCache;

/* Full source equality is checked on every request; advisory dirty counters
 * cannot hide OAM writes, resets or changes between scanlines. No allocation,
 * guest access or pointers into live OAM survive this call. size_mode: 0..7. */
const uint32_t *snesrecomp_obj_range_row(SnesRecompObjRangeCache *cache,
    const uint16_t oam[256], const uint8_t high_oam[32],
    unsigned size_mode, bool interlace, unsigned line);

/* First candidate in [begin,128), or 128. Two ordered walks (start..127,
 * then 0..start-1) retain the evaluator's original rotated range order. */
static inline unsigned snesrecomp_obj_range_next(const uint32_t row[4], unsigned begin) {
    if(begin>=128) return 128;
    unsigned word=begin>>5;
    uint32_t bits=row[word] & (UINT32_MAX<<(begin&31u));
    while(!bits) {
        if(++word==4) return 128;
        bits=row[word];
    }
    unsigned bit;
#if defined(__GNUC__) || defined(__clang__)
    bit=(unsigned)__builtin_ctz(bits);
#else
    bit=0;
    if(!(bits&0xffffu)) {bit+=16;bits>>=16;}
    if(!(bits&0xffu)) {bit+=8;bits>>=8;}
    if(!(bits&0xfu)) {bit+=4;bits>>=4;}
    if(!(bits&3u)) {bit+=2;bits>>=2;}
    if(!(bits&1u)) ++bit;
#endif
    return word*32u+bit;
}

#ifdef __cplusplus
}
#endif
#endif
