#include "snesrecomp_platform/snes_ppu_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every field is written through these, so the on-disk layout is the same
 * whatever the compiler decides about padding, enum width or member order. */
static void PutU8(uint8_t **p, uint8_t v) { *(*p)++ = v; }

static void PutU16(uint8_t **p, uint16_t v) {
    PutU8(p, (uint8_t)(v & 0xFFu));
    PutU8(p, (uint8_t)(v >> 8));
}

static void PutU32(uint8_t **p, uint32_t v) {
    PutU16(p, (uint16_t)(v & 0xFFFFu));
    PutU16(p, (uint16_t)(v >> 16));
}

static void PutU64(uint8_t **p, uint64_t v) {
    PutU32(p, (uint32_t)(v & 0xFFFFFFFFu));
    PutU32(p, (uint32_t)(v >> 32));
}

static uint8_t GetU8(const uint8_t **p) { return *(*p)++; }

static uint16_t GetU16(const uint8_t **p) {
    const uint16_t lo = GetU8(p);
    return (uint16_t)(lo | ((uint16_t)GetU8(p) << 8));
}

static uint32_t GetU32(const uint8_t **p) {
    const uint32_t lo = GetU16(p);
    return lo | ((uint32_t)GetU16(p) << 16);
}

static uint64_t GetU64(const uint8_t **p) {
    const uint64_t lo = GetU32(p);
    return lo | ((uint64_t)GetU32(p) << 32);
}

uint64_t snesrecomp_ppu_trace_hash(const void *data, size_t bytes) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ull;      /* FNV-1a offset basis */
    for (size_t i = 0; i < bytes; i++) {
        h ^= p[i];
        h *= 1099511628211ull;                /* FNV-1a prime */
    }
    return h;
}

/* Bytes one band occupies on disk. Kept as a constant the reader checks
 * against the file's own count, so a version that grows a band is rejected
 * rather than misread. */
enum {
    TRACE_BG_BYTES = 2 + 2 + 2 + 2 + 1 + 1 + 1 + 1 + 1 + 1 + 1,   /* 15 */
    TRACE_BAND_BYTES = 2 + 2 + 1 + 1 + (TRACE_BG_BYTES * SNES_PPU_BG_COUNT) +
                       1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 2 + 1 + 4 +
                       1 + 1 + 1 + 1 + SNES_PPU_REG_BLOCK_BYTES,
    TRACE_HEADER_V2_BYTES = SNES_PPU_TRACE_MAGIC_BYTES + 4 + 4 + 8 +
                            4 + 4 + 4 +
                            2 + 2 + 2 + 2 +
                            4 + 4 + 4 +
                            8 + 8 + 8 +
                            8 + 4,
    TRACE_HEADER_V3_BYTES = TRACE_HEADER_V2_BYTES + 2,
    /* v4 adds the two OAM payload sizes, the OAM hash and the ten host OBJ
     * policy bytes. Sizes are stored rather than assumed so a reader rejects a
     * file whose OAM section is not exactly the tables it claims. */
    TRACE_HEADER_BYTES = TRACE_HEADER_V3_BYTES + 4 + 4 + 8 + 10,
};

static unsigned TraceHeaderBytes(uint32_t version) {
    if (version == SNES_PPU_TRACE_VERSION_V2)
        return TRACE_HEADER_V2_BYTES;
    if (version == SNES_PPU_TRACE_VERSION_V3)
        return TRACE_HEADER_V3_BYTES;
    return TRACE_HEADER_BYTES;
}

static void WriteBg(uint8_t **p, const SnesPpuBgState *b) {
    PutU16(p, b->tilemap_word_addr);
    PutU16(p, b->char_word_addr);
    PutU16(p, b->h_scroll);
    PutU16(p, b->v_scroll);
    PutU8(p, b->wide ? 1u : 0u);
    PutU8(p, b->tall ? 1u : 0u);
    PutU8(p, b->big_tiles ? 1u : 0u);
    PutU8(p, b->bpp);
    PutU8(p, b->margin_left);
    PutU8(p, b->margin_right);
    PutU8(p, b->margin_repeats ? 1u : 0u);
}

