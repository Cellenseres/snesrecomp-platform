#include "snesrecomp_platform/snes_ppu_frame.h"
#include "snes_ppu_memory.h"
#include "snesrecomp_platform/snes_ppu_elastic.h"

#include <stdlib.h>
#include <string.h>

struct SnesRecompPpuFrame {
    const SnesRecompPpuFrame *storage;
    SnesRecompPpuFrameVersions storage_versions;
    unsigned references;
    bool valid, has_obj;
    uint64_t sequence;
    SnesRecompPpuFrameVersions versions;
    SnesPpuFrameCapture capture;
    SnesRecompObjFrame obj;
    uint16_t vram[SNES_PPU_VRAM_WORDS];
    uint16_t cgram[SNES_PPU_CGRAM_ENTRIES];
    uint16_t oam[SNES_PPU_OAM_WORDS];
    uint8_t high_oam[SNES_PPU_HIGH_OAM_BYTES];
    uint16_t palettes[SNES_PPU_MAX_BANDS * SNES_PPU_CGRAM_ENTRIES];
    SnesPpuRasterBand bands[SNES_PPU_MAX_BANDS];
    SnesPpuResolvedWindow windows[SNES_PPU_MAX_BANDS*6];
    uint8_t maps[SNES_PPU_BG_COUNT][SNESRECOMP_PPU_RESOLVED_MAP_BYTES];
    SnesRecompObjSliver slivers[SNESRECOMP_OBJ_MAX_SLIVERS];
    uint8_t priorities[SNES_PPU_MAX_BANDS];
    uint16_t obj_pixels[SNESRECOMP_PPU_MAX_WIDTH * SNES_PPU_MAX_BANDS];
    uint64_t obj_coverage[SNES_PPU_MAX_BANDS];
};
/* Like frame retain/freeze, creation belongs to the host thread. Workers only
 * borrow immutable views; they do not create or mutate frame owners. */
static uint64_t s_frame_owner;

SnesRecompPpuFrame *snesrecomp_ppu_frame_create(void) {
    if(s_frame_owner==UINT64_MAX) return NULL;
    SnesRecompPpuFrame *f = calloc(1, sizeof(*f));
    if (f) { f->references = 1;f->versions.owner=++s_frame_owner; }
    return f;
}

void snesrecomp_ppu_frame_retain(const SnesRecompPpuFrame *frame) {
    if (frame) ++((SnesRecompPpuFrame *)frame)->references;
}

void snesrecomp_ppu_frame_release(const SnesRecompPpuFrame *frame) {
    if (frame && --((SnesRecompPpuFrame *)frame)->references == 0) {
        snesrecomp_ppu_frame_release(frame->storage);
        free((void *)frame);
    }
}

SnesRecompPpuResult snesrecomp_ppu_frame_discard_raster_view(SnesRecompPpuFrame *f) {
    if(!f) return SNESRECOMP_PPU_INVALID;
    if(!f->storage) return SNESRECOMP_PPU_OK;
    if(f->references!=1) return SNESRECOMP_PPU_BUSY;
    snesrecomp_ppu_frame_release(f->storage);f->storage=NULL;f->valid=false;
    return SNESRECOMP_PPU_OK;
}

