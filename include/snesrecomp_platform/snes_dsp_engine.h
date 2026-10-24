/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef SNESRECOMP_PLATFORM_SNES_DSP_ENGINE_H
#define SNESRECOMP_PLATFORM_SNES_DSP_ENGINE_H
#include "snesrecomp_platform/snes_dsp_wire.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Single owner, allocation-free, resumable execution engine. Request/reply
 * storage remains alive and exclusively owned until step() returns OK.
 * The memory backend must latch DMA errors separately; a failed DMA poisons
 * the entire uncommitted job. Never publish its PCM as a successful reply.
 */
typedef struct SrSnesDspEngine {
    SrSnesDspCore core;
    SrSnesDspMemory memory;
    const SrSnesDspU16 *request;
    SrSnesDspU16 *reply;
    SrSnesDspU32 epoch, sequence, now, next_sample;
    SrSnesDspU16 event_index, event_count, samples, echo_count;
    SrSnesDspU16 active, record_echo, poisoned;
} SrSnesDspEngine;
void sr_dsp_engine_init(SrSnesDspEngine *e, const SrSnesDspMemory *memory);
/* Reset/init does not change ARAM. Setup supplies it separately while stopped.
 * now and next_sample describe the first not-yet-synthesized boundary;
 * distance must be0..31. A null state resets the compact core.
 */
SrSnesDspU16 sr_dsp_engine_restore(SrSnesDspEngine *e,
                                  const SrSnesDspU16 *state,
                                  SrSnesDspU32 now, SrSnesDspU32 next_sample);
void sr_dsp_engine_epoch(SrSnesDspEngine *e, SrSnesDspU32 epoch,
                         SrSnesDspU32 first_sequence);
/* Validates the ENTIRE job before any core/ARAM mutation. On rejection writes
 * an error reply, keeps state/time/sequence unchanged, and is immediately idle.
 */
SrSnesDspU16 sr_dsp_engine_begin(SrSnesDspEngine *e,
                                const SrSnesDspU16 *request,
                                SrSnesDspU16 *reply);
/* Budget1..MAX_SAMPLES. Non-sample work bounded by MAX_EVENTS/MAX_PAYLOAD.
 * BUSY means call again. No dynamic allocation, wall-clock advance or dropping.
 */
SrSnesDspU16 sr_dsp_engine_step(SrSnesDspEngine *e, SrSnesDspU16 budget);
/* Cancelling an in-progress job invalidates its partially changed state.
 * Caller must restore a committed checkpoint before reuse; cancel alone does
 * not magically rewind echo/voices. epoch invalidates late completion writes.
 */
void sr_dsp_engine_cancel(SrSnesDspEngine *e);
#ifdef __cplusplus
}
#endif
#endif
