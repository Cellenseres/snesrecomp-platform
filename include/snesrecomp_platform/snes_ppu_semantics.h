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

#ifdef __cplusplus
}
#endif

#endif
