#include "snesrecomp_platform/snes_ppu_semantics.h"

uint8_t snesrecomp_ppu_window_combine(bool window1_enabled,
                                      uint8_t window1_regions,
                                      bool window2_enabled,
                                      uint8_t window2_regions,
                                      uint8_t logic) {
    if (!window1_enabled)
        return window2_enabled ? window2_regions : 0;
    if (!window2_enabled)
        return window1_regions;

    switch (logic & 3u) {
    case 0: return (uint8_t)(window1_regions | window2_regions); /* OR */
    case 1: return (uint8_t)(window1_regions & window2_regions); /* AND */
    case 2: return (uint8_t)(window1_regions ^ window2_regions); /* XOR */
    default:
        return (uint8_t)~(window1_regions ^ window2_regions);    /* XNOR */
    }
}

uint16_t snesrecomp_ppu_fixed_colour_write(uint16_t current, uint8_t value) {
    const uint16_t component = (uint16_t)(value & 0x1fu);
    if (value & 0x20u)
        current = (uint16_t)((current & ~(0x1fu << 0)) | component);
    if (value & 0x40u)
        current = (uint16_t)((current & ~(0x1fu << 5)) | (component << 5));
    if (value & 0x80u)
        current = (uint16_t)((current & ~(0x1fu << 10)) | (component << 10));
    return current;
}
