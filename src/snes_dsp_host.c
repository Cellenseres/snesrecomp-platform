/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "snesrecomp_platform/snes_dsp.h"
#include "snesrecomp_platform/snes_dsp_engine.h"
#include <stdlib.h>
#include <string.h>

#define JOURNAL 64u
#define PCM_FRAMES 8192u
#define REQUEST_WORDS (SR_DSP_REQUEST_CHECK_BYTES / 2u)

typedef struct HostJob {
    uint16_t words[REQUEST_WORDS];
    uint32_t start_next, end_time, end_next;
    uint16_t events, payload, samples, sealed, sent, complete;
} HostJob;

struct SrSnesDspHost {
    SrSnesDspTransport transport;
    SrSnesDspBridgeBackend backend;
    SrSnesDspHostStats stats;
    SrSnesDspEngine cpu;
    HostJob jobs[JOURNAL];
    uint16_t reply[SR_DSP_SLOT_WORDS];
    uint16_t saved_state[SR_SNES_DSP_CORE_STATE_WORDS];
    uint16_t latest_state[SR_SNES_DSP_CORE_STATE_WORDS];
    uint8_t checkpoint_aram[65536], aram[65536], work_aram[65536];
    uint8_t dirty[65536 / 8], regs[128];
    uint32_t dirty_begin, dirty_end;
    int16_t pcm[PCM_FRAMES * 2];
    uint32_t pcm_read, pcm_write, pending_samples;
    uint32_t epoch, next_sequence, now, next_sample;
    uint32_t saved_now, saved_next, saved_sequence;
    unsigned count, completed, submitted;
    uint64_t deadline_ns;
    int active, remote, unsafe, latest_valid;
};

