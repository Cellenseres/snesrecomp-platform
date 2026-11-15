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
    SNESRECOMP_APU_IDLE_STABLE_DSP = 1u << 0,
    /* Explicit CPU-only-span contract: every program callback observes only
     * APU/ARAM and owned program state, never guest CPU/WRAM, host time or
     * external producers. Callbacks cannot enqueue CPU input-port writes,
     * re-enter APU execution or change its CPU scheduling anchors.
     * Requires IDLE_STABLE_DSP and the separately enabled span overlay. */
    SNESRECOMP_APU_CPU_INDEPENDENT = 1u << 1
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

/* Optional, synchronous batching of actual APU execution inside a certified
 * CPU-only interval. Relative credit updates and absolute sync requests stay
 * in their original callers. Only portClock is virtual until end; no other
 * APU state may be read while active. End executes EVERY owed cycle/sample.
 * The caller provides exclusive ownership, unchanged CPU->APU inputs and ends
 * before any boundary/access that observes or changes APU state or its inputs.
 * Unrelated clock edges are allowed only when the caller proves that read set.
 * Public APU access,
 * reset/load and program replacement finish pending work before proceeding.
 * An empty CPU-port queue and an already active certified native program are
 * required. max_cycles is1..1024; exhaustion finishes work and resumes normal
 * execution. Nested begin fails transactionally. Default overlay is disabled. */
bool snesrecomp_apu_cpu_span_begin(struct Apu *apu,uint32_t max_cycles);
void snesrecomp_apu_cpu_span_end(struct Apu *apu);
/* True while a span begun for this APU is still open. Reads only the overlay's
 * owner record: no APU field, no completion, no side effect. Always false when
 * the overlay is disabled. */
bool snesrecomp_apu_cpu_span_open(const struct Apu *apu);
typedef struct SnesRecompApuCpuSpanStats {
    uint64_t started, finished, relative_calls, absolute_calls;
    uint64_t cycles_deferred, forced_finishes;
} SnesRecompApuCpuSpanStats;
void snesrecomp_apu_cpu_span_stats(SnesRecompApuCpuSpanStats *out);

#ifdef __cplusplus
}
#endif

#endif