static void ReadBg(const uint8_t **p, SnesPpuBgState *b) {
    b->tilemap_word_addr = GetU16(p);
    b->char_word_addr = GetU16(p);
    b->h_scroll = GetU16(p);
    b->v_scroll = GetU16(p);
    b->wide = GetU8(p) != 0;
    b->tall = GetU8(p) != 0;
    b->big_tiles = GetU8(p) != 0;
    b->bpp = GetU8(p);
    b->margin_left = GetU8(p);
    b->margin_right = GetU8(p);
    b->margin_repeats = GetU8(p) != 0;
}

static void WriteBand(uint8_t **p, const SnesPpuRasterBand *b) {
    PutU16(p, b->y_begin);
    PutU16(p, b->y_end);
    PutU8(p, b->bg_mode);
    PutU8(p, b->bg3_priority ? 1u : 0u);
    for (unsigned i = 0; i < SNES_PPU_BG_COUNT; i++)
        WriteBg(p, &b->bg[i]);
    PutU8(p, b->main_enable);
    PutU8(p, b->sub_enable);
    PutU8(p, b->main_window_enable);
    PutU8(p, b->sub_window_enable);
    PutU8(p, b->brightness);
    PutU8(p, b->forced_blank ? 1u : 0u);
    PutU8(p, b->cgwsel);
    PutU8(p, b->cgadsub);
    PutU16(p, b->fixed_colour);
    PutU8(p, b->mosaic);
    PutU32(p, b->window_sel);
    PutU8(p, b->window1_left);
    PutU8(p, b->window1_right);
    PutU8(p, b->window2_left);
    PutU8(p, b->window2_right);
    for (unsigned i = 0; i < SNES_PPU_REG_BLOCK_BYTES; i++)
        PutU8(p, b->regs[i]);
}

static void ReadBand(const uint8_t **p, SnesPpuRasterBand *b) {
    memset(b, 0, sizeof *b);
    b->y_begin = GetU16(p);
    b->y_end = GetU16(p);
    b->bg_mode = GetU8(p);
    b->bg3_priority = GetU8(p) != 0;
    for (unsigned i = 0; i < SNES_PPU_BG_COUNT; i++)
        ReadBg(p, &b->bg[i]);
    b->main_enable = GetU8(p);
    b->sub_enable = GetU8(p);
    b->main_window_enable = GetU8(p);
    b->sub_window_enable = GetU8(p);
    b->brightness = GetU8(p);
    b->forced_blank = GetU8(p) != 0;
    b->cgwsel = GetU8(p);
    b->cgadsub = GetU8(p);
    b->fixed_colour = GetU16(p);
    b->mosaic = GetU8(p);
    b->window_sel = GetU32(p);
    b->window1_left = GetU8(p);
    b->window1_right = GetU8(p);
    b->window2_left = GetU8(p);
    b->window2_right = GetU8(p);
    for (unsigned i = 0; i < SNES_PPU_REG_BLOCK_BYTES; i++)
        b->regs[i] = GetU8(p);
}

static uint32_t CaptureFlags(const SnesPpuFrameCapture *cap) {
    uint32_t flags = 0;
    for (unsigned i = 0; i < cap->band_count; i++) {
        for (unsigned bg = 0; bg < SNES_PPU_BG_COUNT; bg++) {
            const SnesPpuBgState *b = &cap->bands[i].bg[bg];
            if (!b->margin_repeats && (b->margin_left || b->margin_right))
                flags |= SNES_PPU_TRACE_FLAG_NEEDS_WS_SHADOW;
        }
    }
    if (cap->raster_memory_flags & SNES_PPU_RASTER_MEMORY_VRAM)
        flags |= SNES_PPU_TRACE_FLAG_RASTER_VRAM;
    if (cap->raster_memory_flags & SNES_PPU_RASTER_MEMORY_CGRAM)
        flags |= SNES_PPU_TRACE_FLAG_RASTER_CGRAM;
    if (cap->raster_memory_flags & SNES_PPU_RASTER_MEMORY_OAM)
        flags |= SNES_PPU_TRACE_FLAG_RASTER_OAM;
    if (cap->raster_memory_flags & SNES_PPU_RASTER_MEMORY_UNKNOWN)
        flags |= SNES_PPU_TRACE_FLAG_RASTER_UNKNOWN;
    return flags;
}

