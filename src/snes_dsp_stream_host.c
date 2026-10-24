/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "snesrecomp_platform/snes_dsp_stream_host.h"
#include "snesrecomp_platform/snes_dsp_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
typedef volatile long TraceAtomic;
static void TraceStore(TraceAtomic *p, uint32_t value) { _InterlockedExchange(p, (long)value); }
static uint32_t TraceLoad(const TraceAtomic *p) {
    return (uint32_t)_InterlockedCompareExchange((volatile long *)p, 0, 0);
}
#else
#include <stdatomic.h>
typedef _Atomic(uint32_t) TraceAtomic;
static void TraceStore(TraceAtomic *p, uint32_t value) {
    atomic_store_explicit(p, value, memory_order_release);
}
static uint32_t TraceLoad(const TraceAtomic *p) {
    return atomic_load_explicit(p, memory_order_acquire);
}
#endif

#define RING_MASK (SR_DSPS_RING_WORDS - 1u)
/* Boundaries per ADVANCE before the DSP may start on them. */
#define GRANULE 64u
/* Bound small ARAM publication batches without a syscall per byte pair. */
#define PUBLISH_WORDS 128u
/* Dead only after this long without ANY DSP progress; a DSP that waits
 * for playback still advances its read or synthesis counters. */
#define STALL_NS 250000000ull
#define POLL_NS 250000ull

struct SrSnesDspStream {
    SrSnesDspStreamTransport t;
    SrSnesDspBridgeBackend backend;
    SrSnesDspStreamStats stats;
    uint16_t write, sum, fence_id, gain, pending;
    uint16_t read, published; /* Conservative consumer position and last commit. */
    uint16_t saved_at;
    int attached, dead, saved_valid;
    TraceAtomic trace_phase, trace_attached, trace_write, trace_fence;
    uint8_t regs[128];
};

static uint16_t ctl(SrSnesDspStream *s, unsigned i) { return s->t.ctl[i]; }

static uint32_t ctl32(SrSnesDspStream *s, unsigned i) {
    uint16_t hi, lo;
    do { hi = s->t.ctl[i + 1u]; lo = s->t.ctl[i]; } while (hi != s->t.ctl[i + 1u]);
    return (uint32_t)lo | (uint32_t)hi << 16;
}

static void commit(SrSnesDspStream *s) {
    s->t.barrier(s->t.context);
    s->t.ctl[SR_DSPS_A_WRITE] = s->write;
    TraceStore(&s->trace_write, s->write);
    s->published = s->write;
    s->t.barrier(s->t.context);
}

static int failed(SrSnesDspStream *s) {
    uint16_t status = ctl(s, SR_DSPS_D_STATUS);
    if (status) { s->stats.last_status = status; s->dead = 1; }
    return s->dead;
}

typedef struct Watch { uint64_t since; uint16_t read, synth; } Watch;

static void watch_start(SrSnesDspStream *s, Watch *w) {
    w->since = s->t.monotonic_ns(s->t.context);
    w->read = ctl(s, SR_DSPS_D_READ);
    w->synth = ctl(s, SR_DSPS_D_SYNTH);
}

/* One bounded wait; 0 once the DSP failed or stopped making progress. */
static int watch_wait(SrSnesDspStream *s, Watch *w) {
    uint64_t now = s->t.monotonic_ns(s->t.context);
    uint16_t read = ctl(s, SR_DSPS_D_READ), synth = ctl(s, SR_DSPS_D_SYNTH);
    if (read != w->read || synth != w->synth) { w->since = now; w->read = read; w->synth = synth; }
    if (failed(s) || now - w->since > STALL_NS) { s->dead = 1; return 0; }
    s->t.wait(s->t.context, POLL_NS);
    return 1;
}

/* Publishes what is written so far, then waits for ring space. */
static int reserve(SrSnesDspStream *s, uint16_t words) {
    Watch w;
    uint64_t begin;
    if ((uint16_t)(s->write - s->read) <= SR_DSPS_RING_WORDS - words) return !s->dead;
    s->read = ctl(s, SR_DSPS_D_READ);
    if ((uint16_t)(s->write - s->read) <= SR_DSPS_RING_WORDS - words) return !s->dead;
    commit(s);
    TraceStore(&s->trace_phase, SR_DSPS_PROGRESS_RING_WAIT);
    ++s->stats.ring_waits;
    watch_start(s, &w);
    begin = w.since;
    while ((uint16_t)(s->write - s->read) > SR_DSPS_RING_WORDS - words) {
        s->read = ctl(s, SR_DSPS_D_READ);
        if ((uint16_t)(s->write - s->read) <= SR_DSPS_RING_WORDS - words) break;
        if (!watch_wait(s, &w)) return 0;
    }
    TraceStore(&s->trace_phase, SR_DSPS_PROGRESS_RUNNING);
    s->stats.ring_wait_ns += s->t.monotonic_ns(s->t.context) - begin;
    return !s->dead;
}

