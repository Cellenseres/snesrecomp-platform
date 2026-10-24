/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef SNESRECOMP_PLATFORM_SNES_DSP_STREAM_HOST_H
#define SNESRECOMP_PLATFORM_SNES_DSP_STREAM_HOST_H

#include <stdint.h>
#include "snesrecomp_platform/snes_dsp_bridge.h"
#include "snesrecomp_platform/snes_dsp_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SDK-free producer for stream ABI 3. The transport owns the uncached DSP
 * RAM mapping, the FCRAM arena and the mailbox; this object owns ordering,
 * flow control and fences. One thread, under the game's APU lock. */
typedef struct SrSnesDspStreamTransport {
    void *context;
    volatile uint16_t *ctl, *ring, *reply;
    uint8_t *arena;
    /* 0 on success. attach hands the prepared arena to the DSP. */
    int (*attach)(void *context);
    int (*detach)(void *context);
    void (*clean)(void *context, uint32_t offset, uint32_t bytes);
    void (*invalidate)(void *context, uint32_t offset, uint32_t bytes);
    void (*barrier)(void *context);
    /* Bounded sleep or event wait; spurious wakeups are fine. */
    void (*wait)(void *context, uint64_t nanoseconds);
    uint64_t (*monotonic_ns)(void *context);
} SrSnesDspStreamTransport;

typedef struct SrSnesDspStreamStats {
    uint64_t commands, words, advances, aram_bytes, fences;
    uint64_t ring_waits, ring_wait_ns, fence_wait_ns, max_fence_wait_ns;
    uint32_t attached, dead, last_status, fallbacks;
    /* Mirrors of DSP-owned counters, refreshed by stats(). */
    uint32_t dsp_played, dsp_busy;
    uint16_t dsp_synth, dsp_fill, dsp_underruns, dsp_stalls, dsp_max_chunk,
             dsp_frames, dsp_pending;
} SrSnesDspStreamStats;

enum {
    SR_DSPS_PROGRESS_IDLE, SR_DSPS_PROGRESS_ATTACH, SR_DSPS_PROGRESS_RUNNING,
    SR_DSPS_PROGRESS_RING_WAIT, SR_DSPS_PROGRESS_FENCE_WAIT, SR_DSPS_PROGRESS_DETACH
};
typedef struct SrSnesDspStreamProgress {
    uint32_t phase, attached, published_write, dsp_read, fence_sent, fence_done;
    uint32_t status, pcm_fill, synthesized, pending;
} SrSnesDspStreamProgress;

typedef struct SrSnesDspStream SrSnesDspStream;

SrSnesDspStream *sr_snes_dsp_stream_create(const SrSnesDspStreamTransport *transport);
const SrSnesDspBridgeBackend *sr_snes_dsp_stream_backend(SrSnesDspStream *stream);
/* End of a guest frame: publishes all buffered ARAM/register commands and
 * sample boundaries. Fences and bounded ring batches publish earlier. */
void sr_snes_dsp_stream_publish(SrSnesDspStream *stream);
/* Q14 output gain; 0x4000 is unity. */
void sr_snes_dsp_stream_gain(SrSnesDspStream *stream, uint16_t q14);
void sr_snes_dsp_stream_stats(SrSnesDspStream *stream, SrSnesDspStreamStats *out);
/* Bounded observations; never waits or publishes commands. */
void sr_snes_dsp_stream_progress(const SrSnesDspStream *stream,
    SrSnesDspStreamProgress *out);
/* Attached and healthy: the DSP, not the host ring, is playing S-DSP audio. */
int sr_snes_dsp_stream_active(const SrSnesDspStream *stream);
/* Returns 0 if the DSP could not be detached; memory stays owned then. */
int sr_snes_dsp_stream_destroy(SrSnesDspStream *stream);

#ifdef __cplusplus
}
#endif
#endif