static uint16_t cpu_read(void *p, uint16_t a) {
    return ((SrSnesDspHost *)p)->work_aram[a];
}
static void cpu_write(void *p, uint16_t a, uint16_t v) {
    ((SrSnesDspHost *)p)->work_aram[a] = (uint8_t)v;
}
static void set_aram(SrSnesDspHost *h, uint16_t a, uint8_t v) {
    if (h->aram[a] != v) {
        h->aram[a] = v;
        h->dirty[a >> 3] |= (uint8_t)(1u << (a & 7u));
        if (a < h->dirty_begin) h->dirty_begin = a;
        if ((uint32_t)a + 1u > h->dirty_end) h->dirty_end = (uint32_t)a + 1u;
    }
}
static void header(SrSnesDspHost *h, HostJob *j) {
    memset(j, 0, sizeof(*j));
    sr_dsp_wire_put32(j->words, 0, SR_DSP_MAGIC);
    sr_dsp_wire_put16(j->words, 4, SR_DSP_ABI);
    sr_dsp_wire_put16(j->words, 6, SR_DSP_HEADER_BYTES);
    sr_dsp_wire_put32(j->words, 8, SR_DSP_SLOT_BYTES);
    sr_dsp_wire_put32(j->words, SR_DSP_REQ_EPOCH, h->epoch);
    sr_dsp_wire_put32(j->words, SR_DSP_REQ_SEQUENCE, h->next_sequence++);
    sr_dsp_wire_put32(j->words, SR_DSP_REQ_START_TIME, h->now);
    j->start_next = j->end_next = h->next_sample;
    j->end_time = h->now;
}
static void seal(HostJob *j) {
    if (j->sealed) return;
    sr_dsp_wire_put16(j->words, SR_DSP_REQ_EVENTS, j->events);
    sr_dsp_wire_put16(j->words, SR_DSP_REQ_PAYLOAD_BYTES, j->payload);
    sr_dsp_wire_put16(j->words, SR_DSP_REQ_SAMPLE_LIMIT, j->samples);
    sr_dsp_wire_put32(j->words, SR_DSP_REQ_CRC,
                     sr_dsp_wire_crc(j->words, SR_DSP_REQUEST_CHECK_BYTES));
    j->sealed = 1;
}
static void seal_all(SrSnesDspHost *h) {
    if (h->count) seal(&h->jobs[h->count - 1]);
}
static int valid_reply(SrSnesDspHost *h, const HostJob *j) {
    const uint16_t *r = h->reply;
    uint16_t n = sr_dsp_wire_u16(r, SR_DSP_RES_SAMPLES);
    uint16_t en = sr_dsp_wire_u16(r, SR_DSP_RES_ECHO_COUNT), i;
    uint32_t start = sr_dsp_wire_u32(j->words, SR_DSP_REQ_START_TIME);
    uint32_t previous = start;
    if (sr_dsp_wire_u32(r, 0) != SR_DSP_REPLY_MAGIC ||
        sr_dsp_wire_u16(r, 4) != SR_DSP_ABI ||
        sr_dsp_wire_u16(r, 6) != SR_DSP_HEADER_BYTES ||
        sr_dsp_wire_u32(r, SR_DSP_RES_EPOCH) != h->epoch ||
        sr_dsp_wire_u32(r, SR_DSP_RES_SEQUENCE) !=
            sr_dsp_wire_u32(j->words, SR_DSP_REQ_SEQUENCE) ||
        sr_dsp_wire_u16(r, SR_DSP_RES_STATUS) != SR_DSP_OK ||
        n != j->samples || en > (uint16_t)(n * 4u) || (en & 3u) ||
        sr_dsp_wire_u32(r, SR_DSP_RES_TIME) != j->end_time ||
        sr_dsp_wire_u32(r, SR_DSP_RES_NEXT_SAMPLE) != j->end_next ||
        sr_dsp_wire_u32(r, SR_DSP_RES_CRC) !=
            sr_dsp_wire_crc(r, SR_DSP_REPLY_CHECK_BYTES)) return 0;
    for (i = 0; i < en; ++i) {
        uint16_t o = (uint16_t)(SR_DSP_REPLY_ECHO + i * SR_DSP_ECHO_BYTES);
        uint32_t t = sr_dsp_wire_u32(r, (uint16_t)(o + 4));
        if (sr_dsp_wire_u16(r, (uint16_t)(o + 2)) > 255u ||
            t - start >= j->end_time - start ||
            t - previous >= 0x80000000u ||
            ((t - j->start_next) & 31u)) return 0;
        /* Each synthesized echo update is exactly four consecutive bytes,
         * in L-low/L-high/R-low/R-high order, including address wrap. A
         * sample cannot emit two echo groups. Reject incomplete/reordered
         * deltas even if their transport checksum was recomputed. */
        if ((i & 3u) != 0u) {
            uint16_t prior = (uint16_t)(o - SR_DSP_ECHO_BYTES);
            if (t != previous || sr_dsp_wire_u16(r, o) !=
                    (uint16_t)(sr_dsp_wire_u16(r, prior) + 1u)) return 0;
        } else if (i && t - previous < 32u) return 0;
        previous = t;
    }
    {
        uint16_t sw = sr_dsp_wire_u16(r, SR_DSP_RES_STATE_WORDS);
        int expected = !!(sr_dsp_wire_u16(j->words, SR_DSP_REQ_FLAGS) & SR_DSP_FLAG_SNAPSHOT);
        if (sw != (expected ? SR_SNES_DSP_CORE_STATE_WORDS : 0u)) return 0;
        if (sw) {
            /* Check snapshot without changing the live local fallback core. */
            SrSnesDspCore check = h->cpu.core;
            if (!sr_snes_dsp_core_load(&check, r + SR_DSP_REPLY_STATE / 2u, sw)) return 0;
        }
    }
    return 1;
}
static void apply_echo(SrSnesDspHost *h, unsigned index) {
    uint16_t o = (uint16_t)(SR_DSP_REPLY_ECHO + index * SR_DSP_ECHO_BYTES);
    set_aram(h, sr_dsp_wire_u16(h->reply, o),
             (uint8_t)sr_dsp_wire_u16(h->reply, (uint16_t)(o + 2)));
}
static int accept_reply(SrSnesDspHost *h, HostJob *j, int replay) {
    unsigned i, k, ei = 0;
    unsigned echoes = sr_dsp_wire_u16(h->reply, SR_DSP_RES_ECHO_COUNT);
    if (!valid_reply(h, j)) return 0;
    if (!replay && (j->samples > h->pending_samples ||
                   j->samples > PCM_FRAMES - (h->pcm_write - h->pcm_read))) {
        h->stats.last_error = SR_DSP_ERR_CAPACITY;
        return 0;
    }
    /* Merge echo writes BEFORE the event at each exclusive frontier. At the
     * same timestamp the request's writes precede that timestamp's sample. */
    for (i = 0; i < j->events; ++i) {
        uint16_t o = (uint16_t)(SR_DSP_REQUEST_EVENTS + i * SR_DSP_EVENT_BYTES);
        uint32_t t = sr_dsp_wire_u32(j->words, (uint16_t)(o + 4));
        uint32_t start = sr_dsp_wire_u32(j->words, SR_DSP_REQ_START_TIME);
        while (ei < echoes) {
            uint32_t et = sr_dsp_wire_u32(h->reply,
                (uint16_t)(SR_DSP_REPLY_ECHO + ei * SR_DSP_ECHO_BYTES + 4));
            if (et - start >= t - start) break;
            apply_echo(h, ei++);
        }
        if (sr_dsp_wire_u16(j->words, o) == SR_DSP_EVENT_WRITE_ARAM) {
            uint32_t a = sr_dsp_wire_u32(j->words, (uint16_t)(o + 8));
            uint32_t b = sr_dsp_wire_u32(j->words, (uint16_t)(o + 12));
            for (k = 0; k < (b >> 16); ++k)
                set_aram(h, (uint16_t)(a + k), sr_dsp_wire_u8(j->words,
                    (uint16_t)(SR_DSP_REQUEST_PAYLOAD + (b & 65535u) + k)));
        }
    }
    while (ei < echoes) apply_echo(h, ei++);
    for (i = 0; i < 128; ++i)
        h->regs[i] = (uint8_t)sr_dsp_wire_u8(h->reply, (uint16_t)(SR_DSP_REPLY_REGS + i));
    if (!replay) {
        for (i = 0; i < j->samples; ++i) {
            unsigned p = (h->pcm_write++ & (PCM_FRAMES - 1u)) * 2u;
            h->pcm[p] = (int16_t)sr_dsp_wire_u16(h->reply, (uint16_t)(SR_DSP_REPLY_PCM + i * 4u));
            h->pcm[p + 1] = (int16_t)sr_dsp_wire_u16(h->reply, (uint16_t)(SR_DSP_REPLY_PCM + i * 4u + 2u));
        }
        h->pending_samples -= j->samples;
        h->stats.produced_samples += j->samples;
        h->stats.echo_bytes += echoes;
        ++h->stats.completed_jobs;
        if (h->remote) {
            const uint32_t busy = sr_dsp_wire_u32(h->reply, SR_DSP_RES_BUSY_TICKS);
            const uint32_t dma = sr_dsp_wire_u32(h->reply, SR_DSP_RES_DMA_BYTES);
            h->stats.remote_busy_ticks += busy; h->stats.remote_dma_bytes += dma;
            if (busy > h->stats.remote_max_busy_ticks) h->stats.remote_max_busy_ticks = busy;
            if (dma > h->stats.remote_max_dma_bytes) h->stats.remote_max_dma_bytes = dma;
        }
    }
    if (sr_dsp_wire_u16(h->reply, SR_DSP_RES_STATE_WORDS)) {
        memcpy(h->latest_state, h->reply + SR_DSP_REPLY_STATE / 2u, sizeof(h->latest_state));
        h->latest_valid = 1;
    } else h->latest_valid = 0;
    j->complete = 1;
    if (h->pcm_write - h->pcm_read > h->stats.pcm_peak)
        h->stats.pcm_peak = h->pcm_write - h->pcm_read;
    return 1;
}
static int run_cpu(SrSnesDspHost *h, HostJob *j) {
    uint16_t status = sr_dsp_engine_begin(&h->cpu, j->words, h->reply);
    if (status != SR_DSP_OK) return 0;
    do { status = sr_dsp_engine_step(&h->cpu, SR_DSP_MAX_SAMPLES); }
    while (status == SR_DSP_BUSY);
    return status == SR_DSP_OK;
}
static int stop_remote(SrSnesDspHost *h) {
    if (!h->remote) return !h->unsafe;
    if (h->transport.stop(h->transport.context) != SR_DSP_OK) {
        h->unsafe = 1; h->stats.last_error = SR_DSP_ERR_TIMEOUT;
        return 0;
    }
    h->remote = 0; h->stats.active_remote = 0; h->unsafe = 0;
    return 1;
}
static int replay_cpu(SrSnesDspHost *h) {
    unsigned i;
    if (!stop_remote(h)) return 0;
    seal_all(h);
    ++h->epoch;
    if (!h->epoch) ++h->epoch;
    h->stats.epoch = h->epoch;
    memcpy(h->work_aram, h->checkpoint_aram, 65536);
    memcpy(h->aram, h->checkpoint_aram, 65536);
    sr_dsp_engine_cancel(&h->cpu);
    if (sr_dsp_engine_restore(&h->cpu, h->saved_state, h->saved_now, h->saved_next) != SR_DSP_OK)
        return 0;
    sr_dsp_engine_epoch(&h->cpu, h->epoch, h->saved_sequence);
    for (i = 0; i < h->count; ++i) {
        HostJob *j = &h->jobs[i];
        int already = j->complete;
        sr_dsp_wire_put32(j->words, SR_DSP_REQ_EPOCH, h->epoch);
        j->sealed = 0; seal(j);
        if (!run_cpu(h, j) || !accept_reply(h, j, already)) {
            h->unsafe = 1; h->stats.last_error = SR_DSP_ERR_STATE;
            return 0;
        }
        j->sent = 1;
    }
    h->completed = h->submitted = h->count;
    ++h->stats.recoveries;
    return 1;
}
static int pump_mode(SrSnesDspHost *h, int drain) {
    uint64_t deadline = 0, previous = 0;
    unsigned ready;
    if (drain) seal_all(h);
    ready = h->count;
    if (ready && !h->jobs[ready - 1].sealed) --ready;
    if (!h->active || h->unsafe) return 0;
    if (h->completed == ready || (!drain && h->submitted == ready)) return 1;
    if (drain && h->remote) {
        previous = h->transport.monotonic_ns(h->transport.context);
        deadline = previous + h->deadline_ns;
    }
    while (h->completed < ready) {
        if (!h->remote) {
            HostJob *j = &h->jobs[h->completed];
            if (!run_cpu(h, j) || !accept_reply(h, j, 0)) return 0;
            j->sent = 1; ++h->submitted; ++h->completed;
            ++h->stats.submitted_jobs;
            continue;
        }
        while (h->submitted < ready && h->submitted - h->completed < SR_DSP_SLOT_COUNT) {
            HostJob *j = &h->jobs[h->submitted];
            uint16_t s = h->transport.submit(h->transport.context, j->words);
            if (s == SR_DSP_BUSY) break;
            if (s != SR_DSP_OK) { h->stats.last_error = s; return replay_cpu(h); }
            j->sent = 1; ++h->submitted; ++h->stats.submitted_jobs;
            h->stats.request_bytes += sr_dsp_wire_request_head(j->words) +
                sr_dsp_wire_request_data(j->words);
            if (h->submitted - h->completed > h->stats.backlog_peak)
                h->stats.backlog_peak = h->submitted - h->completed;
        }
        if (h->completed < h->submitted) {
            HostJob *j = &h->jobs[h->completed];
            uint16_t s = h->transport.poll(h->transport.context,
                sr_dsp_wire_u32(j->words, SR_DSP_REQ_SEQUENCE), h->reply);
            if (s == SR_DSP_OK) {
                h->stats.reply_bytes += sr_dsp_wire_reply_head(h->reply) +
                    sr_dsp_wire_reply_echo(h->reply) + sr_dsp_wire_reply_state(h->reply);
                if (!accept_reply(h, j, 0)) {
                    h->stats.last_error = SR_DSP_ERR_TRANSPORT;
                    return replay_cpu(h);
                }
                ++h->completed;
                previous = h->transport.monotonic_ns(h->transport.context);
                deadline = previous + h->deadline_ns;
                continue;
            }
            if (s != SR_DSP_BUSY) { h->stats.last_error = s; return replay_cpu(h); }
        }
        if (!drain) return 1; /* No polling loop or wait in the producer. */
        {
            uint64_t now = h->transport.monotonic_ns(h->transport.context);
            uint64_t amount;
            if (now < previous || now >= deadline) {
                ++h->stats.timeouts; h->stats.last_error = SR_DSP_ERR_TIMEOUT;
                return replay_cpu(h);
            }
            amount = deadline - now;
            if (amount > 1000000u) amount = 1000000u;
            ++h->stats.waits;
            h->transport.wait(h->transport.context, amount);
            previous = h->transport.monotonic_ns(h->transport.context);
            if (previous < now) {
                ++h->stats.timeouts; h->stats.last_error = SR_DSP_ERR_TIMEOUT;
                return replay_cpu(h);
            }
            h->stats.wait_ns += previous - now;
        }
    }
    return 1;
}
static int pump(SrSnesDspHost *h) { return pump_mode(h, 1); }
static int append_event(SrSnesDspHost *h, uint16_t type, uint32_t t,
                         uint32_t a, uint32_t b, const uint8_t *bytes);