SnesRecompPpuResult snesrecomp_ppu_frame_freeze_raster_view(SnesRecompPpuFrame *f,
    const SnesRecompPpuFrame *source,const SnesPpuRasterBand *bands,
    uint16_t count,uint64_t sequence) {
    if(!f || !source || !source->valid || f==source || !bands || !count || count>SNES_PPU_MAX_BANDS)
        return SNESRECOMP_PPU_INVALID;
    if(f->references!=1) return SNESRECOMP_PPU_BUSY;
    /* Flatten the storage dependency. Raster rows belong to this view, so
     * intermediate views can be released independently without a chain. */
    const SnesRecompPpuFrame *storage=source->storage?source->storage:source;
    if(storage==f) return SNESRECOMP_PPU_INVALID;
    if(source->capture.resolved_windows && count!=source->capture.band_count)
        return SNESRECOMP_PPU_INCOMPLETE_LAYOUT;
    unsigned end=0;
    for(unsigned b=0;b<count;++b) {
        if(bands[b].y_begin!=end || bands[b].y_end<=end || bands[b].y_end>source->capture.visible_height)
            return SNESRECOMP_PPU_INVALID;
        if(source->capture.resolved_windows &&
           (bands[b].y_begin!=source->bands[b].y_begin || bands[b].y_end!=source->bands[b].y_end))
            return SNESRECOMP_PPU_INCOMPLETE_LAYOUT;
        for(unsigned layer=0;layer<SNES_PPU_BG_COUNT;++layer)
            if(!snesrecomp_ppu_elastic_valid(&bands[b].bg[layer])) return SNESRECOMP_PPU_INVALID;
        if(((bands[b].main_enable|bands[b].sub_enable)&16u) &&
            !source->has_obj && !source->capture.obj_pixels) return SNESRECOMP_PPU_INCOMPLETE_MEMORY;
        end=bands[b].y_end;
    }
    if(end!=source->capture.visible_height) return SNESRECOMP_PPU_INVALID;
    const bool same_owner=f->storage_versions.owner==storage->versions.owner;
    const bool vram=same_owner && f->storage_versions.vram==storage->versions.vram;
    const bool palette=same_owner && f->storage_versions.palette==storage->versions.palette;
    const bool objects=same_owner && f->storage_versions.objects==storage->versions.objects;
    if((!vram && f->versions.vram==UINT64_MAX) || (!palette && f->versions.palette==UINT64_MAX) ||
       (!objects && f->versions.objects==UINT64_MAX)) return SNESRECOMP_PPU_RESOURCE_LIMIT;
    /* Everything above is preflight. Retain before releasing an old view's
     * last owner; the caller may have borrowed that same snapshot. */
    snesrecomp_ppu_frame_retain(storage);
    const SnesRecompPpuFrame *previous=f->storage;
    const SnesPpuFrameCapture capture=source->capture;
    memmove(f->bands,bands,(size_t)count*sizeof *bands);
    if(capture.resolved_windows) memcpy(f->windows,capture.resolved_windows,(size_t)count*6u*sizeof *f->windows);
    f->capture=capture;f->capture.bands=f->bands;f->capture.band_count=count;
    if(capture.resolved_windows) f->capture.resolved_windows=f->windows;
    f->storage=storage;f->storage_versions=storage->versions;
    f->has_obj=source->has_obj;
    if(!vram) ++f->versions.vram;
    if(!palette) ++f->versions.palette;
    if(!objects) ++f->versions.objects;
    f->sequence=sequence;f->valid=true;
    snesrecomp_ppu_frame_release(previous);
    return SNESRECOMP_PPU_OK;
}

