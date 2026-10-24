#include "snesrecomp_platform/snes_ppu_trace.h"
#include "snesrecomp_platform/snes_ppu_frame.h"
#include "snesrecomp_platform/snes_ppu_elastic.h"

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
    TRACE_HEADER_V4_BYTES = TRACE_HEADER_V3_BYTES + 4 + 4 + 8 + 10,
    TRACE_HEADER_BYTES = TRACE_HEADER_V4_BYTES + 4 + 4 + 4 + 16 + 8 + 4,
    TRACE_JOB_HEADER_BYTES = TRACE_HEADER_BYTES + 32,
    TRACE_ELASTIC_BAND_BYTES = TRACE_BAND_BYTES + SNES_PPU_BG_COUNT *
        (1 + SNES_PPU_ELASTIC_SEGMENTS * 8),
    TRACE_LEGACY_MAX_BYTES = TRACE_JOB_HEADER_BYTES + SNES_PPU_MAX_BANDS * TRACE_ELASTIC_BAND_BYTES +
        SNES_PPU_TRACE_VRAM_BYTES + SNES_PPU_TRACE_CGRAM_BYTES + SNES_PPU_TRACE_OAM_BYTES +
        SNES_PPU_TRACE_HIGH_OAM_BYTES + SNES_PPU_MAX_BANDS * SNES_PPU_TRACE_CGRAM_BYTES +
        SNES_PPU_BG_COUNT * SNESRECOMP_PPU_RESOLVED_MAP_BYTES +
        SNESRECOMP_PPU_MAX_WIDTH * SNES_PPU_MAX_BANDS * 2 + SNES_PPU_MAX_BANDS * 6 * 10 +
        SNESRECOMP_PPU_JOB_MAX_PIXELS * 4 + SNESRECOMP_PPU_JOB_MAX_IMAGES * 8 +
        SNESRECOMP_PPU_JOB_MAX_OVERLAYS * 20 + SNESRECOMP_PPU_JOB_MAX_SOURCES * 20,
    TRACE_PRIMITIVE_HEADER_BYTES = 88,
    TRACE_PRIMITIVE_MAX_BYTES = TRACE_PRIMITIVE_HEADER_BYTES + 2 * TRACE_LEGACY_MAX_BYTES +
        SNESRECOMP_PPU_JOB_MAX_PRIMITIVES * 24 + SNESRECOMP_PPU_JOB_MAX_MATERIALS * 8 +
        SNESRECOMP_PPU_JOB_MAX_IMAGES * 8 + SNESRECOMP_PPU_JOB_MAX_PIXELS * 4 +
        2 * (SNESRECOMP_OBJ_MAX_SLIVERS * 12 + SNES_PPU_MAX_BANDS),
};

static bool TraceIsJob(uint32_t version) {
    return version==SNES_PPU_TRACE_JOB_VERSION || version==SNES_PPU_TRACE_ELASTIC_JOB_VERSION ||
        version==SNES_PPU_TRACE_BRIGHTNESS_JOB_VERSION || version==SNES_PPU_TRACE_ELASTIC_BRIGHTNESS_JOB_VERSION ||
        version==SNES_PPU_TRACE_SOURCE_JOB_VERSION || version==SNES_PPU_TRACE_ELASTIC_SOURCE_JOB_VERSION ||
        version==SNES_PPU_TRACE_WINDOW_JOB_VERSION || version==SNES_PPU_TRACE_ELASTIC_WINDOW_JOB_VERSION;
}
static bool TraceHasElastic(uint32_t version) {
    return version==SNES_PPU_TRACE_ELASTIC_VERSION || version==SNES_PPU_TRACE_ELASTIC_JOB_VERSION ||
        version==SNES_PPU_TRACE_ELASTIC_BRIGHTNESS_JOB_VERSION || version==SNES_PPU_TRACE_ELASTIC_SOURCE_JOB_VERSION ||
        version==SNES_PPU_TRACE_ELASTIC_WINDOW_VERSION || version==SNES_PPU_TRACE_ELASTIC_WINDOW_JOB_VERSION;
}
static bool TraceHasSources(uint32_t version) {
    return version==SNES_PPU_TRACE_SOURCE_JOB_VERSION || version==SNES_PPU_TRACE_ELASTIC_SOURCE_JOB_VERSION ||
        version==SNES_PPU_TRACE_WINDOW_JOB_VERSION || version==SNES_PPU_TRACE_ELASTIC_WINDOW_JOB_VERSION;
}
static bool TraceHasWindows(uint32_t version) {
    return version>=SNES_PPU_TRACE_WINDOW_VERSION && version<=SNES_PPU_TRACE_ELASTIC_WINDOW_JOB_VERSION;
}

