#ifndef SNESRECOMP_PLATFORM_SNES_PPU_SEMANTICS_H
#define SNESRECOMP_PLATFORM_SNES_PPU_SEMANTICS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared CPU PPU semantics used by the live renderer and portable tests. */
uint8_t snesrecomp_ppu_window_combine(bool window1_enabled,
                                      uint8_t window1_regions,
                                      bool window2_enabled,
                                      uint8_t window2_regions,
                                      uint8_t logic);

/* Apply one COLDATA write while preserving components not selected by it. */
uint16_t snesrecomp_ppu_fixed_colour_write(uint16_t current, uint8_t value);

/* Exact RGB5 composition, expanded after arithmetic and master brightness. */
uint32_t snesrecomp_ppu_compose_colour(uint16_t main, uint16_t second,
    bool clip, bool math, bool subtract, bool half, unsigned brightness);
/* Complete native depth base, including BG layer id. Palette/index are added
 * afterward. Zero rejects invalid Mode-1 inputs; OBJ order is independent. */
uint16_t snesrecomp_ppu_mode1_bg_depth_base(uint8_t layer,uint8_t high,uint8_t bg3_priority);

#ifdef __cplusplus
}
#endif

#endif