static uint32_t InvalidObjPair(uint32_t pair) {
    /* Each qualified 16-bit pixel must select an OBJ palette. Works in either
     * byte order; memcpy permits unaligned capture storage. */
    return (pair&0xf000f000u)|(((pair&0x08000800u)>>4)&~pair);
}
static SnesRecompPpuResult Validate(const SnesPpuFrameCapture *c,
    const SnesRecompObjFrame *obj, const SnesRecompPpuFrame *previous,
    bool *same_objects, uint64_t *coverage) {
    *same_objects=false;
    if (!c || !c->vram || !c->cgram || !c->bands ||
        c->native_width != 256 || c->canvas_width < 256 ||
        c->canvas_width > SNESRECOMP_PPU_MAX_WIDTH ||
        c->canvas_extra > c->canvas_width - 256 ||
        !c->visible_height || c->visible_height > SNES_PPU_MAX_BANDS ||
        !c->band_count || c->band_count > SNES_PPU_MAX_BANDS ||
        (c->oam == NULL) != (c->high_oam == NULL) ||
        (c->cgram_line_count && (!c->cgram_lines ||
         c->cgram_line_count != c->visible_height)))
        return SNESRECOMP_PPU_INVALID;
    unsigned end = 0;
    if(c->resolved_windows) for(unsigned i=0;i<c->band_count*6u;++i)
        if((c->resolved_windows[i].selector>15u && c->resolved_windows[i].selector!=255u) ||
            c->resolved_windows[i].logic>3u) return SNESRECOMP_PPU_INVALID;
    for (unsigned b = 0; b < c->band_count; ++b) {
        if (c->bands[b].y_begin != end || c->bands[b].y_end <= end ||
            c->bands[b].y_end > c->visible_height)
            return SNESRECOMP_PPU_INVALID;
        end = c->bands[b].y_end;
        for (unsigned layer = 0; layer < SNES_PPU_BG_COUNT; ++layer)
            if (!snesrecomp_ppu_elastic_valid(&c->bands[b].bg[layer]))
                return SNESRECOMP_PPU_INVALID;
    }
    if (end != c->visible_height) return SNESRECOMP_PPU_INVALID;
    if ((c->obj_pixels && c->obj_pitch < c->canvas_width) ||
        (!c->obj_pixels && c->obj_pitch)) return SNESRECOMP_PPU_INVALID;
    if (c->obj_pixels && previous->valid && !previous->storage && previous->capture.obj_pixels &&
        c->obj_pitch==c->canvas_width && previous->capture.canvas_width==c->canvas_width &&
        previous->capture.visible_height==c->visible_height)
        *same_objects=snesrecomp_ppu_words_equal(previous->obj_pixels,c->obj_pixels,
            (size_t)c->canvas_width*c->visible_height);
    if (c->obj_pixels && !*same_objects)
        for (unsigned y = 0; y < c->visible_height; ++y) {
            const uint16_t *row=c->obj_pixels+(size_t)y*c->obj_pitch;
            uint64_t occupied=0;
            unsigned x=0;
            for (;x+8u<=c->canvas_width;x+=8) {
                uint32_t pairs[4];
#if defined(__arm__) && (defined(__GNUC__) || defined(__clang__))
                if(!((uintptr_t)(row+x)&3u)) {
                    const void *aligned=__builtin_assume_aligned(row+x,4);
                    memcpy(pairs,aligned,sizeof pairs);
                } else
#endif
                    memcpy(pairs,row+x,sizeof pairs);
                /* Transparent capture groups are already canonical zeroes.
                 * They need neither palette qualification nor coverage work;
                 * nonzero transparent values still take the full validator. */
                const uint32_t occupied_pair=pairs[0]|pairs[1]|pairs[2]|pairs[3];
                if (!occupied_pair) continue;
                if (InvalidObjPair(pairs[0])|InvalidObjPair(pairs[1])|
                    InvalidObjPair(pairs[2])|InvalidObjPair(pairs[3]))
                    return SNESRECOMP_PPU_INVALID;
                if (occupied_pair&0x08000800u)
                    occupied|=UINT64_C(1)<<(x>>3);
            }
            for (;x<c->canvas_width;++x) {
                uint16_t p = row[x];
                if ((p & 0xf000u) || ((p & 0x800u) && !(p & 0x80u)))
                    return SNESRECOMP_PPU_INVALID;
                if(p&0x800u) occupied|=UINT64_C(1)<<(x>>3);
            }
            coverage[y]=occupied;
        }
    if ((c->raster_memory_flags & ~SNES_PPU_RASTER_MEMORY_CGRAM) ||
        ((c->raster_memory_flags & SNES_PPU_RASTER_MEMORY_CGRAM) &&
         c->cgram_line_count != c->visible_height))
        return SNESRECOMP_PPU_INCOMPLETE_MEMORY;
    if (c->resolved_bg_required & ~15u)
        return SNESRECOMP_PPU_INVALID;
    for (unsigned b = 0; b < SNES_PPU_BG_COUNT; ++b)
        if ((c->resolved_bg_required & (1u << b)) && !c->resolved_bg[b])
            return SNESRECOMP_PPU_INCOMPLETE_LAYOUT;
    if (obj) {
        if (obj->count > SNESRECOMP_OBJ_MAX_SLIVERS)
            return SNESRECOMP_PPU_RESOURCE_LIMIT;
        if ((obj->count && !obj->slivers) || obj->capacity < obj->count ||
            !obj->line_priority || obj->line_capacity < c->visible_height)
            return SNESRECOMP_PPU_INVALID;
        for (unsigned i = 0; i < obj->count; ++i) {
            const SnesRecompObjSliver *s = &obj->slivers[i];
            if (s->line >= c->visible_height || s->row > 7 ||
                s->priority > 3 || s->tile >= 2048 || s->flip_h > 1 ||
                s->math_eligible > 1 || s->oam_slot >= 128 ||
                (s->palette_base & 15) || s->palette_base < 128)
                return SNESRECOMP_PPU_INVALID;
        }
    }
    return SNESRECOMP_PPU_OK;
}