static unsigned TraceHeaderBytes(uint32_t version) {
    if(TraceIsJob(version)) return TRACE_JOB_HEADER_BYTES;
    if (version == SNES_PPU_TRACE_VERSION_V2)
        return TRACE_HEADER_V2_BYTES;
    if (version == SNES_PPU_TRACE_VERSION_V3)
        return TRACE_HEADER_V3_BYTES;
    if (version == SNES_PPU_TRACE_VERSION_V4)
        return TRACE_HEADER_V4_BYTES;
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

static void WriteBand(uint8_t **p, const SnesPpuRasterBand *b, bool elastic) {
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
    if(elastic) for(unsigned layer=0;layer<SNES_PPU_BG_COUNT;++layer) {
        const SnesPpuBgState *bg=&b->bg[layer];
        PutU8(p,bg->elastic_count);
        for(unsigned i=0;i<SNES_PPU_ELASTIC_SEGMENTS;++i) {
            const SnesPpuElasticSegment *s=&bg->elastic[i];
            PutU16(p,(uint16_t)s->source_begin);PutU16(p,(uint16_t)s->source_end);
            PutU16(p,(uint16_t)s->destination_begin);PutU16(p,(uint16_t)s->destination_end);
        }
    }
}

static void ReadBand(const uint8_t **p, SnesPpuRasterBand *b, bool elastic) {
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
    if(elastic) for(unsigned layer=0;layer<SNES_PPU_BG_COUNT;++layer) {
        SnesPpuBgState *bg=&b->bg[layer];bg->elastic_count=GetU8(p);
        for(unsigned i=0;i<SNES_PPU_ELASTIC_SEGMENTS;++i) {
            SnesPpuElasticSegment *s=&bg->elastic[i];
            s->source_begin=(int16_t)GetU16(p);s->source_end=(int16_t)GetU16(p);
            s->destination_begin=(int16_t)GetU16(p);s->destination_end=(int16_t)GetU16(p);
        }
    }
}

static uint32_t CaptureFlags(const SnesPpuFrameCapture *cap) {
    uint32_t flags = 0;
    for (unsigned i = 0; i < cap->band_count; i++) {
        for (unsigned bg = 0; bg < SNES_PPU_BG_COUNT; bg++) {
            const SnesPpuBgState *b = &cap->bands[i].bg[bg];
            if (!b->margin_repeats && (b->margin_left || b->margin_right) &&
                !cap->resolved_bg[bg])
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

static bool EncodeTrace(uint8_t **encoded,size_t *encoded_bytes,
                                const SnesPpuFrameCapture *capture,
                                uint32_t frame_id,
                                uint32_t accepted,
                                uint32_t reject_reason,const SnesRecompPpuJobView *job) {
    size_t band_bytes, oam_bytes, total, extra_bytes;
    uint32_t maps = 0;
    uint64_t extra_hash;
    uint8_t *buf, *p;
    uint64_t vram_hash, cgram_hash, capture_hash, oam_hash = 0;
    const bool have_oam = capture && capture->oam && capture->high_oam;
    const unsigned header_bytes=job?TRACE_JOB_HEADER_BYTES:TRACE_HEADER_BYTES;
    size_t job_bytes=0;

    if (!encoded || !encoded_bytes || !capture || !capture->bands || !capture->vram ||
        !capture->cgram || !capture->band_count ||
        capture->band_count > SNES_PPU_MAX_BANDS ||
        capture->cgram_line_count > SNES_PPU_MAX_BANDS ||
        (capture->cgram_line_count && (!capture->cgram_lines ||
         capture->cgram_line_count != capture->visible_height)) ||
        (capture->resolved_bg_required & ~15u) ||
        capture->canvas_width > SNESRECOMP_PPU_MAX_WIDTH ||
        capture->visible_height > SNES_PPU_MAX_BANDS ||
        (capture->obj_pixels && capture->obj_pitch < capture->canvas_width) ||
        (!capture->obj_pixels && capture->obj_pitch))
        return false;

    if(capture->resolved_windows) for(unsigned i=0;i<capture->band_count*6u;++i)
        if((capture->resolved_windows[i].selector>15u && capture->resolved_windows[i].selector!=255u) ||
            capture->resolved_windows[i].logic>3u) return false;
    bool elastic=false;
    for(unsigned bi=0;bi<capture->band_count;++bi)
        for(unsigned layer=0;layer<SNES_PPU_BG_COUNT;++layer) {
            const SnesPpuBgState *bg=&capture->bands[bi].bg[layer];
            if(!snesrecomp_ppu_elastic_valid(bg)) return false;
            elastic=elastic || bg->elastic_count!=0;
        }
    const unsigned stored_band_bytes=elastic?TRACE_ELASTIC_BAND_BYTES:TRACE_BAND_BYTES;
    bool brightness=false;
    if(job) for(unsigned i=0;i<job->overlay_count;++i)
        brightness|=job->overlays[i].brightness_reduction!=0;
    const uint32_t version=capture->resolved_windows?
        (job?(elastic?SNES_PPU_TRACE_ELASTIC_WINDOW_JOB_VERSION:SNES_PPU_TRACE_WINDOW_JOB_VERSION):
             (elastic?SNES_PPU_TRACE_ELASTIC_WINDOW_VERSION:SNES_PPU_TRACE_WINDOW_VERSION)):
        job && job->source_count?
        (elastic?SNES_PPU_TRACE_ELASTIC_SOURCE_JOB_VERSION:SNES_PPU_TRACE_SOURCE_JOB_VERSION):brightness?
        (elastic?SNES_PPU_TRACE_ELASTIC_BRIGHTNESS_JOB_VERSION:SNES_PPU_TRACE_BRIGHTNESS_JOB_VERSION):
        elastic?(job?SNES_PPU_TRACE_ELASTIC_JOB_VERSION:SNES_PPU_TRACE_ELASTIC_VERSION):
        (job?SNES_PPU_TRACE_JOB_VERSION:SNES_PPU_TRACE_VERSION);
    extra_bytes = (size_t)capture->cgram_line_count * SNES_PPU_TRACE_CGRAM_BYTES;
    if(capture->resolved_windows) extra_bytes+=(size_t)capture->band_count*6u*10u;
    if (capture->obj_pixels)
        extra_bytes += (size_t)capture->canvas_width * capture->visible_height * 2;
    for (unsigned b = 0; b < SNES_PPU_BG_COUNT; ++b)
        if (capture->resolved_bg[b]) {
            maps |= 1u << b;
            extra_bytes += SNESRECOMP_PPU_RESOLVED_MAP_BYTES;
        }

    band_bytes = (size_t)capture->band_count * stored_band_bytes;
    oam_bytes = have_oam ? (SNES_PPU_TRACE_OAM_BYTES +
                            SNES_PPU_TRACE_HIGH_OAM_BYTES) : 0u;
    if(job) {
        for(unsigned i=0;i<capture->band_count;++i)
            if(((capture->bands[i].main_enable|capture->bands[i].sub_enable)&16u) && !capture->obj_pixels) return false;
        job_bytes=(size_t)job->image_count*8u+(size_t)job->overlay_count*20u+(size_t)job->source_count*20u;
        for(unsigned i=0;i<job->image_count;++i)
            job_bytes+=(size_t)job->images[i].width*job->images[i].height*4u;
    }
    total = header_bytes + band_bytes + SNES_PPU_TRACE_VRAM_BYTES +
            SNES_PPU_TRACE_CGRAM_BYTES + oam_bytes + extra_bytes + job_bytes;

    buf = (uint8_t *)calloc(1, total);
    if (!buf)
        return false;

    /* Hashes are over the serialised band bytes rather than the in-memory
     * struct, so they do not change when padding does. */
    p = buf + header_bytes;
    for (unsigned i = 0; i < capture->band_count; i++)
        WriteBand(&p, &capture->bands[i],elastic);
    capture_hash = snesrecomp_ppu_trace_hash(buf + header_bytes,
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
        p += oam_bytes;
    }

    uint8_t *extra_begin = p;
    for (unsigned i = 0; i < capture->cgram_line_count * 256u; ++i)
        PutU16(&p, capture->cgram_lines[i]);
    for (unsigned b = 0; b < SNES_PPU_BG_COUNT; ++b)
        if (capture->resolved_bg[b]) {
            memcpy(p, capture->resolved_bg[b], SNESRECOMP_PPU_RESOLVED_MAP_BYTES);
            p += SNESRECOMP_PPU_RESOLVED_MAP_BYTES;
        }
    if (capture->obj_pixels)
        for (unsigned y = 0; y < capture->visible_height; ++y)
            for (unsigned x = 0; x < capture->canvas_width; ++x)
                PutU16(&p, capture->obj_pixels[y * capture->obj_pitch + x]);
    if(capture->resolved_windows) for(unsigned i=0;i<capture->band_count*6u;++i) {
        const SnesPpuResolvedWindow *w=capture->resolved_windows+i;
        PutU16(&p,(uint16_t)w->w1_left);PutU16(&p,(uint16_t)w->w1_right);
        PutU16(&p,(uint16_t)w->w2_left);PutU16(&p,(uint16_t)w->w2_right);
        PutU8(&p,w->selector);PutU8(&p,w->logic);
    }
    extra_hash = snesrecomp_ppu_trace_hash(extra_begin, extra_bytes);
    uint8_t *job_begin=p;
    if(job) {
        for(unsigned i=0;i<job->source_count;++i) {
            const SnesRecompPpuSource *r=job->sources+i;
            PutU16(&p,r->x);PutU16(&p,r->y);PutU16(&p,r->width);PutU16(&p,r->height);
            PutU16(&p,r->source_x);PutU16(&p,r->source_y);
            PutU16(&p,r->source_width);PutU16(&p,r->source_height);
            PutU8(&p,r->image);PutU8(&p,r->layer);PutU8(&p,r->screens);PutU8(&p,r->priority);
        }
        for(unsigned i=0;i<job->image_count;++i) {
            const SnesRecompPpuJobImage *image=job->images+i;
            PutU16(&p,image->width);PutU16(&p,image->height);
            PutU32(&p,(uint32_t)image->width*image->height);
        }
        for(unsigned i=0;i<job->overlay_count;++i) {
            const SnesRecompPpuOverlay *r=job->overlays+i;
            PutU16(&p,r->x);PutU16(&p,r->y);PutU16(&p,r->width);PutU16(&p,r->height);
            PutU16(&p,r->source_x);PutU16(&p,r->source_y);
            PutU8(&p,r->image);PutU8(&p,r->blend);PutU8(&p,r->condition);PutU8(&p,r->brightness_reduction);PutU32(&p,r->key_argb);
        }
        for(unsigned i=0;i<job->image_count;++i) {
            const SnesRecompPpuJobImage *image=job->images+i;
            for(unsigned y=0;y<image->height;++y) for(unsigned x=0;x<image->width;++x)
                PutU32(&p,image->pixels[(size_t)y*image->pitch+x]);
        }
    }
    const uint64_t job_hash=snesrecomp_ppu_trace_hash(job_begin,job_bytes);

    p = buf;
    memcpy(p, SNES_PPU_TRACE_MAGIC, SNES_PPU_TRACE_MAGIC_BYTES);
    p += SNES_PPU_TRACE_MAGIC_BYTES;
    PutU32(&p, version);
    PutU32(&p, header_bytes);
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
    PutU32(&p, stored_band_bytes);
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
    PutU32(&p, capture->cgram_line_count);
    PutU32(&p, maps);
    PutU32(&p, capture->resolved_bg_required);
    for (unsigned b = 0; b < SNES_PPU_BG_COUNT; ++b)
        PutU32(&p, capture->resolved_bg_serial[b]);
    PutU64(&p, extra_hash);
    PutU32(&p, capture->obj_pixels ? capture->canvas_width * capture->visible_height : 0);
    if(job) {
        PutU32(&p,job->image_count);PutU32(&p,job->overlay_count);
        PutU32(&p,(uint32_t)job_bytes);PutU32(&p,job->source_count);
        PutU64(&p,job->sequence);PutU64(&p,job_hash);
    }

    *encoded=buf;*encoded_bytes=total;
    return true;
}

static bool WriteBuffer(const char *path,uint8_t *buf,size_t total) {
    FILE *f=path?fopen(path,"wb"):NULL;
    bool ok=f && fwrite(buf,1,total,f)==total;
    if(f && fclose(f)!=0) ok=false;
    if(f && !ok) remove(path);
    free(buf);return ok;
}
static bool WriteTrace(const char *path,const SnesPpuFrameCapture *capture,
    uint32_t frame_id,uint32_t accepted,uint32_t reject_reason,const SnesRecompPpuJobView *job) {
    uint8_t *buf=NULL;size_t bytes=0;
    return path && EncodeTrace(&buf,&bytes,capture,frame_id,accepted,reject_reason,job) &&
        WriteBuffer(path,buf,bytes);
}

bool snesrecomp_ppu_trace_write(const char *path,const SnesPpuFrameCapture *capture,
    uint32_t frame_id,uint32_t accepted,uint32_t reject_reason) {
    return WriteTrace(path,capture,frame_id,accepted,reject_reason,NULL);
}
static bool WritePrimitiveTrace(const char *path,const SnesRecompPpuJobView *view,
    uint32_t frame_id,uint32_t accepted,uint32_t reject_reason);
static bool WriteIndexedTrace(const char *path,const SnesRecompPpuJobView *view,
    uint32_t frame_id,uint32_t accepted,uint32_t reject_reason);
bool snesrecomp_ppu_trace_write_job(const char *path,const SnesRecompPpuJob *job,
    uint32_t frame_id,uint32_t accepted,uint32_t reject_reason) {
    const SnesRecompPpuJobView *v=snesrecomp_ppu_job_view(job);
    if(v && v->indexed_layout) return WriteIndexedTrace(path,v,frame_id,accepted,reject_reason);
    return v && (v->primitive_count?WritePrimitiveTrace(path,v,frame_id,accepted,reject_reason):
        WriteTrace(path,snesrecomp_ppu_frame_capture(v->frame),frame_id,accepted,reject_reason,v));
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

static bool DecodeJob(const SnesPpuFrameCapture *capture,const uint8_t *payload,size_t bytes,
    unsigned image_count,unsigned overlay_count,unsigned source_count,uint64_t sequence,bool brightness,SnesRecompPpuJob **out) {
    SnesRecompPpuJobImage images[SNESRECOMP_PPU_JOB_MAX_IMAGES]={0};
    SnesRecompPpuOverlay overlays[SNESRECOMP_PPU_JOB_MAX_OVERLAYS]={0};
    SnesRecompPpuSource sources[SNESRECOMP_PPU_JOB_MAX_SOURCES];
    const size_t metadata=image_count*8u+overlay_count*20u+source_count*20u;
    if(metadata>bytes) return false;
    const uint8_t *p=payload;uint32_t pixel_count=0;
    for(unsigned i=0;i<source_count;++i) {
        SnesRecompPpuSource *r=sources+i;
        r->x=GetU16(&p);r->y=GetU16(&p);r->width=GetU16(&p);r->height=GetU16(&p);
        r->source_x=GetU16(&p);r->source_y=GetU16(&p);
        r->source_width=GetU16(&p);r->source_height=GetU16(&p);
        r->image=GetU8(&p);r->layer=GetU8(&p);r->screens=GetU8(&p);r->priority=GetU8(&p);
    }
    for(unsigned i=0;i<image_count;++i) {
        images[i].width=GetU16(&p);images[i].height=GetU16(&p);images[i].pitch=images[i].width;
        const uint32_t count=GetU32(&p);
        if(!images[i].width || images[i].width>SNESRECOMP_PPU_MAX_WIDTH ||
            !images[i].height || images[i].height>SNES_PPU_MAX_BANDS ||
            count!=(uint32_t)images[i].width*images[i].height ||
            count>SNESRECOMP_PPU_JOB_MAX_PIXELS-pixel_count) return false;
        pixel_count+=count;
    }
    if(metadata+(size_t)pixel_count*4u!=bytes) return false;
    for(unsigned i=0;i<overlay_count;++i) {
        SnesRecompPpuOverlay *r=overlays+i;
        r->x=GetU16(&p);r->y=GetU16(&p);r->width=GetU16(&p);r->height=GetU16(&p);
        r->source_x=GetU16(&p);r->source_y=GetU16(&p);
        r->image=GetU8(&p);r->blend=GetU8(&p);r->condition=GetU8(&p);
        r->brightness_reduction=GetU8(&p);r->key_argb=GetU32(&p);
        if(!brightness && r->brightness_reduction) return false;
    }
    uint32_t *pixels=pixel_count?malloc((size_t)pixel_count*sizeof(*pixels)):NULL;
    if(pixel_count && !pixels) return false;
    unsigned at=0;
    for(unsigned i=0;i<image_count;++i) {
        images[i].pixels=pixels+at;
        const unsigned count=(unsigned)images[i].width*images[i].height;
        for(unsigned x=0;x<count;++x) pixels[at++]=GetU32(&p);
    }
    SnesRecompPpuFrame *frame=snesrecomp_ppu_frame_create();
    SnesRecompPpuJob *job=snesrecomp_ppu_job_create(pixel_count);
    const bool ok=frame && job && snesrecomp_ppu_frame_freeze(frame,capture,NULL,sequence)==SNESRECOMP_PPU_OK &&
        snesrecomp_ppu_job_freeze_sources(job,frame,images,(uint16_t)image_count,overlays,(uint16_t)overlay_count,
            sources,(uint16_t)source_count,sequence)==SNESRECOMP_PPU_OK;
    snesrecomp_ppu_frame_release(frame);free(pixels);
    if(!ok) snesrecomp_ppu_job_release(job);
    else *out=job;
    return ok;
}

static bool DecodeTrace(uint8_t *buf,size_t file_bytes,SnesPpuTrace *out,
    SnesRecompPpuJob **job,char *error, size_t error_bytes) {
    const uint8_t *p;
    uint32_t version, header_bytes, band_count, band_bytes;
    uint32_t oam_bytes = 0, high_oam_bytes = 0;
    uint32_t palette_lines = 0, maps = 0, required = 0, obj_count = 0;
    uint64_t extra_hash = 0;
    size_t extra_bytes = 0;
    uint64_t total_bytes;
    size_t expected;
    uint32_t image_count=0,overlay_count=0,source_count=0,job_bytes=0;
    uint64_t sequence=0,job_hash=0;
    if(job) *job=NULL;
    memset(out,0,sizeof *out);
    if(file_bytes<TRACE_HEADER_V2_BYTES || file_bytes>TRACE_LEGACY_MAX_BYTES) {
        free(buf);return Fail(out,error,error_bytes,"implausible size");
    }

    if (memcmp(buf, SNES_PPU_TRACE_MAGIC, SNES_PPU_TRACE_MAGIC_BYTES) != 0) {
        free(buf);
        return Fail(out, error, error_bytes, "bad magic");
    }
    p = buf + SNES_PPU_TRACE_MAGIC_BYTES;
    version = GetU32(&p);
    if (!TraceIsJob(version) && !TraceHasWindows(version) && version != SNES_PPU_TRACE_ELASTIC_VERSION && version != SNES_PPU_TRACE_VERSION &&
        version != SNES_PPU_TRACE_VERSION_V4 &&
        version != SNES_PPU_TRACE_VERSION_V3 &&
        version != SNES_PPU_TRACE_VERSION_V2) {
        free(buf);
        return Fail(out, error, error_bytes, "unsupported version");
    }
    if(TraceIsJob(version) && !job) {
        free(buf);return Fail(out,error,error_bytes,"post-composition trace requires job reader");
    }
    if (file_bytes < TraceHeaderBytes(version)) {
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
    if (version >= SNES_PPU_TRACE_VERSION_V4) {
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
    if (version >= SNES_PPU_TRACE_VERSION) {
        palette_lines = GetU32(&p);
        maps = GetU32(&p);
        required = GetU32(&p);
        for (unsigned b = 0; b < SNES_PPU_BG_COUNT; ++b)
            out->capture.resolved_bg_serial[b] = GetU32(&p);
        extra_hash = GetU64(&p);
        obj_count = GetU32(&p);
        if (palette_lines > SNES_PPU_MAX_BANDS ||
            (palette_lines && palette_lines != out->capture.visible_height) ||
            (maps & ~15u) || (required & ~15u) ||
            out->capture.canvas_width > SNESRECOMP_PPU_MAX_WIDTH ||
            out->capture.visible_height > SNES_PPU_MAX_BANDS ||
            (obj_count && obj_count != (unsigned)out->capture.canvas_width * out->capture.visible_height)) {
            free(buf);
            return Fail(out, error, error_bytes, "invalid raster extension");
        }
        extra_bytes = (size_t)palette_lines * SNES_PPU_TRACE_CGRAM_BYTES;
        if(TraceHasWindows(version)) extra_bytes+=(size_t)band_count*6u*10u;
        extra_bytes += (size_t)obj_count * 2;
        for (unsigned b = 0; b < SNES_PPU_BG_COUNT; ++b)
            if (maps & (1u << b)) extra_bytes += SNESRECOMP_PPU_RESOLVED_MAP_BYTES;
        out->capture.resolved_bg_required = (uint8_t)required;
    }
    if(TraceIsJob(version)) {
        image_count=GetU32(&p);overlay_count=GetU32(&p);job_bytes=GetU32(&p);
        const uint32_t reserved=GetU32(&p);sequence=GetU64(&p);job_hash=GetU64(&p);
        if(TraceHasSources(version)) source_count=reserved;
        if((!TraceHasSources(version) && reserved) || source_count>SNESRECOMP_PPU_JOB_MAX_SOURCES ||
            image_count>SNESRECOMP_PPU_JOB_MAX_IMAGES || overlay_count>SNESRECOMP_PPU_JOB_MAX_OVERLAYS ||
            job_bytes>SNESRECOMP_PPU_JOB_MAX_PIXELS*4u+SNESRECOMP_PPU_JOB_MAX_IMAGES*8u+
                SNESRECOMP_PPU_JOB_MAX_OVERLAYS*20u+SNESRECOMP_PPU_JOB_MAX_SOURCES*20u) {
            free(buf);return Fail(out,error,error_bytes,"invalid post-composition extension");
        }
    } else sequence=out->frame_id;

    if (header_bytes != TraceHeaderBytes(version) ||
        band_bytes != (unsigned)(TraceHasElastic(version)?TRACE_ELASTIC_BAND_BYTES:TRACE_BAND_BYTES)) {
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
    expected = header_bytes + (size_t)band_count * band_bytes +
               SNES_PPU_TRACE_VRAM_BYTES + SNES_PPU_TRACE_CGRAM_BYTES +
               oam_bytes + high_oam_bytes + extra_bytes + job_bytes;
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
        ReadBand(&p, &out->bands[i],TraceHasElastic(version));
    for(unsigned bi=0;bi<band_count;++bi) for(unsigned layer=0;layer<SNES_PPU_BG_COUNT;++layer)
        if(!snesrecomp_ppu_elastic_valid(&out->bands[bi].bg[layer])) {
            snesrecomp_ppu_trace_free(out);
            free(buf);return Fail(out,error,error_bytes,"invalid elastic mapping");
        }
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

    p += SNES_PPU_TRACE_CGRAM_BYTES + oam_bytes + high_oam_bytes;
    if (version >= SNES_PPU_TRACE_VERSION) {
        if (snesrecomp_ppu_trace_hash(p, extra_bytes) != extra_hash) {
            snesrecomp_ppu_trace_free(out);
            free(buf);
            return Fail(out, error, error_bytes, "raster payload hash mismatch");
        }
        if (palette_lines) {
            out->cgram_lines = malloc((size_t)palette_lines * SNES_PPU_TRACE_CGRAM_BYTES);
            if (!out->cgram_lines) goto extra_oom;
            for (unsigned i = 0; i < palette_lines * 256u; ++i)
                out->cgram_lines[i] = GetU16(&p);
            out->capture.cgram_lines = out->cgram_lines;
            out->capture.cgram_line_count = palette_lines;
        }
        for (unsigned b = 0; b < SNES_PPU_BG_COUNT; ++b)
            if (maps & (1u << b)) {
                out->resolved_bg[b] = malloc(SNESRECOMP_PPU_RESOLVED_MAP_BYTES);
                if (!out->resolved_bg[b]) goto extra_oom;
                memcpy(out->resolved_bg[b], p, SNESRECOMP_PPU_RESOLVED_MAP_BYTES);
                p += SNESRECOMP_PPU_RESOLVED_MAP_BYTES;
                out->capture.resolved_bg[b] = out->resolved_bg[b];
            }
        if (obj_count) {
            out->obj_pixels = malloc((size_t)obj_count * 2);
            if (!out->obj_pixels) goto extra_oom;
            for (unsigned i = 0; i < obj_count; ++i) out->obj_pixels[i] = GetU16(&p);
            out->capture.obj_pixels = out->obj_pixels;
            out->capture.obj_pitch = out->capture.canvas_width;
        }
        if(TraceHasWindows(version)) {
            out->resolved_windows=malloc((size_t)band_count*6u*sizeof(*out->resolved_windows));
            if(!out->resolved_windows) goto extra_oom;
            for(unsigned i=0;i<band_count*6u;++i) {
                SnesPpuResolvedWindow *w=out->resolved_windows+i;
                w->w1_left=(int16_t)GetU16(&p);w->w1_right=(int16_t)GetU16(&p);
                w->w2_left=(int16_t)GetU16(&p);w->w2_right=(int16_t)GetU16(&p);
                w->selector=GetU8(&p);w->logic=GetU8(&p);
                if((w->selector>15u && w->selector!=255u) || w->logic>3u) {
                    snesrecomp_ppu_trace_free(out);free(buf);
                    return Fail(out,error,error_bytes,"invalid resolved window");
                }
            }
            out->capture.resolved_windows=out->resolved_windows;
        }
    }

    /* The stored hashes are checked, not merely carried: a file that survived
     * the size checks can still have had its payload altered. */
    if (snesrecomp_ppu_trace_hash(buf + header_bytes,
                                  (size_t)band_count * band_bytes) !=
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
    if(job) {
        if((TraceIsJob(version) && snesrecomp_ppu_trace_hash(p,job_bytes)!=job_hash) ||
            !DecodeJob(&out->capture,p,job_bytes,image_count,overlay_count,source_count,sequence,
                version==SNES_PPU_TRACE_BRIGHTNESS_JOB_VERSION ||
                version==SNES_PPU_TRACE_ELASTIC_BRIGHTNESS_JOB_VERSION || TraceHasSources(version),job)) {
            snesrecomp_ppu_trace_free(out);free(buf);
            return Fail(out,error,error_bytes,"invalid or incomplete post-composition job");
        }
    }
    free(buf);
    return true;
extra_oom:
    snesrecomp_ppu_trace_free(out);
    free(buf);
    return Fail(out, error, error_bytes, "out of memory");
}

#include "snes_ppu_primitive_trace.inc"
#include "snes_ppu_indexed_trace.inc"

static bool ReadTrace(const char *path,SnesPpuTrace *out,SnesRecompPpuJob **job,
    char *error,size_t error_bytes) {
    if(job) *job=NULL;
    if(!path || !out) return Fail(out,error,error_bytes,"bad arguments");
    memset(out,0,sizeof *out);
    FILE *f=fopen(path,"rb");
    if(!f) return Fail(out,error,error_bytes,"cannot open");
    if(fseek(f,0,SEEK_END)!=0) {fclose(f);return Fail(out,error,error_bytes,"cannot seek");}
    const long bytes=ftell(f);rewind(f);
    if(bytes<TRACE_HEADER_V2_BYTES || bytes>TRACE_PRIMITIVE_MAX_BYTES) {
        fclose(f);return Fail(out,error,error_bytes,"implausible size");
    }
    uint8_t *buf=malloc((size_t)bytes);
    if(!buf) {fclose(f);return Fail(out,error,error_bytes,"out of memory");}
    const bool read_ok=fread(buf,1,(size_t)bytes,f)==(size_t)bytes;
    fclose(f);
    if(!read_ok) {free(buf);return Fail(out,error,error_bytes,"short read");}
    const uint8_t *version=buf+SNES_PPU_TRACE_MAGIC_BYTES;
    if(!memcmp(buf,SNES_PPU_TRACE_MAGIC,SNES_PPU_TRACE_MAGIC_BYTES) &&
       GetU32(&version)==SNES_PPU_TRACE_INDEXED_JOB_VERSION)
        return DecodeIndexedTrace(buf,(size_t)bytes,out,job,error,error_bytes);
    version=buf+SNES_PPU_TRACE_MAGIC_BYTES;
    if(!memcmp(buf,SNES_PPU_TRACE_MAGIC,SNES_PPU_TRACE_MAGIC_BYTES) &&
       GetU32(&version)==SNES_PPU_TRACE_PRIMITIVE_JOB_VERSION)
        return DecodePrimitiveTrace(buf,(size_t)bytes,out,job,error,error_bytes);
    return DecodeTrace(buf,(size_t)bytes,out,job,error,error_bytes);
}

bool snesrecomp_ppu_trace_read(const char *path,SnesPpuTrace *out,char *error,size_t error_bytes) {
    return ReadTrace(path,out,NULL,error,error_bytes);
}
bool snesrecomp_ppu_trace_read_job(const char *path,SnesPpuTrace *out,SnesRecompPpuJob **job,
    char *error,size_t error_bytes) {
    if(!job) return Fail(out,error,error_bytes,"missing job output");
    return ReadTrace(path,out,job,error,error_bytes);
}

void snesrecomp_ppu_trace_free(SnesPpuTrace *trace) {
    if (!trace)
        return;
    free(trace->bands);
    free(trace->vram);
    free(trace->cgram);
    free(trace->oam);
    free(trace->high_oam);
    free(trace->cgram_lines);
    free(trace->obj_pixels);
    free(trace->resolved_windows);
    for (unsigned b = 0; b < SNES_PPU_BG_COUNT; ++b) free(trace->resolved_bg[b]);
    memset(trace, 0, sizeof *trace);
}
