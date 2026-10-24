#include "snesrecomp_platform/snes_ppu_primitive_colour.h"
#include "snesrecomp_platform/snes_ppu_semantics.h"
#include "snesrecomp_platform/snes_ppu_semantic_gpu.h"
#include "snesrecomp_platform/snes_ppu_tile_plan.h"
#include <stdlib.h>
#include <string.h>
typedef struct PrimitiveCommandRange { uint32_t first,count; } PrimitiveCommandRange;
typedef struct PrimitiveRetainedCopy { uint32_t source,destination,count; } PrimitiveRetainedCopy;
struct SnesRecompPpuPrimitiveColourCache {
    SnesRecompPpuPrimitiveColour *commands[2];
    SnesRecompPpuPrimitive *primitives;
    SnesRecompPpuMaterial materials[SNESRECOMP_PPU_JOB_MAX_MATERIALS];
    PrimitiveCommandRange *ranges[2];
    uint32_t primitive_count;
    uint16_t material_count;
    uint16_t retained_palette[SNES_PPU_CGRAM_ENTRIES];
    uint16_t retained_palette_lines[SNES_PPU_MAX_BANDS][SNES_PPU_CGRAM_ENTRIES];
    uint32_t palette_dirty[SNES_PPU_CGRAM_ENTRIES][(SNES_PPU_MAX_BANDS+31u)/32u];
    bool has_palette_lines;
    SnesRecompWindowSpan windows[SNES_PPU_MAX_BANDS][SNESRECOMP_WINDOW_MAX_SPANS];
    uint8_t window_count[SNES_PPU_MAX_BANDS];
    SnesRecompWindowSpan retained_windows[SNES_PPU_MAX_BANDS][SNESRECOMP_WINDOW_MAX_SPANS];
    uint8_t retained_window_count[SNES_PPU_MAX_BANDS];
    uint16_t line_band[SNES_PPU_MAX_BANDS];
    uint32_t capacity,count;
    unsigned active;
    uint64_t generation;
    bool valid;
    SnesRecompPpuPrimitiveColourStats stats;
    uint64_t job_owner, geometry, graphics_owner, palette;
    uint16_t width, height, band_count;
    SnesPpuLayoutState layout;
    SnesPpuRasterBand bands[SNES_PPU_MAX_BANDS];
    SnesPpuResolvedWindow resolved_windows[SNES_PPU_MAX_BANDS*6u];
    bool has_resolved_windows;
};
static void FlushRetainedCopy(const SnesRecompPpuPrimitiveColourCache *p,
    SnesRecompPpuPrimitiveColour *commands,PrimitiveRetainedCopy *copy,uint32_t *calls) {
    if(!copy->count) return;
    memcpy(commands+copy->destination,p->commands[p->active]+copy->source,
        (size_t)copy->count*sizeof *commands);
    copy->count=0;++*calls;
}
SnesRecompPpuPrimitiveColourCache *snesrecomp_ppu_primitive_colour_create(uint32_t capacity) {
    if(!capacity || capacity>SNESRECOMP_PRIMITIVE_COLOUR_LIMIT) return NULL;
    SnesRecompPpuPrimitiveColourCache *p=calloc(1,sizeof *p);if(!p) return NULL;
    p->capacity=capacity;
    p->primitives=malloc(SNESRECOMP_PPU_JOB_MAX_PRIMITIVES*sizeof(*p->primitives));
    for(unsigned i=0;i<2;++i) {
        p->commands[i]=malloc((size_t)capacity*sizeof(*p->commands[i]));
        p->ranges[i]=malloc(SNESRECOMP_PPU_JOB_MAX_PRIMITIVES*sizeof(*p->ranges[i]));
    }
    if(!p->commands[0] || !p->commands[1] || !p->primitives || !p->ranges[0] || !p->ranges[1]) {
        snesrecomp_ppu_primitive_colour_destroy(p);return NULL;
    }
    return p;
}
void snesrecomp_ppu_primitive_colour_destroy(SnesRecompPpuPrimitiveColourCache *p) {
    if(p) {
        for(unsigned i=0;i<2;++i) {free(p->commands[i]);free(p->ranges[i]);}
        free(p->primitives);free(p);
    }
}
void snesrecomp_ppu_primitive_colour_stats(const SnesRecompPpuPrimitiveColourCache *p,
    SnesRecompPpuPrimitiveColourStats *stats) {
    if(stats) *stats=p?p->stats:(SnesRecompPpuPrimitiveColourStats){0};
}
uint32_t snesrecomp_ppu_primitive_colour_sample(const SnesRecompPpuPrimitiveColour *q,
    uint16_t sub,bool genuine,uint32_t base_argb) {
    if(!q) return 0;
    if(q->flags&SNESRECOMP_PRIMITIVE_COLOUR_BASE) return base_argb;
    const bool subscreen=(q->flags&SNESRECOMP_PRIMITIVE_COLOUR_SUBSCREEN)!=0;
    return snesrecomp_ppu_compose_colour(q->colour,subscreen && genuine?sub:q->fixed_colour,
        false,(q->flags&SNESRECOMP_PRIMITIVE_COLOUR_MATH)!=0,
        (q->flags&SNESRECOMP_PRIMITIVE_COLOUR_SUBTRACT)!=0,
        (q->flags&SNESRECOMP_PRIMITIVE_COLOUR_HALF) && (!subscreen || genuine),q->brightness);
}
static bool Same(const SnesRecompPpuPrimitiveColour *a,const SnesRecompPpuPrimitiveColour *b) {
    return a->main_enable==b->main_enable && a->colour==b->colour && a->fixed_colour==b->fixed_colour && a->flags==b->flags &&
        a->brightness==b->brightness && a->source_x==b->source_x && a->x==b->x &&
        a->width==b->width && a->priority==b->priority;
}
/* Geometry changes do not imply changes to source colour. Compare exactly
 * the fields the compiler below reads, and the resolved window decisions.
 * These certificates belong to the last SUCCESSFUL prepare: scratch windows
 * may have been overwritten by an intervening resource rejection. */
