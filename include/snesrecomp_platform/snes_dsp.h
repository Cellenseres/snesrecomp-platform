/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef SNESRECOMP_PLATFORM_SNES_DSP_H
#define SNESRECOMP_PLATFORM_SNES_DSP_H

#include <stddef.h>
#include <stdint.h>
#include "snesrecomp_platform/snes_dsp_bridge.h"
#include "snesrecomp_platform/snes_dsp_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SrSnesDspHost SrSnesDspHost;

/* Local configuration only: no pointers or compiler layout cross the wire. */
typedef struct SrSnesDspBoot {
    const uint16_t *state; /* 544 words */
    const uint8_t *aram;  /* 65536 octets */
    uint32_t epoch, now, next_sample, first_sequence;
} SrSnesDspBoot;

typedef struct SrSnesDspTransport {
    void *context;
    /* Return SR_DSP_OK only after setup complete. Unsupported/resources may
     * refuse setup; host then selects the identical portable CPU core. */
    uint16_t (*open)(void *context, const SrSnesDspBoot *boot);
    /* submit copies all active request data before return. BUSY means no acceptance.
     * poll consumes the requested reply only when returning OK. Request and
     * reply storage are caller-owned logical word arrays (2080/4096 words).
     * poll's OK means bytes available; reply status still needs validation. */
    uint16_t (*submit)(void *context, const uint16_t *request);
    uint16_t (*poll)(void *context, uint32_t sequence, uint16_t *reply);
    /* Event-driven bounded wait; notification may be spurious/coalesced. */
    void (*wait)(void *context, uint64_t maximum_ns);
    uint64_t (*monotonic_ns)(void *context);
    /* OK proves no old DMA can ever touch these buffers again. A timeout or
     * sleep transition is NOT that proof. Failed stop must retain memory. */
    uint16_t (*stop)(void *context);
} SrSnesDspTransport;

typedef struct SrSnesDspHostStats {
    uint64_t submitted_jobs, completed_jobs, produced_samples;
    /* Rounded active request/reply spans. Setup, ownership cache lines and
     * firmware ARAM-cache DMA are separate costs, not included here. */
    uint64_t request_bytes, reply_bytes, aram_write_bytes, echo_bytes;
    uint64_t waits, wait_ns, timeouts, recoveries, checkpoints;
    uint64_t checkpoint_copy_bytes;
    uint32_t backlog_peak, journal_peak, pcm_peak;
    uint32_t epoch, active_remote, last_error;
    size_t persistent_host_bytes;
    uint64_t remote_busy_ticks, remote_dma_bytes;
    uint32_t remote_max_busy_ticks, remote_max_dma_bytes;
} SrSnesDspHostStats;

/* One serialized host owner (normally the game's existing APU lock).
 * All memory is allocated up front. NULL transport selects the CPU adapter.
 * Default stalled-progress deadline is 20ms, configurable before activation;
 * it is not a normal per-sample sleep or OpenDSP's 200ms control mailbox timeout.
 */
SrSnesDspHost *sr_snes_dsp_host_create(const SrSnesDspTransport *transport);
const SrSnesDspBridgeBackend *sr_snes_dsp_host_backend(SrSnesDspHost *host);
void sr_snes_dsp_host_deadline(SrSnesDspHost *host, uint64_t nanoseconds);
void sr_snes_dsp_host_stats(const SrSnesDspHost *host, SrSnesDspHostStats *out);
/* Returns 0 if transport cannot prove quiescence; object remains alive. */
int sr_snes_dsp_host_destroy(SrSnesDspHost *host);

#ifdef __cplusplus
}
#endif
#endif
