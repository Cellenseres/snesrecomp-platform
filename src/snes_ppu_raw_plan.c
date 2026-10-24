#include "snesrecomp_platform/snes_ppu_raw_plan.h"
#include "snesrecomp_platform/snes_ppu_mode7.h"
#include "snesrecomp_platform/snes_ppu_semantic_gpu.h"
#include "snesrecomp_platform/snes_ppu_tile_plan.h"
#include "snesrecomp_platform/snes_ppu_indexed_tiles.h"
#include "snes_ppu_windows.h"
#include <stdlib.h>
#include <string.h>

struct SnesRecompPpuRawCache {
    SnesRecompPpuRawRegion regions[SNESRECOMP_RAW_REGION_LIMIT];
    SnesRecompPpuRawRegion scratch[SNESRECOMP_RAW_REGION_LIMIT];
    uint32_t objects[SNESRECOMP_PPU_MAX_WIDTH*SNES_PPU_MAX_BANDS];
    uint8_t object_classes[((SNESRECOMP_PPU_MAX_WIDTH+7)/8)*((SNES_PPU_MAX_BANDS+7)/8)];
    SnesRecompPpuFrameVersions versions;
    uint64_t region_generation,object_generation;
    uint32_t region_count;
    uint16_t width,height;
    bool regions_valid,objects_valid,objects_any,objects_present;
};
SnesRecompPpuRawCache *snesrecomp_ppu_raw_create(void) {
    return calloc(1,sizeof(SnesRecompPpuRawCache));
}
void snesrecomp_ppu_raw_destroy(SnesRecompPpuRawCache *c) { free(c); }
void snesrecomp_ppu_raw_reset(SnesRecompPpuRawCache *c) {
    if(c) c->regions_valid=c->objects_valid=false;
}
static uint32_t RawColour(uint16_t colour,unsigned alpha) {
    return (uint32_t)alpha<<24 | ((uint32_t)colour&31u)<<19 |
        (((uint32_t)colour>>5)&31u)<<11 | (((uint32_t)colour>>10)&31u)<<3;
}
static bool SameMaterial(const SnesRecompPpuRawRegion *a,const SnesRecompPpuRawRegion *b) {
    return a->flags==b->flags && a->brightness_reduction==b->brightness_reduction &&
        a->main_backdrop==b->main_backdrop && a->sub_backdrop==b->sub_backdrop;
}
static bool Region(SnesRecompPpuRawCache *c,unsigned *count,
                   const SnesRecompPpuRawRegion *next) {
    for(unsigned i=0;i<*count;++i) {
        SnesRecompPpuRawRegion *old=&c->scratch[i];
        if(old->x0==next->x0 && old->x1==next->x1 && old->y1==next->y0 &&
           SameMaterial(old,next)) {
            old->y1=next->y1;return true;
        }
    }
    if(*count==SNESRECOMP_RAW_REGION_LIMIT) return false;
    c->scratch[(*count)++]=*next;return true;
}
static SnesRecompPpuResult RawPlan(SnesRecompPpuRawCache *c,
    const SnesRecompPpuFrame *frame,SnesRecompPpuRawPlan *out,bool mode7,bool capture_objects) {
    if(!c || !out) return SNESRECOMP_PPU_INVALID;
    memset(out,0,sizeof(*out));
    const SnesPpuFrameCapture *cap=snesrecomp_ppu_frame_capture(frame);
    if(!cap) return SNESRECOMP_PPU_INVALID;
    out->unsupported=mode7?snesrecomp_ppu_mode7_supports(cap):capture_objects?
        snesrecomp_ppu_mode1_raw_supports(cap):snesrecomp_ppu_mode1_indexed_supports(cap);
    if(out->unsupported) return SNESRECOMP_PPU_UNSUPPORTED;
    const SnesRecompObjFrame *slivers=snesrecomp_ppu_frame_obj(frame);
    if(capture_objects && !cap->obj_pixels && slivers && slivers->count) return SNESRECOMP_PPU_INCOMPLETE_MEMORY;
    SnesRecompWindowSpan windows[SNESRECOMP_WINDOW_MAX_SPANS];
    unsigned window_count=0,region_count=0;
    for(unsigned bi=0;bi<cap->band_count;++bi) {
        const SnesPpuRasterBand *b=&cap->bands[bi];
        if(!bi || !snesrecomp_ppu_windows_equal(b,cap->bands+bi-1) ||
            !snesrecomp_ppu_resolved_windows_equal(cap,bi,bi-1u))
            window_count=snesrecomp_ppu_window_spans(cap,b,windows,SNESRECOMP_WINDOW_MAX_SPANS);
        if(!window_count) return SNESRECOMP_PPU_INVALID;
        for(unsigned y=b->y_begin;y<b->y_end;) {
            const uint16_t *palette=cap->cgram_lines?cap->cgram_lines+y*256:cap->cgram;
            /* Only backdrop index zero affects these regions. Other palette
             * animation still reaches tile/OBJ caches independently. */
            unsigned end=b->y_end;
            if(cap->cgram_lines && !b->forced_blank) {
                end=y+1;
                while(end<b->y_end &&
                    ((cap->cgram_lines[end*256]^palette[0])&32767u)==0) ++end;
            }
            SnesRecompPpuRawRegion pending={0};bool have_pending=false;
            for(unsigned wi=0;wi<window_count;++wi) {
                const SnesRecompWindowSpan *w=&windows[wi];
                const bool rgb=(w->mask&SNESRECOMP_SEMANTIC_MAIN_RGB)!=0;
                const bool math=(w->mask&SNESRECOMP_SEMANTIC_MATH)!=0;
                SnesRecompPpuRawRegion r={.x0=w->begin,.x1=w->end,
                    .y0=(uint16_t)y,.y1=(uint16_t)end};
                if(!b->forced_blank) {
                    r.brightness_reduction=(uint8_t)(15u-b->brightness);
                    r.main_backdrop=RawColour(rgb?palette[0]:0,math&&(b->cgadsub&32u)?255:0);
                    /* No genuine subscreen operand disables half colour.
                     * Fixed-colour mode permits it, including on backdrop. */
                    r.sub_backdrop=RawColour(b->fixed_colour,(b->cgwsel&2u)?0:255);
                    r.flags=(uint8_t)((rgb?SNESRECOMP_RAW_MAIN_RGB:0) |
                        (b->cgadsub&64u?SNESRECOMP_RAW_HALF:0) |
                        (b->cgadsub&128u?SNESRECOMP_RAW_SUBTRACT:0) |
                        (w->obj_mask&SNESRECOMP_SEMANTIC_OBJ_MAIN?SNESRECOMP_RAW_OBJ_MAIN:0) |
                        ((b->cgwsel&2u)&&(w->obj_mask&SNESRECOMP_SEMANTIC_OBJ_SUB)?SNESRECOMP_RAW_OBJ_SUB:0) |
                        (math&&(b->cgadsub&16u)?SNESRECOMP_RAW_OBJ_MATH:0));
                }
                /* BG masks already clip tile geometry. They do not require
                 * distinct backdrop/math/OBJ draws when this material agrees.
                 * Merge horizontally before vertical coalescing, so each band
                 * has a stable partition regardless of redundant BG edges. */
                if(have_pending && pending.x1==r.x0 && SameMaterial(&pending,&r)) pending.x1=r.x1;
                else {
                    if(have_pending && !Region(c,&region_count,&pending)) return SNESRECOMP_PPU_RESOURCE_LIMIT;
                    pending=r;have_pending=true;
                }
            }
            if(have_pending && !Region(c,&region_count,&pending)) return SNESRECOMP_PPU_RESOURCE_LIMIT;
            y=end;
        }
    }
    const SnesRecompPpuFrameVersions *versions=snesrecomp_ppu_frame_versions(frame);
    const bool same_size=c->width==cap->canvas_width && c->height==cap->visible_height;
    const bool same_regions=c->regions_valid && same_size && c->region_count==region_count &&
        !memcmp(c->regions,c->scratch,region_count*sizeof(*c->regions));
    const bool same_objects=c->objects_valid && same_size &&
        c->objects_present==(cap->obj_pixels!=NULL) && c->versions.owner==versions->owner &&
        c->versions.palette==versions->palette && c->versions.objects==versions->objects;
    if((!same_regions && c->region_generation==UINT64_MAX) ||
       (capture_objects && !same_objects && c->object_generation==UINT64_MAX)) return SNESRECOMP_PPU_RESOURCE_LIMIT;
    if(!same_regions) {
        memcpy(c->regions,c->scratch,region_count*sizeof(*c->regions));
        c->region_count=region_count;c->regions_valid=true;++c->region_generation;
    }
    const uint64_t *coverage=capture_objects?snesrecomp_ppu_frame_obj_coverage(frame):NULL;
    if(capture_objects && !same_objects) {
        memset(c->object_classes,0,sizeof c->object_classes);
        memset(c->objects,0,(size_t)cap->canvas_width*cap->visible_height*sizeof(*c->objects));
        c->objects_any=false;
        for(unsigned y=0;cap->obj_pixels && y<cap->visible_height;++y) {
            const uint16_t *palette=cap->cgram_lines?cap->cgram_lines+y*256:cap->cgram;
            for(unsigned x=0;x<cap->canvas_width;) {
                unsigned end=x+8<cap->canvas_width?x+8:cap->canvas_width;
                if(coverage[y]&(UINT64_C(1)<<(x>>3)))
                    for(;x<end;++x) {
                        uint16_t p=cap->obj_pixels[y*cap->obj_pitch+x];
                        if(!(p&0x800u)) continue;
                        unsigned alpha=(((p>>8)&3u)+1)*64u-2u+((p>>10)&1u);
                        c->objects[y*cap->canvas_width+x]=RawColour(palette[p&255u],alpha);
                        const unsigned object_class=((p>>8)&3u)*2u+((p>>10)&1u);
                        c->object_classes[(y>>3)*((cap->canvas_width+7u)/8u)+(x>>3)] |=
                            (uint8_t)(1u<<object_class);
                        c->objects_any=true;
                    }
                else x=end;
            }
        }
        c->objects_present=cap->obj_pixels!=NULL;c->objects_valid=true;++c->object_generation;
    }
    /* Region preparation updates frame/size certificates. It cannot certify
     * any stale object image retained from an earlier full preparation. */
    if(!capture_objects) c->objects_valid=false;
    c->width=cap->canvas_width;c->height=cap->visible_height;c->versions=*versions;
    out->regions=c->regions;out->region_count=c->region_count;
    out->objects=capture_objects && c->objects_any?c->objects:NULL;out->object_coverage=coverage;
    out->width=c->width;out->height=c->height;
    out->region_generation=c->region_generation;out->object_generation=capture_objects?c->object_generation:0;
    out->object_classes=capture_objects && c->objects_any?c->object_classes:NULL;
    out->object_class_pitch=capture_objects?(uint16_t)((cap->canvas_width+7u)/8u):0;
    return SNESRECOMP_PPU_OK;
}
SnesRecompPpuResult snesrecomp_ppu_mode7_raw_plan(SnesRecompPpuRawCache *c,
    const SnesRecompPpuFrame *frame,SnesRecompPpuRawPlan *out) {
    return RawPlan(c,frame,out,true,true);
}
SnesRecompPpuResult snesrecomp_ppu_mode1_raw_plan(SnesRecompPpuRawCache *c,
    const SnesRecompPpuFrame *frame,SnesRecompPpuRawPlan *out) {
    return RawPlan(c,frame,out,false,true);
}
SnesRecompPpuResult snesrecomp_ppu_mode1_raw_regions(SnesRecompPpuRawCache *c,
    const SnesRecompPpuFrame *frame,SnesRecompPpuRawPlan *out) {
    return RawPlan(c,frame,out,false,false);
}
