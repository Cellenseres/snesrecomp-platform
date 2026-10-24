#include "snesrecomp_platform/snes_ppu_mode7_world.h"
#include "snesrecomp_platform/snes_ppu_semantics.h"
#include "snesrecomp_platform/snes_ppu_semantic_gpu.h"
#include "snes_ppu_memory.h"
#include "snesrecomp_platform/snes_ppu_mode7_cells.h"
#include <stdlib.h>
#include <string.h>

/* Pure fractional-coordinate certificate; independent of tiles and palette. */
typedef struct Mode7PhaseEntry {
    uint32_t key;
    uint16_t count;
    uint8_t min_x,max_x,min_y,max_y;
    bool valid;
} Mode7PhaseEntry;

struct SnesRecompMode7WorldCache {
    uint32_t pixels[2][SNESRECOMP_MODE7_WORLD_SIDE * SNESRECOMP_MODE7_WORLD_SIDE];
    uint64_t versions[2][SNESRECOMP_MODE7_WORLD_CELLS], generation;
    uint64_t resource_generation[2], geometry_generation;
    uint32_t characters[2][256][64];
    uint16_t previous_vram[SNESRECOMP_MODE7_WORLD_CELLS];
    uint32_t colours[2][256];
    uint16_t previous_palette[256];
    uint32_t previous_policy;
    SnesRecompPpuFrameVersions previous_versions;
    uint8_t map[SNESRECOMP_MODE7_WORLD_CELLS];
    SnesRecompMode7Line lines[SNES_PPU_MAX_BANDS];
    SnesRecompMode7WorldSpan spans[32768];
    SnesPpuRasterBand previous_bands[SNES_PPU_MAX_BANDS];
    SnesPpuLayoutState previous_layout;
    unsigned previous_band_count, previous_width, previous_height, previous_extra, span_count;
    bool spans_valid;
    bool valid;
    bool shared_planes;
    uint32_t pending[2][512], resident[2][512], pending_count[2];
    SnesRecompMode7Cells visible[2];
    uint64_t visible_generation, resident_epoch, prepared_epoch, prepared_geometry;
    bool visible_raw;
    bool uniform_palette_certified;
    uint16_t certified_palette_height;
    Mode7PhaseEntry phases[4096];
};

static SnesRecompPpuResult CompileWorldSpans(const SnesRecompPpuFrame *,
    const SnesRecompMode7WorldPlan *,SnesRecompMode7WorldSpan *,unsigned,unsigned *,Mode7PhaseEntry *);

SnesRecompMode7WorldCache *snesrecomp_mode7_world_create(void) {
    return calloc(1, sizeof(SnesRecompMode7WorldCache));
}
void snesrecomp_mode7_world_destroy(SnesRecompMode7WorldCache *cache) { free(cache); }
void snesrecomp_mode7_world_reset(SnesRecompMode7WorldCache *cache) {
    if (cache) cache->valid = cache->spans_valid = false;
}

static uint32_t PaletteColour(uint16_t colour, const SnesPpuRasterBand *b, unsigned math, bool raw) {
    if(raw) return 0xff000000u | ((uint32_t)colour&31u)<<19 |
        (((uint32_t)colour>>5)&31u)<<11 | (((uint32_t)colour>>10)&31u)<<3;
    return snesrecomp_ppu_compose_colour(colour, b->fixed_colour, false,
        math && (b->cgadsub & 1u), (b->cgadsub & 128u) != 0,
        (b->cgadsub & 64u) && !(b->cgwsel & 2u), b->brightness);
}
static uint32_t PalettePolicy(const SnesPpuRasterBand *b, bool raw) {
    if(raw) return UINT32_C(0x80000000);
    return b->brightness|((uint32_t)(b->fixed_colour&32767u)<<4)|
        ((uint32_t)(b->cgadsub&0xc1u)<<19)|((uint32_t)(b->cgwsel&2u)<<26);
}

static unsigned FirstPendingBit(uint32_t bits) {
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_ctz(bits);
#else
    unsigned bit=0;
    if(!(bits&0xffffu)) {bit+=16;bits>>=16;}
    if(!(bits&0xffu)) {bit+=8;bits>>=8;}
    if(!(bits&0xfu)) {bit+=4;bits>>=4;}
    if(!(bits&3u)) {bit+=2;bits>>=2;}
    if(!(bits&1u)) ++bit;
    return bit;
#endif
}