SnesRecompPpuResult snesrecomp_ppu_frame_freeze(
    SnesRecompPpuFrame *f, const SnesPpuFrameCapture *c,
    const SnesRecompObjFrame *obj, uint64_t sequence) {
    if (!f) return SNESRECOMP_PPU_INVALID;
    if (f->references != 1) return SNESRECOMP_PPU_BUSY;
    bool same_objects;
    uint64_t coverage[SNES_PPU_MAX_BANDS];
    const SnesRecompPpuResult result = Validate(c, obj, f, &same_objects, coverage);
    if (result != SNESRECOMP_PPU_OK) return result;
    const SnesPpuFrameCapture source=*c;
    c=&source; /* Source views may refer to this frame's previous snapshot. */
    /* OBJ qualification belongs to the authoritative capture producer. */
    for (unsigned i = 0; i < c->band_count; ++i)
        if (((c->bands[i].main_enable | c->bands[i].sub_enable) & 16u) &&
            !obj && !c->obj_pixels)
            return SNESRECOMP_PPU_INCOMPLETE_MEMORY;
    const bool same_vram=f->valid && !f->storage && snesrecomp_ppu_words_equal(f->vram,c->vram,SNES_PPU_VRAM_WORDS);
    const bool same_palette=f->valid && !f->storage && f->capture.cgram_line_count==c->cgram_line_count &&
        snesrecomp_ppu_words_equal(f->cgram,c->cgram,SNES_PPU_CGRAM_ENTRIES) &&
        (!c->cgram_line_count || snesrecomp_ppu_words_equal(f->palettes,c->cgram_lines,
            (size_t)c->cgram_line_count*SNES_PPU_CGRAM_ENTRIES));
    const bool same_plane=(same_objects || (f->valid && !f->capture.obj_pixels && !c->obj_pixels));
    if((!same_vram && f->versions.vram==UINT64_MAX) ||
        (!same_palette && f->versions.palette==UINT64_MAX) ||
        (!same_plane && f->versions.objects==UINT64_MAX)) return SNESRECOMP_PPU_RESOURCE_LIMIT;
    const SnesRecompPpuFrame *previous_storage=f->storage;
    f->capture = *c;
    if(!same_vram) { memcpy(f->vram, c->vram, sizeof(f->vram));++f->versions.vram; }
    if(!same_palette) { memcpy(f->cgram, c->cgram, sizeof(f->cgram));++f->versions.palette; }
    if(!same_plane) ++f->versions.objects;
    memcpy(f->bands, c->bands, c->band_count * sizeof(*c->bands));
    f->capture.vram = f->vram;
    f->capture.cgram = f->cgram;
    f->capture.bands = f->bands;
    if(c->resolved_windows) {
        memcpy(f->windows,c->resolved_windows,c->band_count*6u*sizeof(*c->resolved_windows));
        f->capture.resolved_windows=f->windows;
    }
    if (c->obj_pixels) {
        if(!same_objects) memcpy(f->obj_coverage,coverage,c->visible_height*sizeof(*coverage));
        for (unsigned y = 0; !same_objects && y < c->visible_height; ++y)
            memcpy(f->obj_pixels + y * c->canvas_width,
                   c->obj_pixels + y * c->obj_pitch,
                   c->canvas_width * sizeof(uint16_t));
        f->capture.obj_pixels = f->obj_pixels;
        f->capture.obj_pitch = c->canvas_width;
    }
    if (c->oam) {
        memcpy(f->oam, c->oam, sizeof(f->oam));
        memcpy(f->high_oam, c->high_oam, sizeof(f->high_oam));
        f->capture.oam = f->oam;
        f->capture.high_oam = f->high_oam;
    }
    f->capture.cgram_lines = NULL;
    if (c->cgram_line_count) {
        if(!same_palette) memcpy(f->palettes, c->cgram_lines,
               c->cgram_line_count * SNES_PPU_CGRAM_ENTRIES * sizeof(uint16_t));
        f->capture.cgram_lines = f->palettes;
    }
    for (unsigned b = 0; b < SNES_PPU_BG_COUNT; ++b)
        if (c->resolved_bg[b]) {
            memcpy(f->maps[b], c->resolved_bg[b], sizeof(f->maps[b]));
            f->capture.resolved_bg[b] = f->maps[b];
        }
    f->has_obj = obj != NULL;
    if (obj) {
        f->obj = *obj;
        if (obj->count) memcpy(f->slivers, obj->slivers,
                               obj->count * sizeof(*obj->slivers));
        memcpy(f->priorities, obj->line_priority, c->visible_height);
        f->obj.slivers = f->slivers;
        f->obj.line_priority = f->priorities;
        f->obj.capacity = SNESRECOMP_OBJ_MAX_SLIVERS;
        f->obj.line_capacity = SNES_PPU_MAX_BANDS;
    }
    f->sequence = sequence;
    f->valid = true;
    f->storage=NULL;f->storage_versions=(SnesRecompPpuFrameVersions){0};
    snesrecomp_ppu_frame_release(previous_storage);
    return SNESRECOMP_PPU_OK;
}

