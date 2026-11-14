#ifndef SNESRECOMP_PLATFORM_APU_PROGRAM_H
#define SNESRECOMP_PLATFORM_APU_PROGRAM_H

/* Native replacement for the game's SPC-700 program.
 * Needs cmake/SnesRecompPlatformApuProgram.cmake; clock, ports, timers and
 * S-DSP stay with the core. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct Apu;

enum {
    /* Opt-in: active stays true and cycle only records the clock throughout
     * idle(), including executed DSP samples. No DSP-feedback-dependent owner
     * change or program access is allowed. Default zero retains sample cuts. */
    SNESRECOMP_APU_IDLE_STABLE_DSP = 1u << 0
};

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
    /* Cycles from now in which cycle only records the clock.
     * NULL steps every cycle. */
    uint32_t (*idle)(void *context, const struct Apu *apu);
} SnesRecompApuProgram;

/* NULL restores interpretation. Call before the APU runs. */
void snesrecomp_apu_set_program(const SnesRecompApuProgram *program);
/* Additive opt-in, preserving the original program structure and default
 * sample boundaries. Unknown capabilities or NULL with nonzero capabilities
 * fail before changing the program. Same ownership/lifecycle as set_program. */
bool snesrecomp_apu_set_program_with_capabilities(
    const SnesRecompApuProgram *program, uint32_t capabilities);

/* True when the overlay is compiled in. */
bool snesrecomp_apu_program_supported(void);

/* Steps up to cycles idle cycles at once; 0 means step one. */
uint32_t snesrecomp_apu_program_skip(struct Apu *apu, uint32_t cycles);

#ifdef __cplusplus
}
#endif

#endif