static SnesRecompPpuResult PrepareWorld(SnesRecompMode7WorldCache *cache,
    const SnesRecompPpuFrame *frame, SnesRecompMode7WorldPlan *out, bool raw, SnesRecompMode7WorldGeometry *view) {
    if (!cache || !out) return SNESRECOMP_PPU_INVALID;
    memset(out, 0, sizeof(*out));
    const SnesPpuFrameCapture *cap = snesrecomp_ppu_frame_capture(frame);
    if(!cap) return SNESRECOMP_PPU_INVALID;
    const SnesRecompPpuFrameVersions *versions=snesrecomp_ppu_frame_versions(frame);
    out->unsupported = snesrecomp_ppu_mode7_supports(cap);
    if (out->unsupported) return SNESRECOMP_PPU_UNSUPPORTED;
    const SnesPpuRasterBand *source = NULL;
    const uint16_t *palette = NULL;
    uint32_t colours[2][256];
    /* Captured palette versions certify the entire immutable history. Once
     * all rows were checked under one policy, camera/VRAM changes cannot
     * require scanning that unchanged history again. Newly enabled rows,
     * differing policies, owners or palettes must take the original check. */
    bool uniform_policy=cap->band_count!=0;
    const uint32_t policy=cap->band_count?PalettePolicy(cap->bands,raw):0;
    for(unsigned bi=0;bi<cap->band_count;++bi) {
        const SnesPpuRasterBand *b=cap->bands+bi;
        if(b->forced_blank || !(b->main_enable&1u) || PalettePolicy(b,raw)!=policy)uniform_policy=false;
    }
    const bool certified=uniform_policy && cache->valid && cache->uniform_palette_certified &&
        cache->certified_palette_height==cap->visible_height && cache->previous_policy==policy &&
        cache->previous_versions.owner==versions->owner && cache->previous_versions.palette==versions->palette;
    /* One world texture cannot silently use the final raster palette. */
    for (unsigned bi = 0; bi < cap->band_count; ++bi) {
        const SnesPpuRasterBand *b = &cap->bands[bi];
        if (b->forced_blank || !(b->main_enable & 1u)) continue;
        const uint16_t *row = cap->cgram_lines ? cap->cgram_lines + b->y_begin * 256 : cap->cgram;
        if (!source) {
            source = b; palette = row;
            if(certified || (cache->valid && cache->previous_policy==PalettePolicy(b,raw) &&
                snesrecomp_ppu_words_equal(cache->previous_palette+1,row+1,255)))
                memcpy(colours,cache->colours,sizeof(colours));
            else {
                colours[0][0] = colours[1][0] = 0;
                for (unsigned i = 1; i < 256; ++i)
                    for (unsigned variant = 0; variant < 2; ++variant)
                        colours[variant][i] = PaletteColour(palette[i], b, variant,raw);
            }
        }
        for (unsigned y = b->y_begin; !certified && y < b->y_end; ++y) {
            row = cap->cgram_lines ? cap->cgram_lines + y * 256 : cap->cgram;
            if ((raw || (b->brightness == source->brightness && b->fixed_colour == source->fixed_colour &&
                !((b->cgadsub ^ source->cgadsub) & 0xc1u) &&
                !((b->cgwsel ^ source->cgwsel) & 2u))) &&
                snesrecomp_ppu_words_equal(row+1,palette+1,255)) {
                if (!cap->cgram_lines) break;
                continue;
            }
            for (unsigned i = 1; i < 256; ++i)
                for (unsigned variant = 0; variant < 2; ++variant)
                    if (colours[variant][i] != PaletteColour(row[i], b, variant,raw)) {
                        out->unsupported = SNES_PPU_UNSUPPORTED_RASTER_STATE;
                        return SNESRECOMP_PPU_UNSUPPORTED;
                    }
            if (!cap->cgram_lines) break;
        }
    }
    if (!snesrecomp_ppu_mode7_compile_lines(cap, cache->lines, SNES_PPU_MAX_BANDS))
        return SNESRECOMP_PPU_INVALID;
    if(view) {
        out->lines=cache->lines;out->pixels[0]=cache->pixels[0];
        const SnesRecompPpuResult result=snesrecomp_mode7_world_cached_spans(cache,frame,out,view);
        if(result!=SNESRECOMP_PPU_OK) return result;
        if(cache->visible_generation!=view->generation || cache->visible_raw!=raw) {
            memset(cache->visible,0,sizeof cache->visible);
            for(unsigned i=0;i<view->count;++i) {
                const SnesRecompMode7WorldSpan *span=view->spans+i;
                snesrecomp_mode7_cells_add(cache->visible+(raw?0u:span->variant),span);
            }
            cache->visible_generation=view->generation;cache->visible_raw=raw;
        }
    }
    /* Scroll and affine changes do not invalidate character/world resources.
     * Upper VRAM is unrelated to the native Mode 7 map and character plane. */
    const bool shared_planes=source ? !memcmp(colours[0],colours[1],sizeof colours[0]) :
        cache->shared_planes;
    /* Identical immutable planes may share storage. If colour math later
     * separates them, restore plane one even for logically unchanged cells:
     * its private storage was not written while the views were shared. */
    const bool restore_plane_one=cache->shared_planes && !shared_planes;
    if (source && (restore_plane_one || !(cache->valid &&
        ((cache->previous_versions.owner==versions->owner &&
          cache->previous_versions.vram==versions->vram) ||
         snesrecomp_ppu_words_equal(cache->previous_vram,cap->vram,SNESRECOMP_MODE7_WORLD_CELLS)) &&
        !memcmp(cache->colours, colours, sizeof(colours))))) {
        ++cache->resident_epoch;
        bool changed[2][256] = {{false}};
        /* Rebuild only characters whose actual colour pixels changed. */
        for (unsigned character = 0; character < 256; ++character) {
            for (unsigned variant = 0; variant < 2; ++variant) {
                uint32_t decoded[64];
                for (unsigned p = 0; p < 64; ++p)
                    decoded[p] = colours[variant][cap->vram[character * 64 + p] >> 8];
                changed[variant][character] = !cache->valid ||
                    memcmp(cache->characters[variant][character], decoded, sizeof(decoded));
                if (changed[variant][character])
                    memcpy(cache->characters[variant][character], decoded, sizeof(decoded));
            }
        }
        for (unsigned cell = 0; cell < SNESRECOMP_MODE7_WORLD_CELLS; ++cell) {
            const unsigned character = cap->vram[cell] & 255u;
            for (unsigned variant = 0; variant < 2; ++variant) {
                const bool cell_changed=!cache->valid || cache->map[cell]!=character || changed[variant][character];
                if(!cell_changed && !(variant==1 && restore_plane_one)) continue;
                const uint32_t bit=UINT32_C(1)<<(cell&31u);
                if(!(cache->pending[variant][cell>>5]&bit)) ++cache->pending_count[variant];
                cache->pending[variant][cell>>5]|=bit;
                cache->resident[variant][cell>>5]&=~bit;
                if(cell_changed) {
                    cache->versions[variant][cell] = ++cache->generation;
                    cache->resource_generation[variant] = cache->generation;
                    ++out->changed_cells[variant];
                }
            }
            cache->map[cell] = (uint8_t)character;
        }
        cache->valid = true;
        memcpy(cache->previous_vram, cap->vram, sizeof(cache->previous_vram));
        memcpy(cache->colours, colours, sizeof(colours));
    }
    if(source) {
        cache->shared_planes=shared_planes;
        memcpy(cache->previous_palette,palette,sizeof(cache->previous_palette));
        cache->previous_policy=PalettePolicy(source,raw);
        cache->previous_versions=*versions;
        cache->uniform_palette_certified=uniform_policy;
        cache->certified_palette_height=cap->visible_height;
    }
    /* Character/map invalidation stays independent of camera movement.
     * Materialize only requested dirty cells; pending invisible work survives
     * palette changes, scrolling, cancellation and shared-plane transitions. */
    const bool reuse_view=view && cache->prepared_geometry==view->generation &&
        cache->prepared_epoch==cache->resident_epoch;
    for(unsigned variant=0;!reuse_view && variant<2;++variant) {
        if(!cache->pending_count[variant] || (variant && cache->shared_planes)) continue;
        for(unsigned word=0;word<512;++word) {
            const uint32_t requested=!view?UINT32_MAX:cache->visible[variant].words[word] |
                (!variant && cache->shared_planes?cache->visible[1].words[word]:0u);
            uint32_t todo=cache->pending[variant][word]&requested;
            while(todo) {
                const unsigned bit=FirstPendingBit(todo),cell=word*32u+bit;
                const unsigned x=(cell&127u)*8u,y=(cell>>7)*8u;
                for(unsigned row=0;row<8;++row)
                    memcpy(cache->pixels[variant]+(y+row)*SNESRECOMP_MODE7_WORLD_SIDE+x,
                        cache->characters[variant][cache->map[cell]]+row*8,8*sizeof(uint32_t));
                const uint32_t mask=UINT32_C(1)<<bit;
                cache->pending[variant][word]&=~mask;cache->resident[variant][word]|=mask;
                --cache->pending_count[variant];
                todo&=todo-1u;++out->materialized_cells[variant];
            }
        }
    }
    if(view) {cache->prepared_geometry=view->generation;cache->prepared_epoch=cache->resident_epoch;}
    for (unsigned variant = 0; variant < 2; ++variant) {
        out->pixels[variant] = cache->pixels[variant && cache->shared_planes ? 0 : variant];
        out->versions[variant] = cache->versions[variant];
        out->resident_cells[variant]=view?cache->resident[variant && cache->shared_planes?0:variant]:NULL;
        out->resource_generation[variant] = cache->resource_generation[variant];
    }
    if(view) {
        out->sampled_cells[0]=cache->visible[0].words;
        out->sampled_cells[1]=cache->visible[1].words;
        out->sampled_geometry_generation=view->generation;
    }
    out->lines = cache->lines;
    out->raw_rgb5=raw;
    return SNESRECOMP_PPU_OK;
}

