#ifndef SNESRECOMP_PLATFORM_SNES_BITPLANES_H
#define SNESRECOMP_PLATFORM_SNES_BITPLANES_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
extern const uint32_t snesrecomp_ppu_plane_nibbles[256];
#ifdef __cplusplus
}
#endif

/* Unpack current VRAM words, without palette or cache dependencies. Nibble
 * seven is the leftmost pixel; horizontal flip only changes the nibble index. */
static inline uint32_t snesrecomp_ppu_spread_plane(uint32_t bits) {
    bits = (bits | bits << 12) & UINT32_C(0x000f000f);
    bits = (bits | bits << 6) & UINT32_C(0x03030303);
    return (bits | bits << 3) & UINT32_C(0x11111111);
}
static inline uint32_t snesrecomp_ppu_unpack_4bpp(uint32_t planes) {
    return snesrecomp_ppu_plane_nibbles[planes & 255u] |
        (snesrecomp_ppu_plane_nibbles[(planes >> 8) & 255u] << 1) |
        (snesrecomp_ppu_plane_nibbles[(planes >> 16) & 255u] << 2) |
        (snesrecomp_ppu_plane_nibbles[planes >> 24] << 3);
}
#endif
