/* Appended to the hash-checked pinned dsp.c. The original implementation is
 * retained as sr_reference_dsp_*; the existing host PCM ring is unchanged.
 * Hold the core's existing RtlApuLock across every public operation. */
#include "snesrecomp_platform/snes_dsp_bridge.h"

#include "snes_dsp_bridge_state.inc"

static SrSnesDspBridgeBackend sr_bridge_backend;
static SrSnesDspBridgeStats sr_bridge_metrics;
static Apu *sr_bridge_apu;
static Dsp *sr_bridge_dsp;
static uint32_t sr_bridge_now;
static uint32_t sr_bridge_next_sample;
static unsigned sr_bridge_depth;
/* Host-only baseline, not DSP internal memory. No per-sample copies. */
static uint8_t sr_bridge_known_aram[65536];
/* Page footprints cover outstanding echo writes and borrowed native lvalues.
 * Echo timing is predicted with the same small integer recurrence as the
 * pinned DSP; no samples, envelope or FIR work is duplicated on ARM. */
static uint8_t sr_bridge_echo_pages[32], sr_bridge_native_pages[32];
/* Eight 32-byte blocks per page. The page bit is also the validity tag,
 * so starting a transaction still clears only the existing 32-byte map. */
static uint8_t sr_bridge_native_blocks[256];
static unsigned sr_bridge_native_first, sr_bridge_native_last;
static uint8_t sr_bridge_reg_shadow[128];
static uint16_t sr_bridge_echo_address, sr_bridge_echo_index;
static uint16_t sr_bridge_echo_delay, sr_bridge_echo_remain;
static unsigned sr_bridge_echo_writes, sr_bridge_tracked;
static void sr_bridge_collect(Dsp *dsp);

static void sr_bridge_predictor_reset(Dsp *dsp) {
    memcpy(sr_bridge_reg_shadow, dsp->ram, 128);
    sr_bridge_echo_address = dsp->echoBufferAdr;
    sr_bridge_echo_index = dsp->echoBufferIndex;
    sr_bridge_echo_delay = dsp->echoDelay;
    sr_bridge_echo_remain = dsp->echoRemain;
    sr_bridge_echo_writes = dsp->echoWrites;
    memset(sr_bridge_echo_pages, 0, sizeof(sr_bridge_echo_pages));
}
static void sr_bridge_predict_sample(void) {
    if (sr_bridge_echo_writes) {
        uint16_t first = (uint16_t)(sr_bridge_echo_address + (sr_bridge_echo_index << 2));
        unsigned a = first >> 8;
        unsigned b = (uint16_t)(first + 3u) >> 8;
        sr_bridge_echo_pages[a >> 3] |= (uint8_t)(1u << (a & 7u));
        sr_bridge_echo_pages[b >> 3] |= (uint8_t)(1u << (b & 7u));
    }
    ++sr_bridge_echo_index;
    if (--sr_bridge_echo_remain == 0) {
        sr_bridge_echo_remain = sr_bridge_echo_delay;
        sr_bridge_echo_index = 0;
    }
}


static int sr_bridge_remote(void) {
    return (sr_bridge_backend.flags & SR_SNES_DSP_BRIDGE_REMOTE_OUTPUT) != 0;
}

static void sr_bridge_fatal(const char *reason) {
    fprintf(stderr, "[snes-dsp] cannot preserve guest audio state: %s\n", reason);
    abort();
}

static int sr_bridge_pcm(void *context, const int16_t *stereo, uint32_t frames) {
    Dsp *dsp = context;
    const uint32_t fill = dsp->sampleWrite - dsp->sampleRead;
    if (!stereo || fill > DSP_SAMPLE_RING || frames > DSP_SAMPLE_RING - fill ||
        frames > sr_bridge_metrics.pending_samples)
        return 0;
    for (uint32_t i = 0; i < frames; ++i) {
        const uint32_t at = dsp->sampleWrite & (DSP_SAMPLE_RING - 1);
        dsp->sampleBuffer[at * 2] = stereo[i * 2];
        dsp->sampleBuffer[at * 2 + 1] = stereo[i * 2 + 1];
        ++dsp->sampleWrite;
        audio_trace_on_sample(stereo[i * 2], stereo[i * 2 + 1], 0,
                              dsp->sampleWrite - dsp->sampleRead);
    }
    sr_bridge_metrics.pending_samples -= frames;
    sr_bridge_metrics.completed_samples += frames;
    return 1;
}

