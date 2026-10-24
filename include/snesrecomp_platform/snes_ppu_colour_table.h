#ifndef SNESRECOMP_PLATFORM_SNES_PPU_COLOUR_TABLE_H
#define SNESRECOMP_PLATFORM_SNES_PPU_COLOUR_TABLE_H

#include "snesrecomp_platform/memory_compare.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Saturated RGB5 palette deltas for host-resolved replacement art. Prepare
 * once per changed palette colour; sampling has no channel arithmetic. */
typedef struct SnesRecompPpuColourDelta {
    uint8_t channels[3][32];
} SnesRecompPpuColourDelta;

static inline void snesrecomp_ppu_colour_delta_prepare(
    SnesRecompPpuColourDelta *table,uint16_t original,uint16_t current) {
    for(unsigned channel=0;channel<3;++channel) {
        const unsigned shift=channel*5u;
        const int delta=(int)((current>>shift)&31u)-(int)((original>>shift)&31u);
        for(unsigned value=0;value<32;++value) {
            int result=(int)value+delta;
            if(result<0) result=0;
            if(result>31) result=31;
            table->channels[channel][value]=(uint8_t)result;
        }
    }
}

static inline uint16_t snesrecomp_ppu_colour_delta_rgb5(
    const SnesRecompPpuColourDelta *table,uint16_t colour) {
    return (uint16_t)(table->channels[0][colour&31u] |
        ((unsigned)table->channels[1][(colour>>5)&31u]<<5) |
        ((unsigned)table->channels[2][(colour>>10)&31u]<<10));
}

/* Exact fixed-colour composition, without per-pixel arithmetic. The caller
 * supplies the reference renderer's saturation/brightness maps. Subscreen
 * operands require their own path. Outputs contain RGB, without alpha. */
typedef struct SnesRecompPpuColourTable {
    uint32_t rgb[2][256];
    uint32_t channels[2][3][32];
    uint16_t palette[256], fixed;
    uint8_t brightness[63], half_brightness[64], flags;
    bool valid;
} SnesRecompPpuColourTable;

static inline uint32_t snesrecomp_ppu_colour_table_rgb5(
    const SnesRecompPpuColourTable *table, uint16_t colour, bool math) {
    return table->channels[math][0][colour & 31u] |
        table->channels[math][1][(colour >> 5) & 31u] |
        table->channels[math][2][(colour >> 10) & 31u];
}

/* Call after raster changes, once per span, never for each pixel. Palette-only
 * changes update their own entries. No allocation, guest access or GPU state. */
static inline void snesrecomp_ppu_colour_table_prepare(
    SnesRecompPpuColourTable *table, const uint16_t palette[256],
    const uint8_t brightness[63], const uint8_t half_brightness[64],
    uint16_t fixed, bool clip, bool subtract, bool half) {
    const uint8_t flags = (uint8_t)(clip | (subtract << 1) | (half << 2));
    fixed &= 0x7fffu;
    const bool rebuild = !table->valid || table->fixed != fixed || table->flags != flags ||
        !snesrecomp_u8_equal(table->brightness, brightness, 63) ||
        !snesrecomp_u8_equal(table->half_brightness, half_brightness, 64);
    if (rebuild) {
        for (unsigned channel = 0; channel < 3; ++channel) {
            const unsigned operand = (fixed >> (channel * 5)) & 31u;
            for (unsigned value = 0; value < 32; ++value) {
                const unsigned main = clip ? 0 : value;
                const unsigned result = subtract ? (main >= operand ? main - operand : 0) : main + operand;
                table->channels[0][channel][value] = (uint32_t)brightness[main] << (16 - channel * 8);
                table->channels[1][channel][value] = (uint32_t)(half ? half_brightness[result] : brightness[result]) << (16 - channel * 8);
            }
        }
        memcpy(table->brightness, brightness, 63);
        memcpy(table->half_brightness, half_brightness, 64);
        table->fixed = fixed;
        table->flags = flags;
    }
    if (rebuild || !snesrecomp_u16_equal(table->palette, palette, 256)) {
        for (unsigned index = 0; index < 256; ++index) {
            if (!rebuild && table->palette[index] == palette[index]) continue;
            table->rgb[0][index] = snesrecomp_ppu_colour_table_rgb5(table, palette[index], false);
            table->rgb[1][index] = snesrecomp_ppu_colour_table_rgb5(table, palette[index], true);
        }
        memcpy(table->palette, palette, sizeof table->palette);
    }
    table->valid = true;
}

#endif
