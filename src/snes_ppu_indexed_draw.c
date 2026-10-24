#include "snesrecomp_platform/snes_ppu_indexed_draw.h"
#include "snesrecomp_platform/snes_ppu_semantics.h"
#include <stdlib.h>
#include <string.h>
enum { CELLS=64*32, SOURCES=SNESRECOMP_TILE_PLAN_QUADS+SNESRECOMP_OBJ_MAX_SLIVERS,
       LINKS=SOURCES*8 };
/* TMW alone does not activate a window. The source-colour shortcut must
 * reject an effective selector, including a consumer-resolved override, rather
 * than treating dormant enable masks as colour processing. */
static bool IndexedMaterialWindow(const SnesPpuFrameCapture *cap,unsigned band,unsigned layer) {
    const SnesPpuRasterBand *b=cap->bands+band;
    if(!(b->main_window_enable&(1u<<layer)))return false;
    unsigned selector=(b->window_sel>>(layer*4u))&15u;
    if(cap->resolved_windows && cap->resolved_windows[band*6u+layer].selector!=255u)
        selector=cap->resolved_windows[band*6u+layer].selector;
    return (selector&10u)!=0;
}
uint32_t snesrecomp_ppu_indexed_palette_equivalent(const SnesRecompPpuJob *job) {
    const SnesRecompPpuJobView *v=snesrecomp_ppu_job_view(job);
    if(!v || !v->indexed_layout)return 0;
    const SnesRecompPpuIndexedLayout *l=v->indexed_layout;
    const SnesPpuFrameCapture *cap=snesrecomp_ppu_frame_capture(v->graphics_frame);
    if(!cap || !cap->bands || !l->materials || !l->band_materials || l->band_count!=cap->band_count)return 0;
    uint32_t referenced=0,rejected=0;
    for(unsigned band=0;band<l->band_count;++band)for(unsigned plane=0;plane<l->plane_count;++plane) {
        const unsigned material=l->band_materials[band*l->plane_count+plane];
        if(material>=l->material_count || material>=32)continue;
        const SnesRecompPpuMaterial *m=l->materials+material;
        if(m->flags!=SNESRECOMP_PPU_MATERIAL_SOURCE_COLOUR)continue;
        const uint32_t bit=UINT32_C(1)<<material;referenced|=bit;
        const unsigned layer=l->source_layers[plane];
        if(layer>4 || layer==3 || (m->occluding_bg>2 && m->occluding_bg!=255) ||
           m->brightness_reduction || m->reserved){rejected|=bit;continue;}
        const unsigned own=1u<<layer,other=m->occluding_bg==255?0u:1u<<m->occluding_bg;
        const SnesPpuRasterBand *b=cap->bands+band;
        /* The selected primitive is strictly above the optional occluder.
         * With these guards, its isolated main pixel is its own palette index;
         * neither a subscreen operand nor a backdrop can change that pixel. */
        if(b->bg_mode!=1 || b->forced_blank || !(m->main_enable&own) ||
           (m->main_enable&~(own|other)) || (m->main_enable&~b->main_enable) ||
           (b->cgadsub&own) || IndexedMaterialWindow(cap,band,layer) ||
           (other && IndexedMaterialWindow(cap,band,m->occluding_bg)) || (b->cgwsel>>6)) {
            rejected|=bit;continue;
        }
        if(layer<3) {
            if(b->bg[layer].margin_left>cap->layout.extra_left_cur ||
               b->bg[layer].margin_right>cap->layout.extra_right_cur)rejected|=bit;
        } else for(unsigned r=0;r<l->obj_run_count;++r) {
            const SnesRecompPpuObjRun *run=l->obj_runs+r;
            if(run->sliver.line<b->y_begin || run->sliver.line>=b->y_end)continue;
            const int left=run->sliver.screen_x+run->overlay_begin;
            const int right=run->sliver.screen_x+run->overlay_end;
            if(left<-(int)cap->layout.extra_left_cur || right>256+(int)cap->layout.extra_right_cur) {
                rejected|=bit;break;
            }
        }
    }
    return referenced&~rejected;
}
typedef struct Source {
    uint16_t x0,y0,x1,y1,priority,classes,winner,owner,band;
    uint8_t palette,plane;
} Source;
typedef struct Link { uint32_t next,source; } Link;
struct SnesRecompIndexedDrawCache {
    SnesRecompIndexedDraw *draws[2];
    Source *sources;
    Link *links;
    uint32_t heads[5*CELLS],seen[SOURCES],stamp;
    uint16_t line_band[256];
    SnesRecompIndexedDrawPlan plan;
    uint64_t owner,layout,objects,projection,background,frame_owner,vram;
    uint32_t capacity;
    unsigned active;
    bool valid,bg_source_ready;
    uint32_t bg_sources,bg_projection_first,bg_projection_count;
    uint16_t bg_materials[SNES_PPU_MAX_BANDS*5],bg_bands;
    uint8_t bg_planes;
};
SnesRecompIndexedDrawCache *snesrecomp_ppu_indexed_draw_create(uint32_t capacity) {
    if(!capacity || capacity>262144u) return NULL;
    SnesRecompIndexedDrawCache *c=calloc(1,sizeof *c);if(!c) return NULL;
    c->capacity=capacity;
    c->draws[0]=malloc((size_t)capacity*sizeof *c->draws[0]);
    c->draws[1]=malloc((size_t)capacity*sizeof *c->draws[1]);
    c->sources=malloc(SOURCES*sizeof *c->sources);c->links=malloc(LINKS*sizeof *c->links);
    if(!c->draws[0] || !c->draws[1] || !c->sources || !c->links) {
        snesrecomp_ppu_indexed_draw_destroy(c);return NULL;
    }
    return c;
}
void snesrecomp_ppu_indexed_draw_destroy(SnesRecompIndexedDrawCache *c) {
    if(!c) return;
    free(c->draws[0]);free(c->draws[1]);free(c->sources);free(c->links);free(c);
}
void snesrecomp_ppu_indexed_draw_reset(SnesRecompIndexedDrawCache *c) {if(c){c->valid=false;c->bg_source_ready=false;}}
static bool AddSource(SnesRecompIndexedDrawCache *c,Source s,unsigned *count,unsigned *links,bool *same) {
    if(s.x0>=s.x1 || s.y0>=s.y1 || !s.classes) return true;
    if(s.x1>512 || s.y1>256 || s.plane>=5 || *count==SOURCES) return false;
    const unsigned n=(*count)++;
    if(same && *same && (n>=c->bg_sources || n>=c->bg_projection_first ||
        memcmp(c->sources+n,&s,sizeof s))) *same=false;
    c->sources[n]=s;
    for(unsigned y=s.y0/8u;y<=(s.y1-1u)/8u;++y)
    for(unsigned x=s.x0/8u;x<=(s.x1-1u)/8u;++x) {
        if(*links==LINKS) return false;
        const unsigned cell=s.plane*CELLS+y*64u+x;
        c->links[*links]=(Link){c->heads[cell],n};c->heads[cell]=(*links)++;
    }
    return true;
}
static bool AppendDraw(SnesRecompIndexedDraw *draws,SnesRecompIndexedDrawPlan *plan,
    unsigned first,unsigned capacity,SnesRecompIndexedDraw next) {
    /* Row-slivers are original authority, not necessarily separate GPU draws.
     * Merge only inside this one ordered projection rectangle. Native winner,
     * full Z, material and source sampling must ALL remain identical. */
    const unsigned begin=plan->count>64u && plan->count-64u>first?plan->count-64u:first;
    for(unsigned i=plan->count;i>begin;) {
        SnesRecompIndexedDraw *a=draws+--i;
        if(a->priority!=next.priority || a->material!=next.material || a->winner!=next.winner || a->index!=next.index ||
           a->rect.x0!=next.rect.x0 || a->rect.x1!=next.rect.x1 || a->rect.y1!=next.rect.y0 ||
           a->rect.source_x!=next.rect.source_x || a->rect.owner!=next.rect.owner || a->rect.object!=next.rect.object ||
           a->rect.plane!=next.rect.plane || a->rect.x_step!=next.rect.x_step || a->rect.y_step!=next.rect.y_step ||
           a->rect.owner_required!=next.rect.owner_required ||
           a->rect.source_y+(a->rect.y1-a->rect.y0)*a->rect.y_step!=next.rect.source_y) continue;
        a->rect.y1=next.rect.y1;return true;
    }
    if(plan->count==capacity)return false;
    draws[plan->count++]=next;return true;
}
SnesRecompPpuResult snesrecomp_ppu_indexed_draw_prepare(SnesRecompIndexedDrawCache *c,
    const SnesRecompPpuJob *job,const SnesRecompIndexedScenePlan *scene,
    const SnesRecompIndexedProjectionPlan *projection,SnesRecompIndexedDrawPlan *out) {
    if(!out) return SNESRECOMP_PPU_INVALID;
    memset(out,0,sizeof *out);
    const SnesRecompPpuJobView *v=snesrecomp_ppu_job_view(job);
    if(!c || !v || !v->indexed_layout || !scene || !projection ||
       (projection->count && !projection->rects)) return SNESRECOMP_PPU_INVALID;
    const SnesRecompPpuIndexedLayout *l=v->indexed_layout;
    const SnesPpuFrameCapture *cap=snesrecomp_ppu_frame_capture(v->graphics_frame);
    const SnesRecompPpuFrameVersions *versions=snesrecomp_ppu_frame_versions(v->graphics_frame);
    const SnesRecompIndexedBackgroundPlan *bg=&scene->background;
    if(!cap || !versions || !l->material_count || scene->obj_count!=l->obj_run_count ||
       bg->quad_count>SNESRECOMP_TILE_PLAN_QUADS || !bg->classes ||
       (bg->quad_count && !bg->quads) || (scene->obj_count && !scene->obj_slots)) return SNESRECOMP_PPU_INVALID;
    if(c->valid && c->owner==v->owner && c->layout==v->indexed_geometry_version &&
       c->objects==v->indexed_obj_version && c->projection==projection->generation &&
       c->background==bg->geometry_generation && c->frame_owner==versions->owner && c->vram==versions->vram) {
        *out=c->plan;out->reused=1;out->background_reused=1;out->candidates_examined=0;return SNESRECOMP_PPU_OK;
    }
    if(c->plan.generation==UINT64_MAX) return SNESRECOMP_PPU_RESOURCE_LIMIT;
    for(unsigned band=0;band<cap->band_count;++band)
        for(unsigned y=cap->bands[band].y_begin;y<cap->bands[band].y_end;++y) c->line_band[y]=(uint16_t)band;
    memset(c->heads,255,sizeof c->heads);
    /* Keep BG sources first, and preserve exact projection inputs in the
     * unused tail of the existing source arena. No additional bulk allocation
     * or hash-based equality is needed. Refusal invalidates this scratch proof;
     * the last successful double-buffered output remains intact. */
    _Static_assert(sizeof(Source)>=sizeof(SnesRecompIndexedProjectionRect),"projection scratch slot");
    bool reuse_bg=c->valid && c->bg_source_ready && c->owner==v->owner &&
        c->frame_owner==versions->owner && c->bg_planes==l->plane_count && c->bg_bands==l->band_count;
    c->bg_source_ready=false;
    unsigned sources=0,links=0,bg_sources=0,bg_projections=0;
    for(unsigned phase=0;phase<2;++phase) {
    for(unsigned p=0;p<l->plane_count;++p) {
        if((l->source_layers[p]==4)!=(phase==1))continue;
        if(l->source_layers[p]==4) {
            for(unsigned i=0;i<l->obj_run_count;++i) {
                const SnesRecompPpuObjRun *r=l->obj_runs+i;const SnesRecompObjSliver *s=&r->sliver;
                if(!(r->flags&SNESRECOMP_OBJ_RUN_OVERLAY)) continue;
                const int x=(int)s->screen_x+cap->canvas_extra;
                int begin=x+r->overlay_begin,end=x+r->overlay_end;
                if(begin<0)begin=0;
                if(end>(int)l->width)end=l->width;
                if(begin>=end) continue;
                const unsigned slot=scene->obj_slots[i];if(slot>=SNESRECOMP_COLOUR_TILE_SLOTS) return SNESRECOMP_PPU_INVALID;
                const uint16_t depth=(uint16_t)(((s->priority*4u+2u)<<12)|((s->math_eligible?4u:6u)<<8));
                const Source next={(uint16_t)begin,s->line,(uint16_t)end,(uint16_t)(s->line+1u),
                    depth,bg->classes[slot],s->oam_slot,r->owner,c->line_band[s->line],s->palette_base,(uint8_t)p};
                if(!AddSource(c,next,&sources,&links,NULL)) return SNESRECOMP_PPU_RESOURCE_LIMIT;
            }
        } else for(unsigned i=0;i<bg->quad_count;++i) {
            const SnesRecompIndexedTileQuad *q=bg->quads+i;
            if(q->screens!=1 || q->layer!=l->source_layers[p]) continue;
            if(q->slot>=SNESRECOMP_COLOUR_TILE_SLOTS || q->x0<0 || q->y0<0 || q->x1>512 || q->y1>256 ||
               q->x0>=q->x1 || q->y0>=q->y1) return SNESRECOMP_PPU_INVALID;
            for(unsigned y=(unsigned)q->y0;y<(unsigned)q->y1;) {
                const unsigned band=c->line_band[y];unsigned end=cap->bands[band].y_end;
                if(end>(unsigned)q->y1)end=(unsigned)q->y1;
                const uint16_t depth=snesrecomp_ppu_mode1_bg_depth_base(q->layer,q->priority,cap->bands[band].bg3_priority);
                if(!depth || end<=y) return SNESRECOMP_PPU_INVALID;
                const Source next={(uint16_t)q->x0,(uint16_t)y,(uint16_t)q->x1,(uint16_t)end,
                    depth,bg->classes[q->slot],0,0,(uint16_t)band,q->palette_base,(uint8_t)p};
                if(!AddSource(c,next,&sources,&links,&reuse_bg)) return SNESRECOMP_PPU_RESOURCE_LIMIT;
                y=end;
            }
        }
    }
    if(!phase) {
        bg_sources=sources;
        reuse_bg=reuse_bg && bg_sources==c->bg_sources;
        for(unsigned band=0;reuse_bg && band<l->band_count;++band)
            for(unsigned p=0;p<l->plane_count;++p)if(l->source_layers[p]!=4 &&
                c->bg_materials[band*5u+p]!=l->band_materials[band*l->plane_count+p])reuse_bg=false;
        for(unsigned i=0;i<projection->count;++i) {
            const SnesRecompIndexedProjectionRect *r=projection->rects+i;
            if(r->plane>=l->plane_count)return SNESRECOMP_PPU_INVALID;
            if(l->source_layers[r->plane]==4)continue;
            if(reuse_bg && (bg_projections>=c->bg_projection_count ||
                memcmp(c->sources+c->bg_projection_first+bg_projections,r,sizeof *r)))reuse_bg=false;
            ++bg_projections;
        }
        reuse_bg=reuse_bg && bg_projections==c->bg_projection_count;
    }
    }
    SnesRecompIndexedDrawPlan plan={.draws=c->draws[c->active^1u],.generation=c->plan.generation+1u,
        .background_reused=reuse_bg};
    SnesRecompIndexedDraw *draws=c->draws[c->active^1u];
    for(unsigned p=0;p<l->plane_count;++p) {
        plan.first[p]=plan.count;
        if(reuse_bg && l->source_layers[p]!=4) {
            const unsigned count=c->plan.counts[p];
            if(count>c->capacity-plan.count)return SNESRECOMP_PPU_RESOURCE_LIMIT;
            if(count)memcpy(draws+plan.count,c->plan.draws+c->plan.first[p],(size_t)count*sizeof *draws);
            plan.count+=count;
        } else for(unsigned i=0;i<projection->count;++i) {
            const SnesRecompIndexedProjectionRect *r=projection->rects+i;if(r->plane!=p)continue;
            const unsigned first=plan.count;
            if(r->x0>=r->x1 || r->y0>=r->y1 || r->x_step>1 || r->y_step>1) return SNESRECOMP_PPU_INVALID;
            const unsigned sx1=r->source_x+(r->x_step?r->x1-r->x0:1u),
                sy1=r->source_y+(r->y_step?r->y1-r->y0:1u);
            if(sx1>512 || sy1>256) return SNESRECOMP_PPU_INVALID;
            if(++c->stamp==0){memset(c->seen,0,sizeof c->seen);c->stamp=1;}
            for(unsigned y=r->source_y/8u;y<=(sy1-1u)/8u;++y)
            for(unsigned x=r->source_x/8u;x<=(sx1-1u)/8u;++x)
            for(unsigned link=c->heads[p*CELLS+y*64u+x];link!=UINT32_MAX;link=c->links[link].next) {
                const unsigned n=c->links[link].source;if(c->seen[n]==c->stamp)continue;c->seen[n]=c->stamp;
                const Source *s=c->sources+n;++plan.candidates_examined;
                if(l->source_layers[p]==4 && s->owner!=r->owner) continue;
                SnesRecompIndexedProjectionRect clipped;
                const int result=snesrecomp_ppu_indexed_projection_clip(r,s->x0,s->y0,s->x1,s->y1,&clipped);
                if(result<0)return SNESRECOMP_PPU_INVALID;
                if(!result)continue;
                const unsigned material=l->band_materials[s->band*l->plane_count+p];
                for(unsigned index=1;index<16;++index) if(s->classes&(1u<<index)) {
                    const SnesRecompIndexedDraw next={clipped,(uint16_t)(s->priority+s->palette+index),
                        (uint16_t)material,l->source_layers[p]==4?s->winner:r->owner,(uint8_t)(s->palette+index),0};
                    if(!AppendDraw(draws,&plan,first,c->capacity,next))return SNESRECOMP_PPU_RESOURCE_LIMIT;
                }
            }
        }
        plan.counts[p]=plan.count-plan.first[p];
        const bool same=c->valid && c->plan.counts[p]==plan.counts[p] &&
            (!plan.counts[p] || !memcmp(c->plan.draws+c->plan.first[p],draws+plan.first[p],
                (size_t)plan.counts[p]*sizeof *draws));
        if(!same && c->plan.plane_generation[p]==UINT64_MAX)return SNESRECOMP_PPU_RESOURCE_LIMIT;
        plan.plane_generation[p]=c->plan.plane_generation[p]+!same;
    }
    if(bg_projections<=SOURCES-sources) {
        c->bg_projection_first=SOURCES-bg_projections;c->bg_projection_count=bg_projections;
        unsigned n=0;
        for(unsigned i=0;i<projection->count;++i) {
            const SnesRecompIndexedProjectionRect *r=projection->rects+i;
            if(l->source_layers[r->plane]!=4)memcpy(c->sources+c->bg_projection_first+n++,r,sizeof *r);
        }
        c->bg_sources=bg_sources;c->bg_planes=l->plane_count;c->bg_bands=l->band_count;
        for(unsigned band=0;band<l->band_count;++band)for(unsigned p=0;p<l->plane_count;++p)
            c->bg_materials[band*5u+p]=l->band_materials[band*l->plane_count+p];
        c->bg_source_ready=true;
    }
    c->owner=v->owner;c->layout=v->indexed_geometry_version;c->objects=v->indexed_obj_version;
    c->projection=projection->generation;c->background=bg->geometry_generation;
    c->frame_owner=versions->owner;c->vram=versions->vram;c->active^=1u;c->valid=true;c->plan=plan;
    *out=plan;return SNESRECOMP_PPU_OK;
}