const SnesPpuFrameCapture *snesrecomp_ppu_frame_capture(const SnesRecompPpuFrame *f) {
    return f && f->valid ? &f->capture : NULL;
}
const SnesRecompObjFrame *snesrecomp_ppu_frame_obj(const SnesRecompPpuFrame *f) {
    if(f && f->valid && f->storage) return snesrecomp_ppu_frame_obj(f->storage);
    return f && f->valid && f->has_obj ? &f->obj : NULL;
}
const uint64_t *snesrecomp_ppu_frame_obj_coverage(const SnesRecompPpuFrame *f) {
    if(f && f->valid && f->storage) return snesrecomp_ppu_frame_obj_coverage(f->storage);
    return f && f->valid && f->capture.obj_pixels ? f->obj_coverage : NULL;
}
const SnesRecompPpuFrameVersions *snesrecomp_ppu_frame_versions(const SnesRecompPpuFrame *f) {
    return f && f->valid ? &f->versions : NULL;
}
bool snesrecomp_ppu_frame_same_memory(const SnesRecompPpuFrame *a,
    const SnesRecompPpuFrame *b,SnesRecompPpuFrameMemoryPair *cache) {
    if(!a || !b || !a->valid || !b->valid ||
       a->capture.canvas_width!=b->capture.canvas_width ||
       a->capture.visible_height!=b->capture.visible_height) return false;
    const unsigned width=a->capture.canvas_width,height=a->capture.visible_height;
    if(cache && cache->valid && cache->width==width && cache->height==height &&
       !memcmp(&cache->first,&a->versions,sizeof a->versions) &&
       !memcmp(&cache->second,&b->versions,sizeof b->versions)) return cache->equal;
    const bool equal=a==b || (a->storage?a->storage:a)==(b->storage?b->storage:b) ||
        (a->capture.cgram_line_count==b->capture.cgram_line_count &&
         (a->capture.obj_pixels!=NULL)==(b->capture.obj_pixels!=NULL) &&
         snesrecomp_ppu_words_equal(a->capture.vram,b->capture.vram,SNES_PPU_VRAM_WORDS) &&
         snesrecomp_ppu_words_equal(a->capture.cgram,b->capture.cgram,SNES_PPU_CGRAM_ENTRIES) &&
         (!a->capture.cgram_line_count || snesrecomp_ppu_words_equal(a->capture.cgram_lines,
             b->capture.cgram_lines,(size_t)a->capture.cgram_line_count*SNES_PPU_CGRAM_ENTRIES)) &&
         (!a->capture.obj_pixels || snesrecomp_ppu_words_equal(a->capture.obj_pixels,
             b->capture.obj_pixels,(size_t)width*height)));
    if(cache) *cache=(SnesRecompPpuFrameMemoryPair){a->versions,b->versions,
        (uint16_t)width,(uint16_t)height,true,equal};
    return equal;
}
uint64_t snesrecomp_ppu_frame_sequence(const SnesRecompPpuFrame *f) {
    return f && f->valid ? f->sequence : 0;
}
const char *snesrecomp_ppu_result_text(SnesRecompPpuResult r) {
    static const char *const names[] = {
        "ready", "invalid capture", "missing raster memory or qualified OBJ",
        "missing resolved layout", "unsupported effect", "resource limit",
        "frame busy", "backend failure"
    };
    return (unsigned)r < sizeof(names) / sizeof(*names) ? names[r] : "unknown";
}
