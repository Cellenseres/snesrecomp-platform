#ifndef SNESRECOMP_PLATFORM_PPU_INDEX_DELTA_H
#define SNESRECOMP_PLATFORM_PPU_INDEX_DELTA_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* A bit covers eight source pixels. A zero bit certifies identical indices;
 * changed bits conservatively cover their complete eight-pixel group. Pitches
 * are elements, not bytes. Inputs are borrowed only during this call. */
bool snesrecomp_ppu_index_delta_u16(const uint16_t *previous,const uint16_t *current,
    size_t previous_pitch,size_t current_pitch,unsigned width,unsigned height,uint64_t *rows);
bool snesrecomp_ppu_index_delta_u8(const uint8_t *previous,const uint8_t *current,
    size_t previous_pitch,size_t current_pitch,unsigned width,unsigned height,uint64_t *rows);
/* False for an invalid rectangle, so callers cannot accidentally certify it. */
bool snesrecomp_ppu_index_region_unchanged(const uint64_t *rows,unsigned width,unsigned height,
    unsigned x,unsigned y,unsigned rectangle_width,unsigned rectangle_height);
#ifdef __cplusplus
}
#endif
#endif