static int sr_bridge_aram(void *context, uint32_t address,
                          const uint8_t *bytes, uint32_t count) {
    Dsp *dsp = context;
    if (!bytes || address > 65536u || count > 65536u - address)
        return 0;
    memcpy(dsp->apu_ram + address, bytes, count);
    memcpy(sr_bridge_known_aram + address, bytes, count);
    sr_bridge_metrics.aram_download_bytes += count;
    return 1;
}

static SrSnesDspBridgeSink sr_bridge_sink(Dsp *dsp) {
    const SrSnesDspBridgeSink sink = {dsp, sr_bridge_pcm, sr_bridge_aram};
    return sink;
}

static void sr_bridge_close(void) {
    if (sr_bridge_dsp && sr_bridge_backend.close)
        sr_bridge_backend.close(sr_bridge_backend.context);
    sr_bridge_dsp = NULL;
    sr_bridge_apu = NULL;
    sr_bridge_metrics.active = 0;
    sr_bridge_metrics.pending_samples = 0;
    sr_bridge_depth = sr_bridge_tracked = 0;
}

static void sr_bridge_recover(Dsp *dsp) {
    uint16_t state[SR_SNES_DSP_CORE_STATE_WORDS];
    uint8_t *transaction = NULL;
    /* A tracked transaction can still have earlier echo pending. Preserve
     * only unsent native mutations, never its stale unmodified echo bytes.
     * Allocation/scanning is confined to recovery, not normal frames. */
    if (sr_bridge_depth) {
        transaction = calloc(1, 65536 + 8192);
        if (!transaction) sr_bridge_fatal("recovery allocation failed");
        memcpy(transaction, dsp->apu_ram, 65536);
        for (uint32_t at = 0; at < 65536; ++at)
            if (transaction[at] != sr_bridge_known_aram[at])
                transaction[65536 + (at >> 3)] |= (uint8_t)(1u << (at & 7u));
    }
    const SrSnesDspBridgeSink sink = sr_bridge_sink(dsp);
    if (!sr_bridge_backend.recover ||
        !sr_bridge_backend.recover(sr_bridge_backend.context, &sink, state) ||
        sr_bridge_metrics.pending_samples != 0 ||
        !sr_bridge_import_state(dsp, state)) {
        free(transaction);
        sr_bridge_fatal("backend recovery did not produce a proven checkpoint");
    }
    if (transaction) {
        for (uint32_t at = 0; at < 65536; ++at)
            if (transaction[65536 + (at >> 3)] & (1u << (at & 7u)))
                dsp->apu_ram[at] = transaction[at];
        free(transaction);
    }
    ++sr_bridge_metrics.recoveries;
    sr_bridge_close();
}

static void sr_bridge_flush(Dsp *dsp) {
    if (dsp != sr_bridge_dsp) return;
    if (sr_bridge_depth && sr_bridge_tracked) sr_bridge_collect(dsp);
    if (dsp != sr_bridge_dsp) return;
    const SrSnesDspBridgeSink sink = sr_bridge_sink(dsp);
    ++sr_bridge_metrics.synchronizations;
    if (!sr_bridge_backend.flush(sr_bridge_backend.context, &sink)) {
        sr_bridge_recover(dsp);
        return;
    }
    if (sr_bridge_metrics.pending_samples != 0)
        sr_bridge_fatal("successful flush omitted accepted PCM frames");
    memset(sr_bridge_echo_pages, 0, sizeof(sr_bridge_echo_pages));
}

/* Output-ring readers: a remote-output backend never fills this ring. */
static void sr_bridge_pcm_flush(Dsp *dsp) {
    if (dsp == sr_bridge_dsp && sr_bridge_remote()) return;
    sr_bridge_flush(dsp);
}