static void emit(SrSnesDspStream *s, uint16_t word) {
    s->t.ring[s->write & RING_MASK] = word;
    ++s->write;
    s->sum = (uint16_t)(s->sum + word);
    ++s->stats.words;
}

static int flush_advance(SrSnesDspStream *s) {
    while (s->pending) {
        uint16_t n = s->pending > SR_DSPS_ARG_MASK ? SR_DSPS_ARG_MASK : s->pending;
        if (!reserve(s, 1)) return 0;
        emit(s, (uint16_t)(SR_DSPS_ADVANCE | n));
        s->pending = (uint16_t)(s->pending - n);
        ++s->stats.commands;
    }
    return 1;
}

static int fence(SrSnesDspStream *s, uint16_t kind, uint16_t a0, uint16_t a1) {
    uint16_t id, sum;
    uint64_t begin, now;
    Watch w;
    if (s->dead || !s->attached || !flush_advance(s) || !reserve(s, SR_DSPS_FENCE_WORDS)) return 0;
    sum = s->sum;
    id = (uint16_t)(++s->fence_id & SR_DSPS_ARG_MASK);
    TraceStore(&s->trace_fence, id);
    TraceStore(&s->trace_phase, SR_DSPS_PROGRESS_FENCE_WAIT);
    emit(s, (uint16_t)(SR_DSPS_FENCE | id));
    emit(s, kind); emit(s, a0); emit(s, a1); emit(s, sum);
    s->sum = 0;
    commit(s);
    ++s->stats.fences;
    watch_start(s, &w);
    begin = w.since;
    while (ctl(s, SR_DSPS_D_FENCE) != id)
        if (!watch_wait(s, &w)) return 0;
    TraceStore(&s->trace_phase, SR_DSPS_PROGRESS_RUNNING);
    now = s->t.monotonic_ns(s->t.context) - begin;
    s->stats.fence_wait_ns += now;
    if (now > s->stats.max_fence_wait_ns) s->stats.max_fence_wait_ns = now;
    return 1;
}

/* One SAVE serves a flush and the checkpoint that follows it. */
static int save(SrSnesDspStream *s) {
    if (s->saved_valid && s->saved_at == s->write && !s->pending) return 1;
    if (!fence(s, SR_DSPS_FENCE_SAVE, 0, 0)) return 0;
    s->t.invalidate(s->t.context, 0, SR_DSPS_ARENA_LIVE);
    s->saved_valid = 1;
    s->saved_at = s->write;
    return 1;
}

static void prepare_arena(SrSnesDspStream *s, const uint16_t *state, const uint8_t *aram) {
    for (unsigned i = 0; i < SR_SNES_DSP_CORE_STATE_WORDS; ++i) {
        s->t.arena[2u * i] = (uint8_t)state[i];
        s->t.arena[2u * i + 1u] = (uint8_t)(state[i] >> 8);
    }
    memcpy(s->t.arena + SR_DSPS_ARENA_ARAM, aram, 65536);
    /* No dirty ARM line may later land on live ARAM. */
    s->t.clean(s->t.context, 0, SR_DSPS_ARENA_BYTES);
    s->t.invalidate(s->t.context, SR_DSPS_ARENA_LIVE, SR_DSPS_ARENA_BYTES - SR_DSPS_ARENA_LIVE);
}

static int backend_restore(void *context, const uint16_t *state, const uint8_t *aram,
                           uint32_t now, uint32_t next_sample) {
    SrSnesDspStream *s = context;
    (void)now; (void)next_sample;
    if (s->dead) return 0;
    for (unsigned i = 0; i < 128u; ++i) s->regs[i] = (uint8_t)state[6u + i];
    s->pending = 0;
    s->saved_valid = 0;
    prepare_arena(s, state, aram);
    if (s->attached) return fence(s, SR_DSPS_FENCE_LOAD, 0, 0);
    s->write = s->read = s->published = s->sum = 0;
    s->t.ctl[SR_DSPS_A_WRITE] = 0;
    s->t.ctl[SR_DSPS_A_GAIN] = s->gain;
    s->t.ctl[SR_DSPS_A_TARGET] = SR_DSPS_DEFAULT_TARGET;
    s->t.ctl[SR_DSPS_A_FLAGS] = 0;
    s->t.ctl[SR_DSPS_A_MAGIC] = SR_DSPS_MAGIC_ARM;
    s->t.barrier(s->t.context);
    TraceStore(&s->trace_phase, SR_DSPS_PROGRESS_ATTACH);
    if (s->t.attach(s->t.context)) return 0;
    if (ctl(s, SR_DSPS_D_MAGIC) != SR_DSPS_MAGIC_DSP) {
        s->t.detach(s->t.context);
        return 0;
    }
    s->attached = 1;
    s->stats.attached = 1;
    TraceStore(&s->trace_attached, 1);
    TraceStore(&s->trace_phase, SR_DSPS_PROGRESS_RUNNING);
    return 1;
}