SnesRecompPpuResult snesrecomp_mode7_world_prepare(SnesRecompMode7WorldCache *cache,
    const SnesRecompPpuFrame *frame,SnesRecompMode7WorldPlan *out) {
    return PrepareWorld(cache,frame,out,false,NULL);
}
SnesRecompPpuResult snesrecomp_mode7_world_prepare_raw(SnesRecompMode7WorldCache *cache,
    const SnesRecompPpuFrame *frame,SnesRecompMode7WorldPlan *out) {
    return PrepareWorld(cache,frame,out,true,NULL);
}

SnesRecompPpuResult snesrecomp_mode7_world_prepare_view(SnesRecompMode7WorldCache *cache,
    const SnesRecompPpuFrame *frame,bool raw,SnesRecompMode7WorldPlan *out,
    SnesRecompMode7WorldGeometry *geometry) {
    if(!geometry) return SNESRECOMP_PPU_INVALID;
    memset(geometry,0,sizeof *geometry);
    return PrepareWorld(cache,frame,out,raw,geometry);
}

SnesRecompPpuResult snesrecomp_mode7_world_cached_spans(SnesRecompMode7WorldCache *cache,
    const SnesRecompPpuFrame *frame, const SnesRecompMode7WorldPlan *world,
    SnesRecompMode7WorldGeometry *out) {
    if (!out) return SNESRECOMP_PPU_INVALID;
    memset(out, 0, sizeof(*out));
    const SnesPpuFrameCapture *cap = snesrecomp_ppu_frame_capture(frame);
    if (!cache || !cap || !world || world->lines != cache->lines ||
        world->pixels[0] != cache->pixels[0]) return SNESRECOMP_PPU_INVALID;
    if (!(cache->spans_valid && cache->previous_band_count == cap->band_count &&
        cache->previous_width == cap->canvas_width && cache->previous_height == cap->visible_height &&
        cache->previous_extra == cap->canvas_extra &&
        !memcmp(&cache->previous_layout, &cap->layout, sizeof(cap->layout)) &&
        !memcmp(cache->previous_bands, cap->bands, cap->band_count * sizeof(*cap->bands)))) {
        cache->spans_valid = false;
        SnesRecompPpuResult result = CompileWorldSpans(frame,world,cache->spans,
            32768,&cache->span_count,cache->phases);
        if (result != SNESRECOMP_PPU_OK) return result;
        cache->previous_band_count = cap->band_count;
        cache->previous_width = cap->canvas_width; cache->previous_height = cap->visible_height;
        cache->previous_extra = cap->canvas_extra; cache->previous_layout = cap->layout;
        memcpy(cache->previous_bands, cap->bands, cap->band_count * sizeof(*cap->bands));
        cache->spans_valid = true; ++cache->geometry_generation;
    }
    *out = (SnesRecompMode7WorldGeometry){cache->spans,cache->span_count,cache->geometry_generation};
    return SNESRECOMP_PPU_OK;
}

