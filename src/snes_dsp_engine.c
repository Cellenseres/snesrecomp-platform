/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "snesrecomp_platform/snes_dsp_engine.h"

#ifdef SR_SNES_DSP_TEAK_MODULE
extern SrSnesDspU16 sr_dsp_cache_read(void *context, SrSnesDspU16 address);
extern void sr_dsp_cache_write(void *context, SrSnesDspU16 address, SrSnesDspU16 value);
extern SrSnesDspU16 sr_dsp_cache_read16(void *, SrSnesDspU16);
extern void sr_dsp_cache_write16(void *, SrSnesDspU16, SrSnesDspU16);
extern SrSnesDspU16 sr_dsp_cache_upload(void *, SrSnesDspU16, const SrSnesDspU16 *, SrSnesDspU32);
#define memory_read sr_snes_dsp_teak_read8
#define memory_write sr_snes_dsp_teak_write8
#define MEMORY_LINKAGE
#else
#define MEMORY_LINKAGE static
#endif

static SrSnesDspU16 backend_read(SrSnesDspEngine *e, SrSnesDspU16 address) {
#ifdef SR_SNES_DSP_TEAK_MODULE
    return sr_dsp_cache_read(e->memory.context, address);
#else
    return e->memory.read8(e->memory.context, address);
#endif
}
static void backend_write(SrSnesDspEngine *e, SrSnesDspU16 address, SrSnesDspU16 value) {
#ifdef SR_SNES_DSP_TEAK_MODULE
    sr_dsp_cache_write(e->memory.context, address, value);
#else
    e->memory.write8(e->memory.context, address, value);
#endif
}
MEMORY_LINKAGE SrSnesDspU16 memory_read(void *context, SrSnesDspU16 address) {
    SrSnesDspEngine *e = (SrSnesDspEngine *)context;
    return backend_read(e, address);
}
MEMORY_LINKAGE void memory_write(void *context, SrSnesDspU16 address, SrSnesDspU16 value) {
    SrSnesDspEngine *e = (SrSnesDspEngine *)context;
    if (e->record_echo) {
        /* Logical wire words, not a native struct or byte-alias cast. */
        SrSnesDspU16 *o = e->reply + SR_DSP_REPLY_ECHO / 2u +
                          e->echo_count * (SR_DSP_ECHO_BYTES / 2u);
        o[0] = address; o[1] = value & 255u;
        /* Keep the 32-bit load opaque to the pinned Teak optimizer. */
        sr_dsp_wire_put32(o, 4u, e->next_sample);
        ++e->echo_count;
    }
    backend_write(e, address, value & 255u);
}
#ifdef SR_SNES_DSP_TEAK_MODULE
SrSnesDspU16 sr_snes_dsp_teak_read16(void *context, SrSnesDspU16 address) {
    SrSnesDspEngine *e = (SrSnesDspEngine *)context;
    return sr_dsp_cache_read16(e->memory.context, address);
}
void sr_snes_dsp_teak_write16(void *context, SrSnesDspU16 address, SrSnesDspU16 value) {
    SrSnesDspEngine *e = (SrSnesDspEngine *)context;
    if (e->record_echo) {
        SrSnesDspU16 *o = e->reply + SR_DSP_REPLY_ECHO / 2u +
                          e->echo_count * (SR_DSP_ECHO_BYTES / 2u);
        o[0] = address; o[1] = value & 255u;
        /* Keep the 32-bit load opaque to the pinned Teak optimizer. */
        sr_dsp_wire_put32(o, 4u, e->next_sample);
        o[4] = (SrSnesDspU16)(address + 1u); o[5] = value >> 8;
        o[6] = o[2]; o[7] = o[3];
        e->echo_count += 2;
    }
    sr_dsp_cache_write16(e->memory.context, address, value);
}
#endif
void sr_dsp_engine_init(SrSnesDspEngine *e, const SrSnesDspMemory *memory) {
#ifndef SR_SNES_DSP_TEAK_MODULE
    SrSnesDspMemory m;
#endif
    e->memory = *memory;
#ifdef SR_SNES_DSP_TEAK_MODULE
    /* Teak cannot materialize function or stack-local object addresses. */
    e->core.memory.context = e;
    e->core.memory.read8 = 0; e->core.memory.write8 = 0;
    sr_snes_dsp_core_reset(&e->core);
#else
    m.context = e;
    m.read8 = memory_read; m.write8 = memory_write;
    sr_snes_dsp_core_init(&e->core, &m);
#endif
    e->request = 0; e->reply = 0;
    e->epoch = 0; e->sequence = 0; e->now = 0; e->next_sample = 0;
    e->event_index = e->event_count = e->samples = e->echo_count = 0;
    e->active = e->record_echo = e->poisoned = 0;
}
SrSnesDspU16 sr_dsp_engine_restore(SrSnesDspEngine *e,
        const SrSnesDspU16 *state, SrSnesDspU32 now, SrSnesDspU32 next_sample) {
    if (e->active) return SR_DSP_BUSY;
    if (sr_dsp_u32_sub(next_sample, now) > 31UL) return SR_DSP_ERR_TIME;
    if (state) {
        if (!sr_snes_dsp_core_load(&e->core, state, SR_SNES_DSP_CORE_STATE_WORDS))
            return SR_DSP_ERR_STATE;
    } else sr_snes_dsp_core_reset(&e->core);
    e->now = now; e->next_sample = next_sample; e->poisoned = 0;
    return SR_DSP_OK;
}
void sr_dsp_engine_epoch(SrSnesDspEngine *e, SrSnesDspU32 epoch,
                         SrSnesDspU32 first_sequence) {
    if (e->active || e->poisoned) return;
    e->epoch = epoch; e->sequence = first_sequence;
}
static void reply_base(SrSnesDspEngine *e, SrSnesDspU16 status) {
    SrSnesDspU16 *r = e->reply;
    sr_dsp_wire_put32(r, 0, SR_DSP_REPLY_MAGIC);
    sr_dsp_wire_put16(r, 4, SR_DSP_ABI);
    sr_dsp_wire_put16(r, 6, SR_DSP_HEADER_BYTES);
    sr_dsp_wire_put32(r, SR_DSP_RES_EPOCH, e->epoch);
    sr_dsp_wire_put32(r, SR_DSP_RES_SEQUENCE,
                     sr_dsp_wire_u32(e->request, SR_DSP_REQ_SEQUENCE));
    sr_dsp_wire_put16(r, SR_DSP_RES_STATUS, status);
    sr_dsp_wire_put32(r, SR_DSP_RES_TIME, e->now);
    sr_dsp_wire_put32(r, SR_DSP_RES_NEXT_SAMPLE, e->next_sample);
}
static SrSnesDspU16 reject(SrSnesDspEngine *e, SrSnesDspU16 status) {
    reply_base(e, status);
#ifndef SR_SNES_DSP_TEAK_MODULE
    sr_dsp_wire_put32(e->reply, SR_DSP_RES_CRC,
                     sr_dsp_wire_crc(e->reply, SR_DSP_REPLY_CHECK_BYTES));
#endif
    return status;
}
/* Entire-program preflight, including memory spans, time wrap and capacity. */
static SrSnesDspU16 validate(const SrSnesDspEngine *e) {
    const SrSnesDspU16 *q = e->request;
    SrSnesDspU16 i, n = sr_dsp_wire_u16(q, SR_DSP_REQ_EVENTS);
    SrSnesDspU16 payload = sr_dsp_wire_u16(q, SR_DSP_REQ_PAYLOAD_BYTES);
    SrSnesDspU32 now = e->now, next = e->next_sample, total = 0;
    SrSnesDspU32 aram_total = 0;
    if (sr_dsp_wire_u32(q, 0) != SR_DSP_MAGIC ||
        sr_dsp_wire_u16(q, 4) != SR_DSP_ABI) return SR_DSP_ERR_ABI;
    if (sr_dsp_wire_u16(q, 6) != SR_DSP_HEADER_BYTES ||
        sr_dsp_wire_u32(q, 8) != SR_DSP_SLOT_BYTES ||
        !n || n > SR_DSP_MAX_EVENTS || payload > SR_DSP_MAX_PAYLOAD)
        return SR_DSP_ERR_SIZE;
    if (!e->epoch || sr_dsp_wire_u32(q, SR_DSP_REQ_EPOCH) != e->epoch)
        return SR_DSP_ERR_EPOCH;
    if (sr_dsp_wire_u32(q, SR_DSP_REQ_SEQUENCE) != e->sequence)
        return SR_DSP_ERR_SEQUENCE;
    if (sr_dsp_wire_u32(q, SR_DSP_REQ_START_TIME) != now)
        return SR_DSP_ERR_TIME;
    if (sr_dsp_wire_u16(q, SR_DSP_REQ_FLAGS) & ~SR_DSP_FLAG_SNAPSHOT)
        return SR_DSP_ERR_ABI;
    for (i = 32u; i < 60u; i += 2u)
        if (sr_dsp_wire_u16(q, i)) return SR_DSP_ERR_ABI;
    if (sr_dsp_wire_u32(q, SR_DSP_REQ_CRC) !=
        sr_dsp_wire_crc(q, SR_DSP_REQUEST_CHECK_BYTES)) return SR_DSP_ERR_TRANSPORT;
    for (i = 0; i < n; ++i) {
        SrSnesDspU16 o = (SrSnesDspU16)(SR_DSP_REQUEST_EVENTS + i * SR_DSP_EVENT_BYTES);
        SrSnesDspU16 type = sr_dsp_wire_u16(q, o);
        SrSnesDspU32 t = sr_dsp_wire_u32(q, (SrSnesDspU16)(o + 4u));
        SrSnesDspU32 a = sr_dsp_wire_u32(q, (SrSnesDspU16)(o + 8u));
        SrSnesDspU32 b = sr_dsp_wire_u32(q, (SrSnesDspU16)(o + 12u));
        SrSnesDspU32 delta = sr_dsp_u32_sub(t, now);
        SrSnesDspU32 phase = sr_dsp_u32_sub(next, now);
        if (sr_dsp_wire_u16(q, (SrSnesDspU16)(o + 2u))) return SR_DSP_ERR_ABI;
        if (delta >= 0x80000000UL) return SR_DSP_ERR_TIME;
        if (delta > 8192UL) return SR_DSP_ERR_CAPACITY;
        if (delta > phase) {
            SrSnesDspU32 count = (delta - phase + 31UL) >> 5;
            total += count; next = sr_dsp_u32_add(next, count << 5);
        }
        if (total > SR_DSP_MAX_SAMPLES ||
            total > sr_dsp_wire_u16(q, SR_DSP_REQ_SAMPLE_LIMIT))
            return SR_DSP_ERR_CAPACITY;
        now = t;
        if (type == SR_DSP_EVENT_WRITE_REG) {
            if (a > 127UL || b > 255UL) return SR_DSP_ERR_EVENT;
        } else if (type == SR_DSP_EVENT_WRITE_ARAM) {
            SrSnesDspU32 count = b >> 16, off = b & 0xffffUL;
            if (a > 65535UL || !count || count > 65536UL - a ||
                off > payload || count > (SrSnesDspU32)payload - off)
                return SR_DSP_ERR_EVENT;
            /* Reusing payload spans must not amplify a bounded slot into
             * hundreds of KiB of synchronous DMA/cache work in one slice. */
            aram_total += count;
            if (aram_total > SR_DSP_MAX_PAYLOAD) return SR_DSP_ERR_CAPACITY;
        } else if (type != SR_DSP_EVENT_ADVANCE || a || b)
            return SR_DSP_ERR_EVENT;
    }
    return SR_DSP_OK;
}
SrSnesDspU16 sr_dsp_engine_begin(SrSnesDspEngine *e,
        const SrSnesDspU16 *request, SrSnesDspU16 *reply) {
    SrSnesDspU16 i, status;
    if (e->active) return SR_DSP_BUSY;
    if (e->poisoned) return SR_DSP_ERR_STATE;
    e->request = request; e->reply = reply;
    /* Active payload/PCM/echo/state words are initialized before publication.
     * CRC and DMA exclude unused gaps. Keep reserved header and all register
     * words deterministic, including error replies. */
    for (i = 0; i < SR_DSP_REPLY_PCM / 2u; ++i) reply[i] = 0;
    status = validate(e);
    if (status != SR_DSP_OK) return reject(e, status);
    e->event_count = sr_dsp_wire_u16(request, SR_DSP_REQ_EVENTS);
    e->event_index = e->samples = e->echo_count = 0;
    e->active = 1;
    return SR_DSP_OK;
}
static void finish(SrSnesDspEngine *e) {
    SrSnesDspU16 i;
    reply_base(e, SR_DSP_OK);
    sr_dsp_wire_put16(e->reply, SR_DSP_RES_SAMPLES, e->samples);
    sr_dsp_wire_put16(e->reply, SR_DSP_RES_ECHO_COUNT, e->echo_count);
    for (i = 0; i < 128u; ++i)
        sr_dsp_wire_put8(e->reply, (SrSnesDspU16)(SR_DSP_REPLY_REGS + i),
                         sr_snes_dsp_core_read(&e->core, i));
    if (sr_dsp_wire_u16(e->request, SR_DSP_REQ_FLAGS) & SR_DSP_FLAG_SNAPSHOT) {
        sr_snes_dsp_core_save(&e->core, e->reply + SR_DSP_REPLY_STATE / 2u,
                              SR_SNES_DSP_CORE_STATE_WORDS);
        sr_dsp_wire_put16(e->reply, SR_DSP_RES_STATE_WORDS, SR_SNES_DSP_CORE_STATE_WORDS);
    }
#ifndef SR_SNES_DSP_TEAK_MODULE
    sr_dsp_wire_put32(e->reply, SR_DSP_RES_CRC,
                     sr_dsp_wire_crc(e->reply, SR_DSP_REPLY_CHECK_BYTES));
#endif
    e->active = 0; e->sequence = sr_dsp_u32_add(e->sequence, 1UL);
}
SrSnesDspU16 sr_dsp_engine_step(SrSnesDspEngine *e, SrSnesDspU16 budget) {
    if (!e->active) return SR_DSP_ERR_STATE;
    if (!budget || budget > SR_DSP_MAX_SAMPLES) return SR_DSP_ERR_SIZE;
    while (e->event_index < e->event_count) {
        SrSnesDspU16 o = (SrSnesDspU16)(SR_DSP_REQUEST_EVENTS + e->event_index * SR_DSP_EVENT_BYTES);
        SrSnesDspU32 t = sr_dsp_wire_u32(e->request, (SrSnesDspU16)(o + 4u));
        if (sr_dsp_u32_sub(t, e->now) > sr_dsp_u32_sub(e->next_sample, e->now)) {
            SrSnesDspS16 *pcm = (SrSnesDspS16 *)(e->reply +
                SR_DSP_REPLY_PCM / 2u + e->samples * 2u);
            SrSnesDspU16 po;
            if (!budget) return SR_DSP_BUSY;
            --budget;
            e->record_echo = 1;
            sr_snes_dsp_core_cycle(&e->core, pcm);
            e->record_echo = 0;
            po = (SrSnesDspU16)(SR_DSP_REPLY_PCM + e->samples * 4u);
            sr_dsp_wire_put16(e->reply, po, (SrSnesDspU16)pcm[0]);
            sr_dsp_wire_put16(e->reply, (SrSnesDspU16)(po + 2u), (SrSnesDspU16)pcm[1]);
            ++e->samples;
            e->now = e->next_sample;
            e->next_sample = sr_dsp_u32_add(e->next_sample, 32UL);
        } else {
            SrSnesDspU16 type = sr_dsp_wire_u16(e->request, o);
            SrSnesDspU32 a = sr_dsp_wire_u32(e->request, (SrSnesDspU16)(o + 8u));
            SrSnesDspU32 b = sr_dsp_wire_u32(e->request, (SrSnesDspU16)(o + 12u));
            e->now = t;
            if (type == SR_DSP_EVENT_WRITE_REG)
                sr_snes_dsp_core_write(&e->core, (SrSnesDspU16)a, (SrSnesDspU16)b);
            else if (type == SR_DSP_EVENT_WRITE_ARAM) {
                SrSnesDspU16 i, count = (SrSnesDspU16)(b >> 16);
                SrSnesDspU16 off = (SrSnesDspU16)b;
#ifdef SR_SNES_DSP_TEAK_MODULE
                if (!sr_dsp_cache_upload(e->memory.context, (SrSnesDspU16)a,
                        e->request + SR_DSP_REQUEST_PAYLOAD / 2u, b))
#endif
                for (i = 0; i < count; ++i)
                    backend_write(e, (SrSnesDspU16)(a + i),
                        sr_dsp_wire_u8(e->request, (SrSnesDspU16)(SR_DSP_REQUEST_PAYLOAD + off + i)));
            }
            ++e->event_index;
        }
    }
    finish(e);
    return SR_DSP_OK;
}
void sr_dsp_engine_cancel(SrSnesDspEngine *e) {
    e->active = e->record_echo = 0;
    e->epoch = 0;
    e->poisoned = 1;
    e->request = 0; e->reply = 0;
}
