#ifndef SNESRECOMP_PLATFORM_APU_PROGRAM_H
#define SNESRECOMP_PLATFORM_APU_PROGRAM_H

/* Native replacement for the game's SPC-700 program.
 * Needs cmake/SnesRecompPlatformApuProgram.cmake; clock, ports, timers and
 * S-DSP stay with the core. */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct Apu;

typedef struct SnesRecompApuProgram {
    void *context;
    /* At an interpreted opcode boundary: true takes over. */
    bool (*take_over)(void *context, struct Apu *apu);
    /* True while the native program owns the SPC. */
    bool (*active)(void *context, struct Apu *apu);
    /* A port write landed or a timer ticked. */
    void (*run)(void *context, struct Apu *apu);
    /* Every APU cycle while active, before the S-DSP; may be NULL. */
    void (*cycle)(void *context, struct Apu *apu);
    /* A savestate replaced the APU state; may be NULL. */
    void (*loaded)(void *context, struct Apu *apu);
} SnesRecompApuProgram;

/* NULL restores interpretation. Call before the APU runs. */
void snesrecomp_apu_set_program(const SnesRecompApuProgram *program);

/* True when the overlay is compiled in. */
bool snesrecomp_apu_program_supported(void);

#ifdef __cplusplus
}
#endif

#endif
