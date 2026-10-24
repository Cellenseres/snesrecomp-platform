/* SDK-free adapter for the reviewed snesrecomp Dsp/ Apu integration.
 * This is a LOCAL host interface, not the ARM/Teak wire ABI. */
#ifndef SNESRECOMP_PLATFORM_SNES_DSP_BRIDGE_H
#define SNESRECOMP_PLATFORM_SNES_DSP_BRIDGE_H

#include <stdint.h>
#include "snesrecomp_platform/snes_dsp_core.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Dsp;
struct Apu;

#define SR_SNES_DSP_BRIDGE_ABI 2u
/* The backend plays PCM itself; flush delivers ARAM only, never PCM. */
#define SR_SNES_DSP_BRIDGE_REMOTE_OUTPUT 1u

typedef struct SrSnesDspBridgeSink {
    void *context;
    /* Return 1 only after retaining all frames. No silent partial consume. */
    int (*pcm)(void *context, const int16_t *stereo, uint32_t frames);
    /* Final coherent bytes after ALL accepted events preceding this flush.
     * Returning old echo bytes over newer host writes is forbidden. */
    int (*aram)(void *context, uint32_t address, const uint8_t *bytes,
                uint32_t count);
} SrSnesDspBridgeSink;

typedef struct SrSnesDspBridgeBackend {
    uint32_t abi;
    void *context;
    /* All state arrays contain SR_SNES_DSP_CORE_STATE_WORDS logical words.
     * restore is called at an ownership boundary. It must invalidate old
     * epochs before using this state. ARAM is copied/retained before return.
     * next_sample is the first boundary NOT represented by state. Guest
     * clocks are uint32 APU cycles; comparisons require a <2^31 horizon. */
    int (*restore)(void *context, const uint16_t *state, const uint8_t *aram,
                   uint32_t now, uint32_t next_sample);
    /* These three submission callbacks return 1 for complete acceptance,
     * 0 for NO acceptance. They own bounded backpressure internally; no
     * command may be partially accepted or silently lost. Failure after
     * ambiguous execution must be resolved by the backend's recovery
     * journal before returning. Host pointers are consumed before return. */
    int (*write_reg)(void *context, uint32_t now, uint8_t reg, uint8_t value);
    int (*write_aram)(void *context, uint32_t now, uint32_t address,
                      const uint8_t *bytes, uint32_t count);
    int (*advance)(void *context, uint32_t sample_timestamp);
    /* advance explicitly includes that sample boundary. A write/read at
     * the same timestamp AFTER advance remains after it in FIFO order.
     * Reads/flush/checkpoint must never advance guest time themselves. */
    int (*read_reg)(void *context, uint8_t reg, uint8_t *value);
    int (*flush)(void *context, const SrSnesDspBridgeSink *sink);
    int (*checkpoint)(void *context, uint16_t *state);
    /* Resolve all ACCEPTED work, deliver each outstanding PCM frame once,
     * return coherent ARAM deltas and state, and fence old DMA/epochs.
     * Then the bridge resumes the pinned CPU implementation. If recovery
     * cannot prove the state, return 0; the bridge fails closed. */
    int (*recover)(void *context, const SrSnesDspBridgeSink *sink,
                   uint16_t *state);
    void (*close)(void *context);
    uint32_t flags;
} SrSnesDspBridgeBackend;

typedef struct SrSnesDspBridgeStats {
    uint64_t submitted_samples;
    uint64_t completed_samples;
    uint64_t aram_scan_bytes;
    uint64_t aram_upload_bytes;
    uint64_t aram_download_bytes;
    uint64_t synchronizations;
    uint64_t recoveries;
    uint32_t pending_samples;
    uint32_t max_pending_samples;
    uint32_t active;
    uint64_t tracked_transactions, range_fences, shadow_reads;
} SrSnesDspBridgeStats;

/* Register before running the guest, with the existing RtlApuLock held.
 * NULL leaves the pinned reference path. Registration itself does not
 * activate a backend; attach only after native SPC takeover. Only one APU
 * may own the registered backend at once. The descriptor is copied. */
int snesrecomp_dsp_bridge_register(const SrSnesDspBridgeBackend *backend);
int snesrecomp_dsp_bridge_attach(struct Apu *apu);
void snesrecomp_dsp_bridge_detach(struct Dsp *dsp);
int snesrecomp_dsp_bridge_active(const struct Dsp *dsp);
void snesrecomp_dsp_bridge_clock(struct Dsp *dsp, uint32_t apu_cycle);

/* The native driver writes ARAM through C lvalues. Fence on entry, collect
 * changed runs on exit. Exactly one 64-KiB scan per outer transaction; no
 * 64-KiB scan/copy in dsp_cycle. All driver execution is at one guest time.
 * Nesting is supported for host-side helpers, with one outer collection. */
void snesrecomp_dsp_bridge_native_begin(struct Apu *apu);
void snesrecomp_dsp_bridge_native_end(struct Apu *apu);
/* Opt-in contract for instrumented native programs: every borrowed lvalue
 * and dynamic access must be declared BEFORE use. No guest time may elapse
 * until native_end. Writable borrows remain tracked until that end. The
 * bridge fences only outstanding echo pages overlapping an access. */
void snesrecomp_dsp_bridge_native_begin_tracked(struct Apu *apu);
void snesrecomp_dsp_bridge_native_access(struct Apu *apu, uint32_t address,
                                         uint32_t count, int writable);

/* Explicit narrow hooks for raw ARAM access outside a driver transaction.
 * written observes an already completed host write, before any next DSP
 * sample. before_read fences prior synthesis/echo. Ranges never wrap;
 * callers split a wrapping operation at 65536. */
void snesrecomp_dsp_bridge_aram_written(struct Dsp *dsp, uint32_t address,
                                       uint32_t count);
void snesrecomp_dsp_bridge_before_read(struct Dsp *dsp);
void snesrecomp_dsp_bridge_before_saveload(struct Dsp *dsp);
void snesrecomp_dsp_bridge_stats(SrSnesDspBridgeStats *out);

#ifdef __cplusplus
}
#endif
#endif