int snesrecomp_dsp_bridge_register(const SrSnesDspBridgeBackend *backend) {
    if (sr_bridge_dsp) return 0;
    if (backend &&
        (backend->abi != SR_SNES_DSP_BRIDGE_ABI || !backend->restore ||
         !backend->write_reg || !backend->write_aram || !backend->advance ||
         !backend->read_reg || !backend->flush || !backend->checkpoint ||
         !backend->recover || !backend->close ||
         (backend->flags & ~SR_SNES_DSP_BRIDGE_REMOTE_OUTPUT)))
        return 0;
    if (backend) sr_bridge_backend = *backend;
    else memset(&sr_bridge_backend, 0, sizeof(sr_bridge_backend));
    memset(&sr_bridge_metrics, 0, sizeof(sr_bridge_metrics));
    return 1;
}

int snesrecomp_dsp_bridge_attach(Apu *apu) {
    uint16_t state[SR_SNES_DSP_CORE_STATE_WORDS];
    if (!apu || !apu->dsp) return 0;
    if (sr_bridge_dsp == apu->dsp) return 1;
    if (sr_bridge_dsp || !sr_bridge_backend.restore || apu->dsp->shadow)
        return 0;
    if (!sr_bridge_export_state(apu->dsp, state)) return 0;
    const uint32_t next = (apu->cycles + 31u) & ~31u;
    if (!sr_bridge_backend.restore(sr_bridge_backend.context, state,
                                    apu->ram, apu->cycles, next)) {
        /* restore failure MUST have fenced all prior epochs. */
        sr_bridge_backend.close(sr_bridge_backend.context);
        return 0;
    }
    sr_bridge_dsp = apu->dsp;
    sr_bridge_apu = apu;
    sr_bridge_now = apu->cycles;
    sr_bridge_next_sample = next;
    sr_bridge_depth = 0;
    memcpy(sr_bridge_known_aram, apu->ram, 65536);
    sr_bridge_metrics.aram_upload_bytes += 65536;
    sr_bridge_metrics.pending_samples = 0;
    sr_bridge_metrics.active = 1;
    sr_bridge_predictor_reset(apu->dsp);
    return 1;
}

void snesrecomp_dsp_bridge_detach(Dsp *dsp) {
    if (dsp != sr_bridge_dsp) return;
    snesrecomp_dsp_bridge_before_saveload(dsp);
    if (dsp == sr_bridge_dsp) sr_bridge_close();
}

int snesrecomp_dsp_bridge_active(const Dsp *dsp) {
    return dsp && dsp == sr_bridge_dsp;
}

void snesrecomp_dsp_bridge_clock(Dsp *dsp, uint32_t apu_cycle) {
    if (dsp == sr_bridge_dsp) sr_bridge_now = apu_cycle;
}

void snesrecomp_dsp_bridge_before_read(Dsp *dsp) { sr_bridge_flush(dsp); }

void snesrecomp_dsp_bridge_before_saveload(Dsp *dsp) {
    uint16_t state[SR_SNES_DSP_CORE_STATE_WORDS];
    if (dsp != sr_bridge_dsp) return;
    sr_bridge_flush(dsp);
    if (dsp != sr_bridge_dsp) return;
    if (!sr_bridge_backend.checkpoint(sr_bridge_backend.context, state) ||
        !sr_bridge_import_state(dsp, state))
        sr_bridge_recover(dsp);
}

void snesrecomp_dsp_bridge_aram_written(Dsp *dsp, uint32_t address,
                                       uint32_t count) {
    if (dsp != sr_bridge_dsp || count == 0) return;
    if (address > 65536u || count > 65536u - address)
        sr_bridge_fatal("host ARAM write range is invalid");
    while (count && dsp == sr_bridge_dsp) {
        uint8_t bytes[256];
        const uint32_t n = count > sizeof(bytes) ? sizeof(bytes) : count;
        memcpy(bytes, dsp->apu_ram + address, n);
        if (!sr_bridge_backend.write_aram(sr_bridge_backend.context,
                                          sr_bridge_now, address, bytes, n)) {
            uint8_t *remaining = malloc(count);
            if (!remaining) sr_bridge_fatal("recovery allocation failed");
            memcpy(remaining, dsp->apu_ram + address, count);
            sr_bridge_recover(dsp);
            /* Every not-yet-accepted byte of this host operation follows
             * recovered echo, including chunks after the failed chunk. */
            memcpy(dsp->apu_ram + address, remaining, count);
            free(remaining);
            return;
        }
        memcpy(sr_bridge_known_aram + address, bytes, n);
        sr_bridge_metrics.aram_upload_bytes += n;
        address += n;
        count -= n;
    }
}

