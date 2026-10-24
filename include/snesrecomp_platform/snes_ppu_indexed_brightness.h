#ifndef SNESRECOMP_PLATFORM_SNES_PPU_INDEXED_BRIGHTNESS_H
#define SNESRECOMP_PLATFORM_SNES_PPU_INDEXED_BRIGHTNESS_H
#include "snesrecomp_platform/snes_ppu_job.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Optional immutable indexed-job transform. Separate uniform final INIDISP
 * brightness from SNES composition, so a backend can reuse its unshaded image.
 * No guest access or pixel work. Unsupported inputs retain the original job.
 * Reset after submission/cancellation BEFORE the consumer freezes its frames. */
typedef struct SnesRecompIndexedBrightness SnesRecompIndexedBrightness;
SnesRecompIndexedBrightness *snesrecomp_ppu_indexed_brightness_create(void);
void snesrecomp_ppu_indexed_brightness_destroy(SnesRecompIndexedBrightness *context);
void snesrecomp_ppu_indexed_brightness_reset(SnesRecompIndexedBrightness *context);
const SnesRecompPpuJob *snesrecomp_ppu_indexed_brightness_prepare(
    SnesRecompIndexedBrightness *context,const SnesRecompPpuJob *source,uint8_t *brightness);
#ifdef __cplusplus
}
#endif
#endif