static int backend_write_reg(void *context, uint32_t now, uint8_t reg, uint8_t value) {
    SrSnesDspStream *s = context;
    (void)now;
    if (s->dead || !flush_advance(s) || !reserve(s, 1)) return 0;
    emit(s, (uint16_t)SR_DSPS_REG(reg, value));
    s->regs[reg & 127u] = value;
    ++s->stats.commands;
    return 1;
}

static int backend_write_aram(void *context, uint32_t now, uint32_t address,
                              const uint8_t *bytes, uint32_t count) {
    SrSnesDspStream *s = context;
    (void)now;
    if (s->dead || address > 65535u || !flush_advance(s)) return 0;
    while (count) {
        uint16_t n = count > SR_DSPS_MAX_ARAM ? SR_DSPS_MAX_ARAM : (uint16_t)count;
        if (!reserve(s, (uint16_t)(2u + ((n + 1u) >> 1)))) return 0;
        emit(s, (uint16_t)(SR_DSPS_ARAM | (n & SR_DSPS_ARG_MASK)));
        emit(s, (uint16_t)address);
        for (uint16_t i = 0; i < n; i += 2)
            emit(s, (uint16_t)(bytes[i] | (i + 1u < n ? bytes[i + 1u] << 8 : 0)));
        /* Publish a bounded ordered batch, or earlier for a fence/full ring.
         * A later ADVANCE can never overtake these ARAM bytes. */
        if ((uint16_t)(s->write - s->published) >= PUBLISH_WORDS) commit(s);
        s->stats.aram_bytes += n;
        ++s->stats.commands;
        bytes += n; count -= n; address = (address + n) & 0xffffu;
    }
    return 1;
}

static int backend_advance(void *context, uint32_t sample_timestamp) {
    SrSnesDspStream *s = context;
    (void)sample_timestamp;
    if (s->dead) return 0;
    ++s->pending;
    ++s->stats.advances;
    if (s->pending >= GRANULE) {
        if (!flush_advance(s)) return 0;
        commit(s);
    }
    return 1;
}

static int backend_read_reg(void *context, uint8_t reg, uint8_t *value) {
    SrSnesDspStream *s = context;
    if (!fence(s, SR_DSPS_FENCE_REGS, 0, 0)) return 0;
    *value = (uint8_t)s->t.reply[reg & 127u];
    return 1;
}

static int backend_flush(void *context, const SrSnesDspBridgeSink *sink) {
    SrSnesDspStream *s = context;
    if (!save(s)) return 0;
    return sink->aram(sink->context, 0, s->t.arena + SR_DSPS_ARENA_ARAM, 65536);
}

static int backend_checkpoint(void *context, uint16_t *state) {
    SrSnesDspStream *s = context;
    if (!save(s)) return 0;
    for (unsigned i = 0; i < SR_SNES_DSP_CORE_STATE_WORDS; ++i)
        state[i] = (uint16_t)(s->t.arena[2u * i] | s->t.arena[2u * i + 1u] << 8);
    return 1;
}

/* A silent DSP must not take the game down: rebuild from register shadows. */
static int backend_recover(void *context, const SrSnesDspBridgeSink *sink, uint16_t *state) {
    SrSnesDspStream *s = context;
    SrSnesDspCore core;
    SrSnesDspMemory memory = {0};
    if (!s->dead && backend_flush(s, sink) && backend_checkpoint(s, state)) return 1;
    s->dead = 1;
    ++s->stats.fallbacks;
    fprintf(stderr, "[snes-dsp] stream unresponsive (status %u); voices restart\n",
            (unsigned)s->stats.last_status);
    sr_snes_dsp_core_init(&core, &memory);
    for (unsigned i = 0; i < 128u; ++i)
        if (i != 0x4cu && i != 0x7cu) sr_snes_dsp_core_write(&core, (uint16_t)i, s->regs[i]);
    return sr_snes_dsp_core_save(&core, state, SR_SNES_DSP_CORE_STATE_WORDS);
}