void snesrecomp_dsp_bridge_native_begin(Apu *apu) {
    if (!apu || apu->dsp != sr_bridge_dsp) return;
    sr_bridge_now = apu->cycles;
    if (sr_bridge_depth) {
        if (sr_bridge_tracked) sr_bridge_fatal("mixed tracked/native transaction");
        ++sr_bridge_depth; return;
    }
    sr_bridge_tracked = 0;
    /* Do not mark the transaction open until its initial echo barrier has
     * succeeded. Recovery before entry must retain newly completed echo,
     * not preserve the host's still-stale pre-barrier ARAM image. */
    sr_bridge_flush(apu->dsp);
    if (apu->dsp == sr_bridge_dsp) sr_bridge_depth = 1;
}

/* One compare per 32 equal bytes. */
static int sr_bridge_block_equal(const uint8_t *actual, const uint8_t *known) {
    uint32_t a[8], k[8], diff = 0;
    memcpy(a, actual, sizeof(a));
    memcpy(k, known, sizeof(k));
    for (unsigned i = 0; i < 8; ++i) diff |= a[i] ^ k[i];
    return diff == 0;
}

static void sr_bridge_collect(Dsp *dsp) {
    const unsigned first = sr_bridge_tracked ? sr_bridge_native_first : 0u;
    const unsigned last = sr_bridge_tracked ? sr_bridge_native_last : 255u;
    for (uint32_t page = first; page <= last && dsp == sr_bridge_dsp; ++page) {
        if (sr_bridge_tracked) {
            const uint8_t group = sr_bridge_native_pages[page >> 3];
            /* Eight untouched pages at once. */
            if (!group) { page |= 7u; continue; }
            if (!(group & (1u << (page & 7u)))) continue;
        }
        if (!sr_bridge_tracked) sr_bridge_metrics.aram_scan_bytes += 256;
        const uint32_t end = (page + 1u) * 256u;
        for (uint32_t at = page * 256u; at < end && dsp == sr_bridge_dsp;) {
            if (sr_bridge_tracked && (at & 31u) == 0u) {
                if (!(sr_bridge_native_blocks[page] & (1u << ((at >> 5) & 7u)))) {
                    at += 32;
                    continue;
                }
                sr_bridge_metrics.aram_scan_bytes += 32;
            }
            if ((at & 31u) == 0u &&
                sr_bridge_block_equal(dsp->apu_ram + at, sr_bridge_known_aram + at)) {
                at += 32;
                continue;
            }
            /* memcpy of a fixed word is alignment/alias safe; ARM lowers it
             * to word loads. Unequal words still resolve exact byte runs. */
            if ((at & 3u) == 0u && end - at >= 4u) {
                uint32_t actual, known;
                memcpy(&actual, dsp->apu_ram + at, sizeof(actual));
                memcpy(&known, sr_bridge_known_aram + at, sizeof(known));
                if (actual == known) { at += 4; continue; }
            }
            if (dsp->apu_ram[at] == sr_bridge_known_aram[at]) { ++at; continue; }
            const uint32_t run_end = sr_bridge_tracked ? (at | 31u) + 1u : end;
            const uint32_t begin = at++;
            while (at < run_end && dsp->apu_ram[at] != sr_bridge_known_aram[at]) ++at;
            snesrecomp_dsp_bridge_aram_written(dsp, begin, at - begin);
        }
    }
}
void snesrecomp_dsp_bridge_native_begin_tracked(Apu *apu) {
    if (!apu || apu->dsp != sr_bridge_dsp) return;
    if (sr_bridge_depth) sr_bridge_fatal("mixed tracked/native transaction");
    sr_bridge_now = apu->cycles;
    sr_bridge_depth = sr_bridge_tracked = 1;
    sr_bridge_native_first = 256u;
    sr_bridge_native_last = 0u;
    memset(sr_bridge_native_pages, 0, sizeof(sr_bridge_native_pages));
    ++sr_bridge_metrics.tracked_transactions;
}
void snesrecomp_dsp_bridge_native_access(Apu *apu, uint32_t address,
                                         uint32_t count, int writable) {
    if (!apu || apu->dsp != sr_bridge_dsp) return;
    if (!sr_bridge_tracked || !sr_bridge_depth || address > 65536u ||
        count > 65536u - address) sr_bridge_fatal("invalid tracked native access");
    if (!count) return;
    unsigned first = address >> 8, last = (address + count - 1u) >> 8;
    /* Grouped page-mask test keeps the same first-overlap barrier. */
    for (unsigned group = first >> 3; group <= (last >> 3); ++group) {
        const unsigned low = group == (first >> 3) ? first & 7u : 0u;
        const unsigned high = group == (last >> 3) ? last & 7u : 7u;
        const uint8_t mask = (uint8_t)((0xffu << low) & (0xffu >> (7u - high)));
        if (sr_bridge_echo_pages[group] & mask) {
            ++sr_bridge_metrics.range_fences;
            sr_bridge_flush(apu->dsp);
            break;
        }
    }
    if (writable && apu->dsp == sr_bridge_dsp) {
        if (first < sr_bridge_native_first) sr_bridge_native_first = first;
        if (last > sr_bridge_native_last) sr_bridge_native_last = last;
        for (unsigned page = first; page <= last; ++page) {
            const uint8_t page_bit = (uint8_t)(1u << (page & 7u));
            if (!(sr_bridge_native_pages[page >> 3] & page_bit))
                sr_bridge_native_blocks[page] = 0;
            sr_bridge_native_pages[page >> 3] |= page_bit;
            const unsigned low = page == first ? ((address >> 5) & 7u) : 0u;
            const unsigned high = page == last ? (((address + count - 1u) >> 5) & 7u) : 7u;
            sr_bridge_native_blocks[page] |= (uint8_t)((0xffu << low) & (0xffu >> (7u - high)));
        }
    }
}
void snesrecomp_dsp_bridge_native_end(Apu *apu) {
    if (!apu || apu->dsp != sr_bridge_dsp) return;
    if (sr_bridge_depth == 0) sr_bridge_fatal("unbalanced native ARAM transaction");
    if (sr_bridge_depth > 1) { --sr_bridge_depth; return; }
    sr_bridge_collect(apu->dsp);
    sr_bridge_depth = sr_bridge_tracked = 0;
}