static int checkpoint(SrSnesDspHost *h) {
    HostJob *j;
    if (!h->active || h->unsafe) return 0;
    if (!h->count || h->jobs[h->count - 1].sent) {
        if (h->count == JOURNAL) return 0;
        j = &h->jobs[h->count++]; header(h, j);
        if (!append_event(h, SR_DSP_EVENT_ADVANCE, h->now, 0, 0, NULL)) return 0;
    }
    j = &h->jobs[h->count - 1];
    sr_dsp_wire_put16(j->words, SR_DSP_REQ_FLAGS, SR_DSP_FLAG_SNAPSHOT);
    j->sealed = 0; seal(j);
    if (!pump(h)) return 0;
    if (!h->latest_valid) return 0;
    memcpy(h->saved_state, h->latest_state, sizeof(h->saved_state));
    memcpy(h->checkpoint_aram, h->aram, 65536);
    h->saved_now = h->now; h->saved_next = h->next_sample;
    h->saved_sequence = h->next_sequence;
    h->count = h->submitted = h->completed = 0;
    ++h->stats.checkpoints;
    h->stats.checkpoint_copy_bytes += 65536 + sizeof(h->saved_state);
    return 1;
}
static int reserve(SrSnesDspHost *h, unsigned needed) {
    if (!h->active || h->unsafe || needed > JOURNAL - 2u) return 0;
    if (h->count + needed >= JOURNAL - 1u && !checkpoint(h)) return 0;
    return h->count + needed < JOURNAL;
}
static int normalize_time(SrSnesDspHost *h, uint32_t *t) {
    uint32_t d = *t - h->now;
    if (d >= 0x80000000u) {
        if (h->now - *t != 1u) return 0;
        *t = h->now; /* inclusive advance consumed this exact boundary */
    }
    return 1;
}
static int append_event(SrSnesDspHost *h, uint16_t type, uint32_t t,
                         uint32_t a, uint32_t b, const uint8_t *bytes) {
    HostJob *j;
    int publish = 0;
    uint16_t pad = 0;
    uint16_t o, i, n = type == SR_DSP_EVENT_WRITE_ARAM ? (uint16_t)(b >> 16) : 0;
    uint32_t delta = t - h->now, phase = h->next_sample - h->now;
    uint32_t samples = delta > phase ? (delta - phase + 31u) >> 5 : 0;
    if (delta >= 0x80000000u || samples > SR_DSP_MAX_SAMPLES ||
        samples > PCM_FRAMES - (h->pcm_write - h->pcm_read) - h->pending_samples)
        return 0;
    j = h->count ? &h->jobs[h->count - 1] : NULL;
    if (j && n >= 32u && (j->payload & 1u)) pad = 1;
    if (!j || j->sealed || j->events == SR_DSP_MAX_EVENTS ||
        (uint32_t)j->payload + pad + n > SR_DSP_MAX_PAYLOAD || j->samples + samples > SR_DSP_MAX_SAMPLES) {
        if (h->count >= JOURNAL) return 0;
        if (j) { seal(j); publish = 1; }
        j = &h->jobs[h->count++]; header(h, j);
        pad = 0;
    }
    /* Consecutive exclusive advances have no intervening observable event.
     * Keep one frontier instead of consuming one event slot per sample. */
    if (type == SR_DSP_EVENT_ADVANCE && j->events &&
        sr_dsp_wire_u16(j->words, (uint16_t)(SR_DSP_REQUEST_EVENTS +
            (j->events - 1u) * SR_DSP_EVENT_BYTES)) == SR_DSP_EVENT_ADVANCE)
        --j->events;
    o = (uint16_t)(SR_DSP_REQUEST_EVENTS + j->events++ * SR_DSP_EVENT_BYTES);
    sr_dsp_wire_put16(j->words, o, type);
    sr_dsp_wire_put32(j->words, (uint16_t)(o + 4), t);
    sr_dsp_wire_put32(j->words, (uint16_t)(o + 8), a);
    if (n) {
        /* Large uploads can use the DSP DMA path without repacking bytes. */
        j->payload = (uint16_t)(j->payload + pad);
        b = ((uint32_t)n << 16) | j->payload;
        for (i = 0; i < n; ++i)
            sr_dsp_wire_put8(j->words, (uint16_t)(SR_DSP_REQUEST_PAYLOAD + j->payload + i), bytes[i]);
        j->payload = (uint16_t)(j->payload + n);
    }
    sr_dsp_wire_put32(j->words, (uint16_t)(o + 12), b);
    j->samples = (uint16_t)(j->samples + samples);
    h->pending_samples += samples;
    h->now = t; h->next_sample += samples * 32u;
    j->end_time = h->now; j->end_next = h->next_sample;
    if (h->count > h->stats.journal_peak) h->stats.journal_peak = h->count;
    if (j->samples == SR_DSP_MAX_SAMPLES || j->events == SR_DSP_MAX_EVENTS ||
        j->payload == SR_DSP_MAX_PAYLOAD) seal(j);
    return publish || j->sealed ? pump_mode(h, 0) : 1;
}
static int backend_restore(void *ctx, const uint16_t *state, const uint8_t *aram,
                            uint32_t now, uint32_t next_sample) {
    SrSnesDspHost *h = ctx;
    SrSnesDspBoot boot;
    SrSnesDspCore checked = h->cpu.core;
    /* Refuse invalid state before fencing/mutating the current backend. */
    if (!state || !aram || next_sample - now > 31u ||
        !sr_snes_dsp_core_load(&checked, state, SR_SNES_DSP_CORE_STATE_WORDS) ||
        !stop_remote(h)) return 0;
    sr_dsp_engine_cancel(&h->cpu);
    if (sr_dsp_engine_restore(&h->cpu, state, now, next_sample) != SR_DSP_OK) return 0;
    ++h->epoch; if (!h->epoch) ++h->epoch;
    memcpy(h->saved_state, state, sizeof(h->saved_state));
    memcpy(h->latest_state, state, sizeof(h->latest_state));
    memcpy(h->checkpoint_aram, aram, 65536);
    memcpy(h->aram, aram, 65536); memcpy(h->work_aram, aram, 65536);
    memset(h->dirty, 0, sizeof(h->dirty));
    h->dirty_begin = 65536u; h->dirty_end = 0;
    h->now = h->saved_now = now;
    h->next_sample = h->saved_next = next_sample;
    h->next_sequence = h->saved_sequence = 0;
    h->count = h->submitted = h->completed = 0;
    h->pcm_read = h->pcm_write = h->pending_samples = 0;
    h->latest_valid = h->active = 1; h->unsafe = 0;
    for (unsigned i = 0; i < 128; ++i) h->regs[i] = (uint8_t)h->cpu.core.ram[i];
    sr_dsp_engine_epoch(&h->cpu, h->epoch, 0);
    boot.state = state; boot.aram = aram; boot.epoch = h->epoch;
    boot.now = now; boot.next_sample = next_sample; boot.first_sequence = 0;
    if (h->transport.open) {
        uint16_t status = h->transport.open(h->transport.context, &boot);
        if (status == SR_DSP_OK) h->remote = 1;
        else {
            h->stats.last_error = status;
            /* Even failed setup can have an ambiguous late attach. */
            h->remote = 1;
            if (!stop_remote(h)) return 0;
        }
    }
    h->stats.epoch = h->epoch; h->stats.active_remote = (uint32_t)h->remote;
    return 1;
}
static int backend_write_reg(void *ctx, uint32_t now, uint8_t reg, uint8_t val) {
    SrSnesDspHost *h = ctx;
    if (reg >= 128 || !normalize_time(h, &now) || !reserve(h, 2)) return 0;
    return append_event(h, SR_DSP_EVENT_WRITE_REG, now, reg, val, NULL);
}
static int backend_write_aram(void *ctx, uint32_t now, uint32_t address,
                             const uint8_t *bytes, uint32_t count) {
    SrSnesDspHost *h = ctx;
    uint32_t off = 0;
    if (!count) return address <= 65536u;
    if (!bytes || address >= 65536u || count > 65536u - address ||
        !normalize_time(h, &now) || !reserve(h, (count + SR_DSP_MAX_PAYLOAD - 1u) / SR_DSP_MAX_PAYLOAD + 2u))
        return 0;
    /* Reserve all journal entries before changing anything: this callback
     * accepts an entire host mutation even when split across DMA jobs. */
    while (off < count) {
        uint32_t n = count - off;
        if (n > SR_DSP_MAX_PAYLOAD) n = SR_DSP_MAX_PAYLOAD;
        if (!append_event(h, SR_DSP_EVENT_WRITE_ARAM, now, address + off, n << 16, bytes + off)) {
            if (off == 0) return 0; /* No event of this operation accepted. */
            h->unsafe = 1; return 0; /* reserved later chunk: internal invariant */
        }
        off += n;
    }
    h->stats.aram_write_bytes += count;
    return 1;
}
static int backend_advance(void *ctx, uint32_t sample_timestamp) {
    SrSnesDspHost *h = ctx;
    if (sample_timestamp != h->next_sample || !reserve(h, 2)) return 0;
    return append_event(h, SR_DSP_EVENT_ADVANCE, sample_timestamp + 1u, 0, 0, NULL);
}
static int backend_read(void *ctx, uint8_t reg, uint8_t *value) {
    SrSnesDspHost *h = ctx;
    if (reg >= 128 || !value || !pump(h)) return 0;
    *value = h->regs[reg]; return 1;
}
static int deliver(SrSnesDspHost *h, const SrSnesDspBridgeSink *sink) {
    uint32_t a = h->dirty_begin;
    if (!sink || !sink->pcm || !sink->aram) return 0;
    while (h->pcm_read != h->pcm_write) {
        uint32_t start = h->pcm_read & (PCM_FRAMES - 1u);
        uint32_t n = h->pcm_write - h->pcm_read;
        if (n > PCM_FRAMES - start) n = PCM_FRAMES - start;
        if (!sink->pcm(sink->context, &h->pcm[start * 2], n)) return 0;
        h->pcm_read += n;
    }
    /* Empty flushes do no ARAM scan. Sparse writes skip whole clean bitmap
     * bytes; rejected sink ranges retain their bits and bounds for retry. */
    while (a < h->dirty_end) {
        uint32_t start;
        if (!h->dirty[a >> 3]) { a = (a + 8u) & ~7u; continue; }
        if (!(h->dirty[a >> 3] & (1u << (a & 7)))) { ++a; continue; }
        start = a++;
        while (a < h->dirty_end && (h->dirty[a >> 3] & (1u << (a & 7)))) ++a;
        if (!sink->aram(sink->context, start, h->aram + start, a - start)) return 0;
        for (uint32_t k = start; k < a; ++k)
            h->dirty[k >> 3] &= (uint8_t)~(1u << (k & 7));
    }
    h->dirty_begin = 65536u; h->dirty_end = 0;
    return 1;
}
static int backend_flush(void *ctx, const SrSnesDspBridgeSink *sink) {
    SrSnesDspHost *h = ctx;
    return pump(h) && deliver(h, sink);
}
static int backend_checkpoint(void *ctx, uint16_t *state) {
    SrSnesDspHost *h = ctx;
    if (!state || !checkpoint(h)) return 0;
    memcpy(state, h->saved_state, sizeof(h->saved_state));
    return 1;
}
static int backend_recover(void *ctx, const SrSnesDspBridgeSink *sink, uint16_t *state) {
    SrSnesDspHost *h = ctx;
    if (!h->active || h->unsafe || !state || !replay_cpu(h) || !deliver(h, sink)) return 0;
    return sr_snes_dsp_core_save(&h->cpu.core, state, SR_SNES_DSP_CORE_STATE_WORDS);
}
static void backend_close(void *ctx) {
    SrSnesDspHost *h = ctx;
    if (stop_remote(h)) h->active = 0;
}
SrSnesDspHost *sr_snes_dsp_host_create(const SrSnesDspTransport *transport) {
    SrSnesDspHost *h;
    SrSnesDspMemory memory;
    if (transport && (!transport->open || !transport->submit || !transport->poll ||
        !transport->wait || !transport->monotonic_ns || !transport->stop)) return NULL;
    h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    if (transport) h->transport = *transport;
    memory.context = h; memory.read8 = cpu_read; memory.write8 = cpu_write;
    sr_dsp_engine_init(&h->cpu, &memory);
    h->dirty_begin = 65536u;
    h->deadline_ns = 20000000u;
    h->stats.persistent_host_bytes = sizeof(*h);
    h->backend = (SrSnesDspBridgeBackend){SR_SNES_DSP_BRIDGE_ABI, h,
        backend_restore, backend_write_reg, backend_write_aram, backend_advance,
        backend_read, backend_flush, backend_checkpoint, backend_recover, backend_close, 0};
    return h;
}
const SrSnesDspBridgeBackend *sr_snes_dsp_host_backend(SrSnesDspHost *h) {
    return h ? &h->backend : NULL;
}
void sr_snes_dsp_host_deadline(SrSnesDspHost *h, uint64_t ns) {
    if (h && !h->active && ns && ns <= 1000000000u) h->deadline_ns = ns;
}
void sr_snes_dsp_host_stats(const SrSnesDspHost *h, SrSnesDspHostStats *out) {
    if (h && out) *out = h->stats;
}
int sr_snes_dsp_host_destroy(SrSnesDspHost *h) {
    if (!h) return 1;
    if (!stop_remote(h)) return 0;
    free(h); return 1;
}