bool snesrecomp_ppu_trace_write(const char *path,
                                const SnesPpuFrameCapture *capture,
                                uint32_t frame_id,
                                uint32_t accepted,
                                uint32_t reject_reason) {
    size_t band_bytes, oam_bytes, total;
    uint8_t *buf, *p;
    FILE *f;
    uint64_t vram_hash, cgram_hash, capture_hash, oam_hash = 0;
    const bool have_oam = capture && capture->oam && capture->high_oam;

    if (!path || !capture || !capture->bands || !capture->vram ||
        !capture->cgram || !capture->band_count ||
        capture->band_count > SNES_PPU_MAX_BANDS)
        return false;

    band_bytes = (size_t)capture->band_count * TRACE_BAND_BYTES;
    oam_bytes = have_oam ? (SNES_PPU_TRACE_OAM_BYTES +
                            SNES_PPU_TRACE_HIGH_OAM_BYTES) : 0u;
    total = TRACE_HEADER_BYTES + band_bytes + SNES_PPU_TRACE_VRAM_BYTES +
            SNES_PPU_TRACE_CGRAM_BYTES + oam_bytes;

    buf = (uint8_t *)calloc(1, total);
    if (!buf)
        return false;

    /* Hashes are over the serialised band bytes rather than the in-memory
     * struct, so they do not change when padding does. */
    p = buf + TRACE_HEADER_BYTES;
    for (unsigned i = 0; i < capture->band_count; i++)
        WriteBand(&p, &capture->bands[i]);
    capture_hash = snesrecomp_ppu_trace_hash(buf + TRACE_HEADER_BYTES,
                                             band_bytes);

    memcpy(p, capture->vram, SNES_PPU_TRACE_VRAM_BYTES);
    vram_hash = snesrecomp_ppu_trace_hash(p, SNES_PPU_TRACE_VRAM_BYTES);
    p += SNES_PPU_TRACE_VRAM_BYTES;

    memcpy(p, capture->cgram, SNES_PPU_TRACE_CGRAM_BYTES);
    cgram_hash = snesrecomp_ppu_trace_hash(p, SNES_PPU_TRACE_CGRAM_BYTES);
    p += SNES_PPU_TRACE_CGRAM_BYTES;

    if (have_oam) {
        memcpy(p, capture->oam, SNES_PPU_TRACE_OAM_BYTES);
        memcpy(p + SNES_PPU_TRACE_OAM_BYTES, capture->high_oam,
               SNES_PPU_TRACE_HIGH_OAM_BYTES);
        /* One hash over both tables: they are only meaningful together. */
        oam_hash = snesrecomp_ppu_trace_hash(p, oam_bytes);
    }

    p = buf;
    memcpy(p, SNES_PPU_TRACE_MAGIC, SNES_PPU_TRACE_MAGIC_BYTES);
    p += SNES_PPU_TRACE_MAGIC_BYTES;
    PutU32(&p, SNES_PPU_TRACE_VERSION);
    PutU32(&p, TRACE_HEADER_BYTES);
    PutU64(&p, (uint64_t)total);
    PutU32(&p, frame_id);
    PutU32(&p, CaptureFlags(capture));
    PutU32(&p, capture->band_count);
    PutU16(&p, capture->native_width);
    PutU16(&p, capture->canvas_width);
    PutU16(&p, capture->canvas_extra);
    PutU16(&p, capture->visible_height);
    PutU32(&p, accepted);
    PutU32(&p, reject_reason);
    PutU32(&p, TRACE_BAND_BYTES);
    PutU64(&p, vram_hash);
    PutU64(&p, cgram_hash);
    PutU64(&p, capture_hash);
    PutU8(&p, capture->layout.extra_left_right);
    PutU8(&p, capture->layout.extra_left_cur);
    PutU8(&p, capture->layout.extra_right_cur);
    PutU8(&p, capture->layout.layer_widen_mask);
    PutU8(&p, capture->layout.layer_clamp);
    PutU8(&p, capture->layout.layer_mirror);
    PutU8(&p, capture->layout.layer_repeat);
    PutU8(&p, capture->layout.bg3_widen_y);
    PutU8(&p, capture->layout.window_expand_layers);
    PutU8(&p, capture->layout.window_expand_windows);
    PutU32(&p, capture->layout.render_flags);
    PutU32(&p, have_oam ? SNES_PPU_TRACE_OAM_BYTES : 0u);
    PutU32(&p, have_oam ? SNES_PPU_TRACE_HIGH_OAM_BYTES : 0u);
    PutU64(&p, oam_hash);
    PutU8(&p, capture->layout.hud_split_height);
    PutU8(&p, capture->layout.hud_left_end);
    PutU8(&p, capture->layout.hud_right_start);
    PutU8(&p, capture->layout.hud_oam_first_slot);
    PutU8(&p, capture->layout.hud_oam_slots);
    PutU8(&p, capture->layout.hud_oam_height);
    PutU8(&p, capture->layout.hud_oam_first_slot2);
    PutU8(&p, capture->layout.hud_oam_slots2);
    PutU8(&p, capture->layout.oam_left_hint_strict);
    PutU8(&p, capture->layout.oam_right_hint_strict);

    f = fopen(path, "wb");
    if (!f) {
        free(buf);
        return false;
    }
    if (fwrite(buf, 1, total, f) != total) {
        fclose(f);
        remove(path);
        free(buf);
        return false;
    }
    fclose(f);
    free(buf);
    return true;
}