void snesrecomp_dsp_bridge_stats(SrSnesDspBridgeStats *out) {
    if (out) *out = sr_bridge_metrics;
}

Dsp *dsp_init(uint8_t *ram) { return sr_reference_dsp_init(ram); }

void dsp_free(Dsp *dsp) {
    if (dsp == sr_bridge_dsp) sr_bridge_close();
    sr_reference_dsp_free(dsp);
}

void dsp_reset(Dsp *dsp) {
    /* Reset deliberately cancels queued old audio; close fences DMA. */
    if (dsp == sr_bridge_dsp) sr_bridge_close();
    sr_reference_dsp_reset(dsp);
}

void dsp_cycle(Dsp *dsp) {
    if (dsp == sr_bridge_dsp) {
        if (sr_bridge_depth || sr_bridge_now != sr_bridge_next_sample)
            sr_bridge_fatal("sample outside the explicit APU boundary");
        const uint32_t fill = dsp->sampleWrite - dsp->sampleRead;
        const int remote = sr_bridge_remote();
        if (!remote && (fill > DSP_SAMPLE_RING || sr_bridge_metrics.pending_samples >=
                                                  DSP_SAMPLE_RING - fill))
            sr_bridge_fatal("host output ring exhausted; pacing required");
        if (sr_bridge_backend.advance(sr_bridge_backend.context, sr_bridge_now)) {
            sr_bridge_predict_sample();
            sr_bridge_next_sample += 32;
            if (!remote) ++sr_bridge_metrics.pending_samples;
            ++sr_bridge_metrics.submitted_samples;
            if (sr_bridge_metrics.pending_samples > sr_bridge_metrics.max_pending_samples)
                sr_bridge_metrics.max_pending_samples = sr_bridge_metrics.pending_samples;
            return;
        }
        sr_bridge_recover(dsp);
    }
    sr_reference_dsp_cycle(dsp);
}