static bool ColourContextEqual(const SnesRecompPpuPrimitiveColourCache *p,
    const SnesPpuFrameCapture *c,const SnesRecompPpuFrameVersions *versions) {
    if(!p->valid || p->graphics_owner!=versions->owner ||
       p->width!=c->canvas_width || p->height!=c->visible_height || p->band_count!=c->band_count)
        return false;
    for(unsigned i=0;i<c->band_count;++i) {
        const SnesPpuRasterBand *a=p->bands+i,*b=c->bands+i;
        if(a->y_begin!=b->y_begin || a->y_end!=b->y_end || a->main_enable!=b->main_enable ||
           a->forced_blank!=b->forced_blank || a->brightness!=b->brightness ||
           a->cgwsel!=b->cgwsel || a->cgadsub!=b->cgadsub || a->fixed_colour!=b->fixed_colour ||
           p->retained_window_count[i]!=p->window_count[i]) return false;
        for(unsigned w=0;w<p->window_count[i];++w) {
            const SnesRecompWindowSpan *x=p->retained_windows[i]+w,*y=p->windows[i]+w;
            if(x->begin!=y->begin || x->end!=y->end || x->mask!=y->mask || x->obj_mask!=y->obj_mask)
                return false;
        }
    }
    return true;
}
/* A palette animation invalidates only primitives selecting that entry and
 * the affected source rows. Build the row bitsets once, instead of comparing
 * a full palette (or all rows) separately for thousands of primitives. */