/* With steps divisible by 32, fractional coordinates repeat within eight
 * pixels and their range cannot exceed 224. Inspect one period, then bound
 * the remaining run analytically at the same world-wrap boundary. */
static unsigned Mode7RunLimit(uint32_t start, int32_t step, unsigned limit) {
    unsigned count=limit;
    if(step>0) count=(0x3ffffu-start)/(uint32_t)step+1u;
    else if(step<0) count=start/(0u-(uint32_t)step)+1u;
    return count<limit?count:limit;
}

SnesRecompPpuResult snesrecomp_mode7_world_spans(const SnesRecompPpuFrame *frame,
    const SnesRecompMode7WorldPlan *world,SnesRecompMode7WorldSpan *spans,
    unsigned capacity,unsigned *count) {
    return CompileWorldSpans(frame,world,spans,capacity,count,NULL);
}

static SnesRecompPpuResult CompileWorldSpans(const SnesRecompPpuFrame *frame,
    const SnesRecompMode7WorldPlan *world,SnesRecompMode7WorldSpan *spans,
    unsigned capacity,unsigned *count,Mode7PhaseEntry *phases) {
    if (!count) return SNESRECOMP_PPU_INVALID;
    *count = 0;
    const SnesPpuFrameCapture *cap = snesrecomp_ppu_frame_capture(frame);
    if (!cap || !world || !world->lines || !spans) return SNESRECOMP_PPU_INVALID;
    unsigned used = 0;
    for (unsigned bi = 0; bi < cap->band_count; ++bi) {
        const SnesPpuRasterBand *b = &cap->bands[bi];
        if (b->forced_blank || !(b->main_enable & 1u)) continue;
        SnesRecompWindowSpan windows[SNESRECOMP_WINDOW_MAX_SPANS];
        unsigned nw = snesrecomp_ppu_window_spans(cap, b, windows, SNESRECOMP_WINDOW_MAX_SPANS);
        if (!nw) return SNESRECOMP_PPU_INVALID;
        uint32_t span_colours[SNESRECOMP_WINDOW_MAX_SPANS];
        for(unsigned wi=0;wi<nw;++wi) {
            const bool math=(windows[wi].mask&SNESRECOMP_SEMANTIC_MATH) && (b->cgadsub&1u);
            span_colours[wi]=snesrecomp_ppu_compose_colour(0,b->fixed_colour,true,math,
                (b->cgadsub&128u)!=0,(b->cgadsub&64u)&&!(b->cgwsel&2u),b->brightness);
        }
        for (unsigned y = b->y_begin; y < b->y_end; ++y) {
            const SnesRecompMode7Line *line = &world->lines[y];
            for (unsigned wi = 0; wi < nw; ++wi) {
                const SnesRecompWindowSpan *window = &windows[wi];
                if (!(window->mask & 1u)) continue;
                int left = (int)cap->canvas_extra - b->bg[0].margin_left;
                int right = (int)cap->canvas_extra + 256 + b->bg[0].margin_right;
                if (left < window->begin) left = window->begin;
                if (right > window->end) right = window->end;
                for (int x = left; x < right;) {
                    const int screen = x - cap->canvas_extra;
                    /* Only the low 18 bits survive world wrap. Unsigned
                     * arithmetic preserves those bits even for negative X
                     * and avoids repeated wide multiplies on ARM. */
                    const int32_t sx = (int32_t)((line->start_x +
                        (uint32_t)line->step_x * (uint32_t)screen) & 0x3ffffu);
                    const int32_t sy = (int32_t)((line->start_y +
                        (uint32_t)line->step_y * (uint32_t)screen) & 0x3ffffu);
                    int min_x=255,max_x=0,min_y=255,max_y=0;
                    unsigned n=0;
                    /* Each admitted position is only 18 bits. One signed
                     * 32-bit step cannot wrap into that range accidentally;
                     * unsigned underflow is rejected by the same bound. */
                    uint32_t px=(uint32_t)sx,py=(uint32_t)sy;
                    const unsigned steps=((uint32_t)line->step_x|(uint32_t)line->step_y)&255u;
                    unsigned limit=(unsigned)(right-x),inspect=limit;
                    if(!(steps&31u)) {
                        limit=Mode7RunLimit(px,line->step_x,limit);
                        limit=Mode7RunLimit(py,line->step_y,limit);
                        const unsigned period=steps&32u?8u:steps&64u?4u:steps&128u?2u:1u;
                        inspect=limit<period?limit:period;
                    }
                    Mode7PhaseEntry *phase=NULL;
                    uint32_t phase_key=0;bool phase_hit=false;
                    if(phases && (steps&31u)) {
                        phase_key=((uint32_t)line->step_x&255u)|
                            (((uint32_t)line->step_y&255u)<<8)|((px&255u)<<16)|((py&255u)<<24);
                        phase=phases+(((phase_key^(phase_key>>16))*UINT32_C(2654435761))>>20);
                        if(phase->valid && phase->key==phase_key && phase->count<=inspect) {
                            const int64_t last_x=(int64_t)sx+(int64_t)line->step_x*(phase->count-1u);
                            const int64_t last_y=(int64_t)sy+(int64_t)line->step_y*(phase->count-1u);
                            if(last_x>=0 && last_x<=0x3ffff && last_y>=0 && last_y<=0x3ffff) {
                                n=phase->count;min_x=phase->min_x;max_x=phase->max_x;
                                min_y=phase->min_y;max_y=phase->max_y;phase_hit=true;
                            }
                        }
                    }
                    while (!phase_hit && n < inspect) {
                        if (px>0x3ffffu || py>0x3ffffu) break;
                        const int fx=(int)px&255,fy=(int)py&255;
                        const int lo_x=fx<min_x?fx:min_x, hi_x=fx>max_x?fx:max_x;
                        const int lo_y=fy<min_y?fy:min_y, hi_y=fy>max_y?fy:max_y;
                        if (hi_x-lo_x>224 || hi_y-lo_y>224) break;
                        min_x=lo_x;max_x=hi_x;min_y=lo_y;max_y=hi_y;++n;
                        px+=(uint32_t)line->step_x;py+=(uint32_t)line->step_y;
                    }
                    if(phase && !phase_hit && n && n<inspect && px<=0x3ffffu && py<=0x3ffffu) {
                        /* Only a fractional-range cut is reusable. Window and
                         * world-wrap cuts must never shorten another view. */
                        phase->key=phase_key;phase->count=(uint16_t)n;
                        phase->min_x=(uint8_t)min_x;phase->max_x=(uint8_t)max_x;
                        phase->min_y=(uint8_t)min_y;phase->max_y=(uint8_t)max_y;phase->valid=true;
                    }
                    if(!(steps&31u)) n=limit;
                    if (!n) return SNESRECOMP_PPU_INVALID;
                    if (used==capacity) return SNESRECOMP_PPU_RESOURCE_LIMIT;
                    const bool math=(window->mask & SNESRECOMP_SEMANTIC_MATH) && (b->cgadsub & 1u);
                    const bool clip=!(window->mask & SNESRECOMP_SEMANTIC_MAIN_RGB);
                    spans[used++]=(SnesRecompMode7WorldSpan){(uint16_t)x,(uint16_t)(x+n),(uint16_t)y,
                        (uint8_t)math,(uint8_t)clip,
                        sx+(256-max_x-min_x)/2,sy+(256-max_y-min_y)/2,line->step_x,line->step_y,
                        span_colours[wi]};
                    x+=(int)n;
                }
            }
        }
    }
    *count=used;
    return SNESRECOMP_PPU_OK;
}