uint8_t dsp_read(Dsp *dsp, uint8_t address) {
    if (dsp == sr_bridge_dsp) {
        uint8_t value;
        /* ADSR1/ADSR2 are written configuration. The exact synthesis core
         * never modifies them, unlike ENVX/OUTX/ENDX. */
        if ((address & 15u) == 5u || (address & 15u) == 6u) {
            ++sr_bridge_metrics.shadow_reads;
            return sr_bridge_reg_shadow[address & 127u];
        }
        /* A remote read_reg fences registers itself, without ARAM. */
        if (sr_bridge_remote()) ++sr_bridge_metrics.synchronizations;
        else sr_bridge_flush(dsp);
        if (dsp == sr_bridge_dsp &&
            sr_bridge_backend.read_reg(sr_bridge_backend.context, address, &value))
            return value;
        if (dsp == sr_bridge_dsp) sr_bridge_recover(dsp);
    }
    return sr_reference_dsp_read(dsp, address);
}

void dsp_write(Dsp *dsp, uint8_t address, uint8_t value) {
    if (dsp == sr_bridge_dsp) {
        if (sr_bridge_backend.write_reg(sr_bridge_backend.context, sr_bridge_now,
                                         address, value)) {
            sr_bridge_reg_shadow[address & 127u] = value;
            switch (address & 127u) {
            case 0x6c: sr_bridge_echo_writes = !(value & 32u); break;
            case 0x6d: sr_bridge_echo_address = (uint16_t)value << 8; break;
            case 0x7d:
                sr_bridge_echo_delay = (value & 15u) << 9;
                if (!sr_bridge_echo_delay) sr_bridge_echo_delay = 1;
                break;
            }
            audio_trace_on_reg_write(address, value);
            return;
        }
        sr_bridge_recover(dsp);
    }
    sr_reference_dsp_write(dsp, address, value);
}

void dsp_saveload(Dsp *dsp, SaveLoadInfo *sli) {
    if (dsp != sr_bridge_dsp) { sr_reference_dsp_saveload(dsp, sli); return; }
    /* apu_saveload already fenced BEFORE serializing ARAM. Keep the pinned
     * blob layout, so old CPU states and rollback output-ring rules work. */
    if (!sli->peek) snesrecomp_dsp_bridge_before_saveload(dsp);
    sr_reference_dsp_saveload(dsp, sli);
    if (sli->peek && dsp == sr_bridge_dsp) {
        uint16_t state[SR_SNES_DSP_CORE_STATE_WORDS];
        sr_bridge_now = sr_bridge_apu->cycles;
        sr_bridge_next_sample = (sr_bridge_now + 31u) & ~31u;
        if (!sr_bridge_export_state(dsp, state) ||
            !sr_bridge_backend.restore(sr_bridge_backend.context, state,
                                        dsp->apu_ram, sr_bridge_now,
                                        sr_bridge_next_sample)) {
            sr_bridge_close();
            return;
        }
        memcpy(sr_bridge_known_aram, dsp->apu_ram, 65536);
        sr_bridge_metrics.aram_upload_bytes += 65536;
        sr_bridge_metrics.pending_samples = 0;
        sr_bridge_predictor_reset(dsp);
    }
}

uint32_t dsp_available(const Dsp *dsp) {
    sr_bridge_pcm_flush((Dsp *)dsp);
    return sr_reference_dsp_available(dsp);
}

uint32_t dsp_getSamples(Dsp *dsp, int16_t *samples, int frames) {
    sr_bridge_pcm_flush(dsp);
    return sr_reference_dsp_getSamples(dsp, samples, frames);
}

uint32_t dsp_trimSamples(Dsp *dsp, uint32_t keep) {
    sr_bridge_pcm_flush(dsp);
    return sr_reference_dsp_trimSamples(dsp, keep);
}

void dsp_output_ring_save(const Dsp *dsp, DspOutputRing *out) {
    sr_bridge_pcm_flush((Dsp *)dsp);
    sr_reference_dsp_output_ring_save(dsp, out);
}

void dsp_output_ring_restore(Dsp *dsp, const DspOutputRing *in) {
    sr_bridge_pcm_flush(dsp);
    sr_reference_dsp_output_ring_restore(dsp, in);
}

void dsp_output_ring_set_write(Dsp *dsp, uint32_t write) {
    sr_bridge_pcm_flush(dsp);
    sr_reference_dsp_output_ring_set_write(dsp, write);
}

uint32_t dsp_output_ring_write(const Dsp *dsp) {
    sr_bridge_pcm_flush((Dsp *)dsp);
    return sr_reference_dsp_output_ring_write(dsp);
}