static void backend_close(void *context) {
    SrSnesDspStream *s = context;
    if (s->attached) {
        if (!s->dead) { flush_advance(s); commit(s); }
        s->t.detach(s->t.context);
    }
    s->attached = 0;
    s->stats.attached = 0;
    TraceStore(&s->trace_attached, 0);
}

SrSnesDspStream *sr_snes_dsp_stream_create(const SrSnesDspStreamTransport *t) {
    SrSnesDspStream *s;
    if (!t || !t->ctl || !t->ring || !t->reply || !t->arena || !t->attach || !t->detach ||
        !t->clean || !t->invalidate || !t->barrier || !t->wait || !t->monotonic_ns)
        return NULL;
    s = calloc(1, sizeof(*s));
    if (!s) return NULL;
#if !defined(_MSC_VER) || defined(__clang__)
    atomic_init(&s->trace_phase, 0);
    atomic_init(&s->trace_attached, 0);
    atomic_init(&s->trace_write, 0);
    atomic_init(&s->trace_fence, 0);
#endif
    s->t = *t;
    s->gain = 0x4000u;
    s->backend = (SrSnesDspBridgeBackend){SR_SNES_DSP_BRIDGE_ABI, s,
        backend_restore, backend_write_reg, backend_write_aram, backend_advance,
        backend_read_reg, backend_flush, backend_checkpoint, backend_recover, backend_close,
        SR_SNES_DSP_BRIDGE_REMOTE_OUTPUT};
    return s;
}

const SrSnesDspBridgeBackend *sr_snes_dsp_stream_backend(SrSnesDspStream *s) {
    return s ? &s->backend : NULL;
}

void sr_snes_dsp_stream_publish(SrSnesDspStream *s) {
    if (!s || !s->attached || s->dead) return;
    if (flush_advance(s)) commit(s);
}

void sr_snes_dsp_stream_gain(SrSnesDspStream *s, uint16_t q14) {
    if (!s) return;
    s->gain = q14;
    if (s->attached) s->t.ctl[SR_DSPS_A_GAIN] = q14;
}

void sr_snes_dsp_stream_stats(SrSnesDspStream *s, SrSnesDspStreamStats *out) {
    if (!s || !out) return;
    s->stats.dead = (uint32_t)s->dead;
    if (s->attached) {
        s->stats.dsp_synth = ctl(s, SR_DSPS_D_SYNTH);
        s->stats.dsp_played = ctl32(s, SR_DSPS_D_PLAYED);
        s->stats.dsp_fill = ctl(s, SR_DSPS_D_FILL);
        s->stats.dsp_underruns = ctl(s, SR_DSPS_D_UNDERRUNS);
        s->stats.dsp_stalls = ctl(s, SR_DSPS_D_STALLS);
        s->stats.dsp_busy = ctl32(s, SR_DSPS_D_BUSY);
        s->stats.dsp_max_chunk = ctl(s, SR_DSPS_D_MAX_CHUNK);
        s->stats.dsp_frames = ctl(s, SR_DSPS_D_FRAMES);
        s->stats.dsp_pending = ctl(s, SR_DSPS_D_PENDING);
        if (ctl(s, SR_DSPS_D_STATUS)) s->stats.last_status = ctl(s, SR_DSPS_D_STATUS);
    }
    *out = s->stats;
}

void sr_snes_dsp_stream_progress(const SrSnesDspStream *s,
    SrSnesDspStreamProgress *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!s) return;
    out->phase = TraceLoad(&s->trace_phase);
    out->attached = TraceLoad(&s->trace_attached);
    out->published_write = TraceLoad(&s->trace_write);
    out->fence_sent = TraceLoad(&s->trace_fence);
    if (!out->attached) return;
    out->dsp_read = s->t.ctl[SR_DSPS_D_READ];
    out->fence_done = s->t.ctl[SR_DSPS_D_FENCE];
    out->status = s->t.ctl[SR_DSPS_D_STATUS];
    out->pcm_fill = s->t.ctl[SR_DSPS_D_FILL];
    out->synthesized = s->t.ctl[SR_DSPS_D_SYNTH];
    out->pending = s->t.ctl[SR_DSPS_D_PENDING];
}

int sr_snes_dsp_stream_active(const SrSnesDspStream *s) {
    return s && s->attached && !s->dead;
}

int sr_snes_dsp_stream_destroy(SrSnesDspStream *s) {
    if (!s) return 1;
    backend_close(s);
    free(s);
    return 1;
}