static bool Fail(SnesPpuTrace *out, char *error, size_t error_bytes,
                 const char *why) {
    if (out)
        memset(out, 0, sizeof *out);
    if (error && error_bytes) {
        size_t n = strlen(why);
        if (n >= error_bytes)
            n = error_bytes - 1;
        memcpy(error, why, n);
        error[n] = 0;
    }
    return false;
}

bool snesrecomp_ppu_trace_read(const char *path, SnesPpuTrace *out,
                               char *error, size_t error_bytes) {
    FILE *f;
    long file_bytes;
    uint8_t *buf;
    const uint8_t *p;
    uint32_t version, header_bytes, band_count, band_bytes;
    uint32_t oam_bytes = 0, high_oam_bytes = 0;
    uint64_t total_bytes;
    size_t expected;

    if (!path || !out)
        return Fail(out, error, error_bytes, "bad arguments");
    memset(out, 0, sizeof *out);

    f = fopen(path, "rb");
    if (!f)
        return Fail(out, error, error_bytes, "cannot open");
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return Fail(out, error, error_bytes, "cannot seek");
    }
    file_bytes = ftell(f);
    rewind(f);
    /* An upper bound before allocating: a foreign file must not be able to ask
     * for an arbitrary amount of memory. */
    if (file_bytes < (long)TRACE_HEADER_V2_BYTES ||
        file_bytes > (long)(TRACE_HEADER_BYTES +
                            (size_t)SNES_PPU_MAX_BANDS * TRACE_BAND_BYTES +
                            SNES_PPU_TRACE_VRAM_BYTES +
                            SNES_PPU_TRACE_CGRAM_BYTES +
                            SNES_PPU_TRACE_OAM_BYTES +
                            SNES_PPU_TRACE_HIGH_OAM_BYTES)) {
        fclose(f);
        return Fail(out, error, error_bytes, "implausible size");
    }

    buf = (uint8_t *)malloc((size_t)file_bytes);
    if (!buf) {
        fclose(f);
        return Fail(out, error, error_bytes, "out of memory");
    }
    if (fread(buf, 1, (size_t)file_bytes, f) != (size_t)file_bytes) {
        fclose(f);
        free(buf);
        return Fail(out, error, error_bytes, "short read");
    }
    fclose(f);

    if (memcmp(buf, SNES_PPU_TRACE_MAGIC, SNES_PPU_TRACE_MAGIC_BYTES) != 0) {
        free(buf);
        return Fail(out, error, error_bytes, "bad magic");
    }
    p = buf + SNES_PPU_TRACE_MAGIC_BYTES;
    version = GetU32(&p);
    if (version != SNES_PPU_TRACE_VERSION &&
        version != SNES_PPU_TRACE_VERSION_V3 &&
        version != SNES_PPU_TRACE_VERSION_V2) {
        free(buf);
        return Fail(out, error, error_bytes, "unsupported version");
    }
    if (file_bytes < (long)TraceHeaderBytes(version)) {
        free(buf);
        return Fail(out, error, error_bytes, "truncated header");
    }
    header_bytes = GetU32(&p);
    total_bytes = GetU64(&p);
    out->frame_id = GetU32(&p);
    out->flags = GetU32(&p);
    band_count = GetU32(&p);
    out->capture.native_width = GetU16(&p);
    out->capture.canvas_width = GetU16(&p);
    out->capture.canvas_extra = GetU16(&p);
    out->capture.visible_height = GetU16(&p);
    out->accepted = GetU32(&p);
    out->reject_reason = GetU32(&p);
    band_bytes = GetU32(&p);
    out->vram_hash = GetU64(&p);
    out->cgram_hash = GetU64(&p);
    out->capture_hash = GetU64(&p);
    out->capture.layout.extra_left_right = GetU8(&p);
    out->capture.layout.extra_left_cur = GetU8(&p);
    out->capture.layout.extra_right_cur = GetU8(&p);
    out->capture.layout.layer_widen_mask = GetU8(&p);
    out->capture.layout.layer_clamp = GetU8(&p);
    out->capture.layout.layer_mirror = GetU8(&p);
    out->capture.layout.layer_repeat = GetU8(&p);
    out->capture.layout.bg3_widen_y = GetU8(&p);
    if (version >= SNES_PPU_TRACE_VERSION_V3) {
        out->capture.layout.window_expand_layers = GetU8(&p);
        out->capture.layout.window_expand_windows = GetU8(&p);
    }
    out->capture.layout.render_flags = GetU32(&p);
    if (version >= SNES_PPU_TRACE_VERSION) {
        oam_bytes = GetU32(&p);
        high_oam_bytes = GetU32(&p);
        out->oam_hash = GetU64(&p);
        out->capture.layout.hud_split_height = GetU8(&p);
        out->capture.layout.hud_left_end = GetU8(&p);
        out->capture.layout.hud_right_start = GetU8(&p);
        out->capture.layout.hud_oam_first_slot = GetU8(&p);
        out->capture.layout.hud_oam_slots = GetU8(&p);
        out->capture.layout.hud_oam_height = GetU8(&p);
        out->capture.layout.hud_oam_first_slot2 = GetU8(&p);
        out->capture.layout.hud_oam_slots2 = GetU8(&p);
        out->capture.layout.oam_left_hint_strict = GetU8(&p);
        out->capture.layout.oam_right_hint_strict = GetU8(&p);
    }

    if (header_bytes != TraceHeaderBytes(version) ||
        band_bytes != TRACE_BAND_BYTES) {
        free(buf);
        return Fail(out, error, error_bytes, "header layout mismatch");
    }
    /* Either both tables, or neither. A partial OAM section would describe
     * sprites the reader cannot evaluate. */
    if ((oam_bytes != 0 || high_oam_bytes != 0) &&
        (oam_bytes != SNES_PPU_TRACE_OAM_BYTES ||
         high_oam_bytes != SNES_PPU_TRACE_HIGH_OAM_BYTES)) {
        free(buf);
        return Fail(out, error, error_bytes, "oam section mismatch");
    }
    if (!band_count || band_count > SNES_PPU_MAX_BANDS) {
        free(buf);
        return Fail(out, error, error_bytes, "band count out of range");
    }
    expected = header_bytes + (size_t)band_count * TRACE_BAND_BYTES +
               SNES_PPU_TRACE_VRAM_BYTES + SNES_PPU_TRACE_CGRAM_BYTES +
               oam_bytes + high_oam_bytes;
    if (total_bytes != (uint64_t)expected ||
        (size_t)file_bytes != expected) {
        free(buf);
        return Fail(out, error, error_bytes, "truncated or inconsistent");
    }

    out->bands = (SnesPpuRasterBand *)calloc(band_count, sizeof *out->bands);
    out->vram = (uint16_t *)malloc(SNES_PPU_TRACE_VRAM_BYTES);
    out->cgram = (uint16_t *)malloc(SNES_PPU_TRACE_CGRAM_BYTES);
    if (!out->bands || !out->vram || !out->cgram) {
        snesrecomp_ppu_trace_free(out);
        free(buf);
        return Fail(out, error, error_bytes, "out of memory");
    }
    if (oam_bytes) {
        out->oam = (uint16_t *)malloc(SNES_PPU_TRACE_OAM_BYTES);
        out->high_oam = (uint8_t *)malloc(SNES_PPU_TRACE_HIGH_OAM_BYTES);
        if (!out->oam || !out->high_oam) {
            snesrecomp_ppu_trace_free(out);
            free(buf);
            return Fail(out, error, error_bytes, "out of memory");
        }
    }

    p = buf + header_bytes;
    for (uint32_t i = 0; i < band_count; i++)
        ReadBand(&p, &out->bands[i]);
    memcpy(out->vram, p, SNES_PPU_TRACE_VRAM_BYTES);
    p += SNES_PPU_TRACE_VRAM_BYTES;
    memcpy(out->cgram, p, SNES_PPU_TRACE_CGRAM_BYTES);
    if (oam_bytes) {
        const uint8_t *oam_payload = p + SNES_PPU_TRACE_CGRAM_BYTES;
        memcpy(out->oam, oam_payload, SNES_PPU_TRACE_OAM_BYTES);
        memcpy(out->high_oam, oam_payload + SNES_PPU_TRACE_OAM_BYTES,
               SNES_PPU_TRACE_HIGH_OAM_BYTES);
        if (snesrecomp_ppu_trace_hash(oam_payload,
                (size_t)oam_bytes + high_oam_bytes) != out->oam_hash) {
            snesrecomp_ppu_trace_free(out);
            free(buf);
            return Fail(out, error, error_bytes, "payload hash mismatch");
        }
    }

    /* The stored hashes are checked, not merely carried: a file that survived
     * the size checks can still have had its payload altered. */
    if (snesrecomp_ppu_trace_hash(buf + header_bytes,
                                  (size_t)band_count * TRACE_BAND_BYTES) !=
            out->capture_hash ||
        snesrecomp_ppu_trace_hash(out->vram, SNES_PPU_TRACE_VRAM_BYTES) !=
            out->vram_hash ||
        snesrecomp_ppu_trace_hash(out->cgram, SNES_PPU_TRACE_CGRAM_BYTES) !=
            out->cgram_hash) {
        snesrecomp_ppu_trace_free(out);
        free(buf);
        return Fail(out, error, error_bytes, "payload hash mismatch");
    }

    out->format_version = version;
    out->capture.bands = out->bands;
    out->capture.band_count = band_count;
    out->capture.vram = out->vram;
    out->capture.cgram = out->cgram;
    out->capture.oam = out->oam;
    out->capture.high_oam = out->high_oam;
    if (out->flags & SNES_PPU_TRACE_FLAG_RASTER_VRAM)
        out->capture.raster_memory_flags |= SNES_PPU_RASTER_MEMORY_VRAM;
    if (out->flags & SNES_PPU_TRACE_FLAG_RASTER_CGRAM)
        out->capture.raster_memory_flags |= SNES_PPU_RASTER_MEMORY_CGRAM;
    if (out->flags & SNES_PPU_TRACE_FLAG_RASTER_OAM)
        out->capture.raster_memory_flags |= SNES_PPU_RASTER_MEMORY_OAM;
    if (out->flags & SNES_PPU_TRACE_FLAG_RASTER_UNKNOWN)
        out->capture.raster_memory_flags |= SNES_PPU_RASTER_MEMORY_UNKNOWN;
    free(buf);
    return true;
}

void snesrecomp_ppu_trace_free(SnesPpuTrace *trace) {
    if (!trace)
        return;
    free(trace->bands);
    free(trace->vram);
    free(trace->cgram);
    free(trace->oam);
    free(trace->high_oam);
    memset(trace, 0, sizeof *trace);
}
