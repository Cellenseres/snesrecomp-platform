#include "snesrecomp_platform/snes_ppu_semantics.h"
uint16_t snesrecomp_ppu_mode1_bg_depth_base(uint8_t layer,uint8_t high,uint8_t bg3_priority) {
    static const uint16_t low[]={0x8000,0x7100,0x1200},upper[]={0xc000,0xb100,0x3200};
    if(layer>=3 || high>1 || bg3_priority>1) return 0;
    return !high?low[layer]:layer==2 && bg3_priority?0xf200:upper[layer];
}

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

uint32_t snesrecomp_ppu_compose_colour(uint16_t main, uint16_t second,
    bool clip, bool math, bool subtract, bool half, unsigned brightness) {
    uint32_t result = 0xff000000u;
    if (brightness > 15) brightness = 15;
    for (unsigned channel = 0; channel < 3; ++channel) {
        int value = clip ? 0 : (main >> (5 * channel)) & 31;
        if (math) {
            int operand = (second >> (5 * channel)) & 31;
            value += subtract ? -operand : operand;
            if (value < 0) value = 0;
            if (half) value /= 2;
            if (value > 31) value = 31;
        }
        result |= (uint32_t)(((value * 8 + value / 4) * brightness) / 15) << (16 - 8 * channel);
    }
    return result;
}
