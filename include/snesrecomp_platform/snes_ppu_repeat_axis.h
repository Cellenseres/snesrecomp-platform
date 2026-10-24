#ifndef SNESRECOMP_PLATFORM_SNES_PPU_REPEAT_AXIS_H
#define SNESRECOMP_PLATFORM_SNES_PPU_REPEAT_AXIS_H
#include <stdint.h>
#include "snesrecomp_platform/abi.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Compact, exact nine-slice/repeating coordinate runs. These are geometry,
 * never pixel colours or guest state. A run is one-to-one (step=1), or repeats
 * a single source texel (step=0); no filtering/scaling is implied. */
typedef struct SnesRecompPpuAxisRun {
    uint16_t destination, source, count;
    uint8_t step, reserved;
} SnesRecompPpuAxisRun;
SNESRECOMP_STATIC_ASSERT(sizeof(SnesRecompPpuAxisRun)==8,"repeat axis ABI");
enum { SNESRECOMP_AXIS_OK=1, SNESRECOMP_AXIS_INVALID=0, SNESRECOMP_AXIS_CAPACITY=2 };
/* Preserve source/2 edge texels for sources under32, otherwise16. Repeat the
 * interior, with a single centre texel for an empty interior. begin/end clip
 * in target coordinates; source/target fit uint16. On capacity failure count
 * gives the required runs and output is untouched. NULL/zero probes capacity.
 * Input/output arrays must not alias count. No allocation or retained views. */
int32_t snesrecomp_ppu_repeat_axis(uint32_t source,uint32_t target,
    uint32_t begin,uint32_t end,SnesRecompPpuAxisRun *output,uint32_t capacity,uint32_t *count);
#ifdef __cplusplus
}
#endif
#endif