static void PaletteChanges(SnesRecompPpuPrimitiveColourCache *p,const SnesPpuFrameCapture *c) {
    memset(p->palette_dirty,0,sizeof p->palette_dirty);
    for(unsigned y=0;y<c->visible_height;++y) {
        const uint16_t *old=p->has_palette_lines?p->retained_palette_lines[y]:p->retained_palette;
        const uint16_t *current=c->cgram_lines?c->cgram_lines+(size_t)y*256u:c->cgram;
        for(unsigned i=0;i<256u;++i)
            if((old[i]^current[i])&0x7fffu) p->palette_dirty[i][y/32u]|=UINT32_C(1)<<(y&31u);
    }
}
static bool PaletteRangeUnchanged(const SnesRecompPpuPrimitiveColourCache *p,
    const SnesRecompPpuPrimitive *r) {
    const uint32_t *dirty=p->palette_dirty[r->priority&255u];
    const unsigned end=r->source_y+r->height;
    for(unsigned y=r->source_y;y<end;) {
        unsigned n=32u-(y&31u);if(n>end-y) n=end-y;
        const uint32_t mask=(UINT32_MAX>>(32u-n))<<(y&31u);
        if(dirty[y/32u]&mask) return false;
        y+=n;
    }
    return true;
}
SnesRecompPpuResult snesrecomp_ppu_primitive_colour_prepare(
    SnesRecompPpuPrimitiveColourCache *p,const SnesRecompPpuJob *job,SnesRecompPpuPrimitiveColourPlan *out) {
    if(!out) return SNESRECOMP_PPU_INVALID;
    *out=(SnesRecompPpuPrimitiveColourPlan){0};
    const SnesRecompPpuJobView *v=snesrecomp_ppu_job_view(job);
    const SnesPpuFrameCapture *c=v?snesrecomp_ppu_frame_capture(v->graphics_frame):NULL;
    if(!p || !v || !c || !v->primitive_count) return SNESRECOMP_PPU_INVALID;
    ++p->stats.prepares;
    if(snesrecomp_ppu_mode1_raw_supports(c)!=SNES_PPU_SUPPORTED) return SNESRECOMP_PPU_UNSUPPORTED;
    const SnesRecompPpuFrameVersions *versions=snesrecomp_ppu_frame_versions(v->graphics_frame);
    /* Index changes are part of the owned geometry version. Colours depend
     * only on the owned palette certificate and raster/window state, never on
     * live VRAM/OBJ pointers. Do not retain a frame here: the consumer must be
     * able to freeze that owner again after submit/cancel. */
#ifndef SNESRECOMP_PRIVATE_PRIMITIVE_COLOUR_REFERENCE
    if(p->valid && p->job_owner==v->owner && p->geometry==v->primitive_geometry_version &&
       p->graphics_owner==versions->owner && p->palette==versions->palette &&
       p->width==c->canvas_width && p->height==c->visible_height && p->band_count==c->band_count &&
       p->has_resolved_windows==(c->resolved_windows!=NULL) &&
       !memcmp(&p->layout,&c->layout,sizeof p->layout) &&
       !memcmp(p->bands,c->bands,(size_t)c->band_count*sizeof(*c->bands)) &&
       (!c->resolved_windows || !memcmp(p->resolved_windows,c->resolved_windows,
            (size_t)c->band_count*6u*sizeof(*c->resolved_windows)))) {
        ++p->stats.cache_hits;
        *out=(SnesRecompPpuPrimitiveColourPlan){p->commands[p->active],p->count,p->generation,p->width,p->height};
        return SNESRECOMP_PPU_OK;
    }
#endif
    /* Validate the complete subset before producing any executable commands. */
    for(uint32_t i=0;i<v->primitive_count;++i) {
        const SnesRecompPpuPrimitive *r=v->primitives+i;
        const SnesRecompPpuMaterial *m=v->materials+r->material;
        if((r->kind!=SNESRECOMP_PPU_PRIMITIVE_BG && r->kind!=SNESRECOMP_PPU_PRIMITIVE_OBJ_CAPTURE) ||
            (m->flags!=SNESRECOMP_PPU_MATERIAL_SOURCE_COLOUR && m->flags!=SNESRECOMP_PPU_MATERIAL_BASE_COLOUR && m->flags!=SNESRECOMP_PPU_MATERIAL_PALETTE_COLOUR) || m->brightness_reduction ||
            !(r->priority&255u)) return SNESRECOMP_PPU_UNSUPPORTED;
        const unsigned layer=r->kind==SNESRECOMP_PPU_PRIMITIVE_BG?r->source:4u;
        if((m->flags==SNESRECOMP_PPU_MATERIAL_BASE_COLOUR && m->main_enable!=2u) ||
           (m->flags!=SNESRECOMP_PPU_MATERIAL_BASE_COLOUR && m->main_enable!=(2u|(1u<<layer))))
            return SNESRECOMP_PPU_UNSUPPORTED;
    }
    for(unsigned b=0;b<c->band_count;++b) {
        const unsigned count=snesrecomp_ppu_window_spans(c,c->bands+b,p->windows[b],SNESRECOMP_WINDOW_MAX_SPANS);
        if(!count) return SNESRECOMP_PPU_UNSUPPORTED;
        p->window_count[b]=(uint8_t)count;
        for(unsigned y=c->bands[b].y_begin;y<c->bands[b].y_end;++y) p->line_band[y]=(uint16_t)b;
    }
    uint32_t count=0;const unsigned next=p->active^1u;
    SnesRecompPpuPrimitiveColour *commands=p->commands[next];
    bool retain=false;
#ifndef SNESRECOMP_PRIVATE_PRIMITIVE_COLOUR_REFERENCE
    retain=p->job_owner==v->owner && ColourContextEqual(p,c,versions);
#endif
    const bool same_palette=retain && p->palette==versions->palette;
    if(retain && !same_palette) PaletteChanges(p,c);
    uint32_t generated=0,reused=0,copy_calls=0,copy_commands=0;
    PrimitiveRetainedCopy pending={0};
    for(uint32_t i=0;i<v->primitive_count;++i) {
        const SnesRecompPpuPrimitive *r=v->primitives+i;
        const SnesRecompPpuMaterial *material=v->materials+r->material;
        const uint32_t first=count;
        if(retain && i<p->primitive_count &&
           !memcmp(r,p->primitives+i,sizeof *r) &&
           !memcmp(material,p->materials+p->primitives[i].material,sizeof *material) &&
           (same_palette || material->flags==SNESRECOMP_PPU_MATERIAL_BASE_COLOUR || PaletteRangeUnchanged(p,r))) {
            const PrimitiveCommandRange old=p->ranges[p->active][i];
            if(old.count>p->capacity-count) return SNESRECOMP_PPU_RESOURCE_LIMIT;
            /* Consecutive retained ranges already have the original command
             * order. Copy one contiguous run, rather than calling a small
             * libc memcpy for every unchanged text/window fragment. Neither
             * arena is committed until the complete preparation succeeds. */
            if(old.count) {
                if(pending.count && (pending.source+pending.count!=old.first ||
                    pending.destination+pending.count!=count))
                    FlushRetainedCopy(p,commands,&pending,&copy_calls);
                if(!pending.count) pending=(PrimitiveRetainedCopy){old.first,count,0};
                pending.count+=old.count;copy_commands+=old.count;
            }
            count+=old.count;p->ranges[next][i]=(PrimitiveCommandRange){first,old.count};
            ++reused;continue;
        }
        FlushRetainedCopy(p,commands,&pending,&copy_calls);
        const unsigned layer=r->kind==SNESRECOMP_PPU_PRIMITIVE_BG?r->source:
            (((r->priority>>8)&15u)==4u?4u:6u);
        /* Merge equal palette rows only inside this primitive. Reordering
         * across another primitive would change equal-Z overlap winners. */
        for(unsigned y=0;y<r->height;) {
            const unsigned sy=r->source_y+y,b=p->line_band[sy];
            const SnesPpuRasterBand *band=c->bands+b;
            /* All material decisions are constant within a raster section.
             * A palette history only splits this primitive where its selected
             * colour actually changes. Other palette entries have no input. */
            unsigned run=band->y_end-sy;
            if(run>r->height-y) run=r->height-y;
            if(band->forced_blank) {y+=run;continue;}
            const uint16_t *palette=c->cgram_lines?c->cgram_lines+(size_t)sy*256u:c->cgram;
            if(c->cgram_lines) {
                const unsigned index=r->priority&255u;
                const uint16_t colour=palette[index]&0x7fffu;
                for(unsigned n=1;n<run;++n)
                    if((c->cgram_lines[(size_t)(sy+n)*256u+index]&0x7fffu)!=colour) {run=n;break;}
            }
            const bool palette_only=material->flags==SNESRECOMP_PPU_MATERIAL_PALETTE_COLOUR;
            const SnesRecompWindowSpan palette_span={.begin=r->source_x,.end=r->source_x+r->width,
                .mask=SNESRECOMP_SEMANTIC_MAIN_RGB|7u,.obj_mask=1u};
            for(unsigned w=0;w<(palette_only?1u:p->window_count[b]);++w) {
                const SnesRecompWindowSpan *span=palette_only?&palette_span:p->windows[b]+w;
                const unsigned begin=span->begin>r->source_x?span->begin:r->source_x;
                const unsigned end=span->end<r->source_x+r->width?span->end:r->source_x+r->width;
                if(begin>=end) continue;
                SnesRecompPpuPrimitiveColour q={.source_x=(uint16_t)begin,.source_y=(uint16_t)sy,
                    .x=(uint16_t)(r->x+begin-r->source_x),.y=(uint16_t)(r->y+y),
                    .width=(uint16_t)(end-begin),.height=(uint16_t)run,.priority=r->priority,
                    .colour=(uint16_t)((span->mask&SNESRECOMP_SEMANTIC_MAIN_RGB)?palette[r->priority&255u]&0x7fffu:0),
                    .fixed_colour=(uint16_t)(band->fixed_colour&0x7fffu),.brightness=band->brightness,
                    .main_enable=material->main_enable};
                if((span->mask&SNESRECOMP_SEMANTIC_MATH) && layer<6 && (band->cgadsub&(1u<<layer)))
                    q.flags|=SNESRECOMP_PRIMITIVE_COLOUR_MATH;
                if(band->cgwsel&2u) q.flags|=SNESRECOMP_PRIMITIVE_COLOUR_SUBSCREEN;
                if(band->cgadsub&128u) q.flags|=SNESRECOMP_PRIMITIVE_COLOUR_SUBTRACT;
                if(band->cgadsub&64u) q.flags|=SNESRECOMP_PRIMITIVE_COLOUR_HALF;
                const bool visible=palette_only || ((band->main_enable&(1u<<(layer==6u?4u:layer))) &&
                    (layer<3?(span->mask&(1u<<layer))!=0:(span->obj_mask&1u)!=0));
                if(material->flags==SNESRECOMP_PPU_MATERIAL_BASE_COLOUR || !visible) {
                    q.flags=SNESRECOMP_PRIMITIVE_COLOUR_BASE;q.colour=q.fixed_colour=0;
                    if(!visible) q.main_enable&=2u;
                }
                if(palette_only) {q.flags=0;q.fixed_colour=0;}
                bool merged=false;
                for(uint32_t k=count;k>first && !merged;--k) {
                    SnesRecompPpuPrimitiveColour *old=commands+k-1;
                    if(old->y+old->height<q.y) break;
                    if(old->y+old->height==q.y && old->source_y+old->height==q.source_y && Same(old,&q)) {
                        old->height=(uint16_t)(old->height+run);merged=true;
                    }
                }
                if(merged) continue;
                if(count==p->capacity) return SNESRECOMP_PPU_RESOURCE_LIMIT;
                commands[count++]=q;
            }
            y+=run;
        }
        p->ranges[next][i]=(PrimitiveCommandRange){first,count-first};
        generated+=count-first;
    }
    FlushRetainedCopy(p,commands,&pending,&copy_calls);
    const bool same=p->valid && count==p->count && (!count || !memcmp(commands,p->commands[p->active],(size_t)count*sizeof *commands));
    if(!same && p->generation==UINT64_MAX) return SNESRECOMP_PPU_RESOURCE_LIMIT;
    /* Commit ranges with their command arena even if bytes did not change.
     * A different primitive partition may produce an identical command list. */
    p->active=next;
    if(!same) ++p->generation;
    p->count=count;p->valid=true;
    if(p->graphics_owner!=versions->owner || p->palette!=versions->palette || p->height!=c->visible_height) {
        memcpy(p->retained_palette,c->cgram,sizeof p->retained_palette);
        if(c->cgram_lines) memcpy(p->retained_palette_lines,c->cgram_lines,
            (size_t)c->visible_height*256u*sizeof(uint16_t));
        p->has_palette_lines=c->cgram_lines!=NULL;
    }
    p->job_owner=v->owner;p->geometry=v->primitive_geometry_version;
    p->graphics_owner=versions->owner;p->palette=versions->palette;
    p->width=c->canvas_width;p->height=c->visible_height;p->band_count=c->band_count;
    p->layout=c->layout;p->has_resolved_windows=c->resolved_windows!=NULL;
    p->primitive_count=v->primitive_count;p->material_count=v->material_count;
    memcpy(p->primitives,v->primitives,(size_t)v->primitive_count*sizeof(*v->primitives));
    memcpy(p->materials,v->materials,(size_t)v->material_count*sizeof(*v->materials));
    memcpy(p->bands,c->bands,(size_t)c->band_count*sizeof(*c->bands));
    for(unsigned i=0;i<c->band_count;++i) {
        p->retained_window_count[i]=p->window_count[i];
        memcpy(p->retained_windows[i],p->windows[i],(size_t)p->window_count[i]*sizeof(p->windows[i][0]));
    }
    if(c->resolved_windows) memcpy(p->resolved_windows,c->resolved_windows,
        (size_t)c->band_count*6u*sizeof(*c->resolved_windows));
    p->stats.generated_commands+=generated;p->stats.reused_primitives+=reused;
    p->stats.retained_copy_calls+=copy_calls;
    p->stats.retained_copy_bytes+=(uint64_t)copy_commands*sizeof *commands;
    *out=(SnesRecompPpuPrimitiveColourPlan){p->commands[p->active],count,p->generation,c->canvas_width,c->visible_height};
    return SNESRECOMP_PPU_OK;
}
