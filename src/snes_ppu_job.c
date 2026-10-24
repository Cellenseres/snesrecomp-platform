#include "snesrecomp_platform/snes_ppu_job.h"
#include "snesrecomp_platform/memory_compare.h"
#include <stdlib.h>
#include <string.h>

struct SnesRecompPpuJob {
    unsigned references;
    uint32_t capacity;
    unsigned active;
    uint16_t canvas_width, canvas_height;
    bool valid;
    SnesRecompPpuJobView view;
    SnesRecompPpuJobImage images[SNESRECOMP_PPU_JOB_MAX_IMAGES];
    SnesRecompPpuOverlay overlays[SNESRECOMP_PPU_JOB_MAX_OVERLAYS];
    SnesRecompPpuSource sources[SNESRECOMP_PPU_JOB_MAX_SOURCES];
    uint64_t versions[SNESRECOMP_PPU_JOB_MAX_IMAGES];
    bool premultiplied_valid[SNESRECOMP_PPU_JOB_MAX_IMAGES];
    uint32_t *payload[2];
    uint64_t payload_geometry[2];
    uint64_t payload_versions[2][SNESRECOMP_PPU_JOB_MAX_IMAGES];
    SnesRecompBlitRect *blit_rects[2];
    SnesRecompBlitPlan blits;
    uint32_t blit_capacity;
    unsigned blit_active;
    bool blits_valid;
    SnesRecompPpuPrimitive *primitives[2];
    SnesRecompPpuMaterial materials[2][SNESRECOMP_PPU_JOB_MAX_MATERIALS];
    uint32_t primitive_capacity;
    unsigned primitive_active;
    bool primitives_valid;
    SnesRecompPpuIndexedLayout indexed;
    SnesRecompPpuIndexedClaim *indexed_claims[2];
    SnesRecompPpuIndexedObject *indexed_objects[2];
    SnesRecompPpuAxisRun *indexed_axes[2];
    SnesRecompPpuObjRun *indexed_runs[2];
    SnesRecompPpuMaterial indexed_materials[2][SNESRECOMP_PPU_JOB_MAX_MATERIALS];
    uint16_t indexed_band_materials[2][SNES_PPU_MAX_BANDS*SNESRECOMP_PPU_INDEXED_MAX_PLANES];
    uint8_t indexed_owners[2][128];
    uint32_t indexed_claim_capacity,indexed_object_capacity,indexed_axis_capacity;
    unsigned indexed_active;
    unsigned indexed_run_active;
    bool indexed_valid;
};
_Static_assert(sizeof(SnesRecompPpuPrimitive)==24,"primitive SDK boundary");
_Static_assert(sizeof(SnesRecompPpuMaterial)==8,"material SDK boundary");
static uint64_t s_job_owner;

static SnesRecompPpuJob *Create(uint32_t capacity,uint32_t maximum,uint32_t rectangles) {
    if(capacity>maximum || s_job_owner==UINT64_MAX) return NULL;
    SnesRecompPpuJob *j=calloc(1,sizeof(*j));
    if(!j) return NULL;
    if(capacity) {
        for(unsigned i=0;i<2;++i) j->payload[i]=malloc((size_t)capacity*sizeof(uint32_t));
        if(!j->payload[0] || !j->payload[1]) {
            free(j->payload[0]);free(j->payload[1]);free(j);return NULL;
        }
    }
    if(rectangles) {
        for(unsigned i=0;i<2;++i) j->blit_rects[i]=malloc((size_t)rectangles*sizeof(SnesRecompBlitRect));
        if(!j->blit_rects[0] || !j->blit_rects[1]) {
            free(j->blit_rects[0]);free(j->blit_rects[1]);
            free(j->payload[0]);free(j->payload[1]);free(j);return NULL;
        }
    }
    j->references=1;j->capacity=capacity;j->blit_capacity=rectangles;j->view.owner=++s_job_owner;
    return j;
}
SnesRecompPpuJob *snesrecomp_ppu_job_create(uint32_t capacity) {
    return Create(capacity,SNESRECOMP_PPU_JOB_MAX_PIXELS,0);
}
SnesRecompPpuJob *snesrecomp_ppu_job_create_blits(uint32_t capacity,uint32_t rectangles) {
    if(!rectangles || rectangles>SNESRECOMP_BLIT_MAX_RECTS) return NULL;
    return Create(capacity,SNESRECOMP_PPU_JOB_MAX_BLIT_PIXELS,rectangles);
}
SnesRecompPpuJob *snesrecomp_ppu_job_create_primitives(uint32_t capacity,uint32_t primitives) {
    if(!primitives || primitives>SNESRECOMP_PPU_JOB_MAX_PRIMITIVES) return NULL;
    SnesRecompPpuJob *j=Create(capacity,SNESRECOMP_PPU_JOB_MAX_BLIT_PIXELS,0);
    if(!j) return NULL;
    for(unsigned i=0;i<2;++i)
        j->primitives[i]=malloc((size_t)primitives*sizeof(*j->primitives[i]));
    if(!j->primitives[0] || !j->primitives[1]) {
        snesrecomp_ppu_job_release(j);return NULL;
    }
    j->primitive_capacity=primitives;return j;
}
void snesrecomp_ppu_job_retain(const SnesRecompPpuJob *job) {
    if(job) ++((SnesRecompPpuJob*)job)->references;
}
void snesrecomp_ppu_job_release(const SnesRecompPpuJob *job) {
    if(job && --((SnesRecompPpuJob*)job)->references==0) {
        SnesRecompPpuJob *j=(SnesRecompPpuJob*)job;
        snesrecomp_ppu_frame_release(j->view.frame);
        snesrecomp_ppu_frame_release(j->view.graphics_frame);
        free(j->primitives[0]);free(j->primitives[1]);
        for(unsigned i=0;i<2;++i) {
            free(j->indexed_claims[i]);free(j->indexed_objects[i]);free(j->indexed_axes[i]);
            free(j->indexed_runs[i]);
        }
        free(j->blit_rects[0]);free(j->blit_rects[1]);
        free(j->payload[0]);free(j->payload[1]);free(j);
    }
}
SnesRecompPpuResult snesrecomp_ppu_job_detach_frame(SnesRecompPpuJob *j) {
    if(!j) return SNESRECOMP_PPU_INVALID;
    if(j->references!=1) return SNESRECOMP_PPU_BUSY;
    snesrecomp_ppu_frame_release(j->view.frame);
    snesrecomp_ppu_frame_release(j->view.graphics_frame);
    j->view.frame=NULL;
    j->view.graphics_frame=NULL;
    return SNESRECOMP_PPU_OK;
}
static bool SameImage(const SnesRecompPpuJobImage *a,const SnesRecompPpuJobImage *b) {
    if(a->width!=b->width || a->height!=b->height) return false;
    if(a->pixels==b->pixels && a->pitch==b->pitch) return true;
    if(a->pitch==a->width && b->pitch==b->width)
        return snesrecomp_u8_equal((const uint8_t *)a->pixels,(const uint8_t *)b->pixels,
            (size_t)a->width*a->height*sizeof(uint32_t));
    for(unsigned y=0;y<a->height;++y)
        if(!snesrecomp_u8_equal((const uint8_t *)(a->pixels+(size_t)y*a->pitch),
            (const uint8_t *)(b->pixels+(size_t)y*b->pitch),
            (size_t)a->width*sizeof(uint32_t))) return false;
    return true;
}
static bool SameOverlay(const SnesRecompPpuOverlay *a,const SnesRecompPpuOverlay *b) {
    return a->x==b->x && a->y==b->y && a->width==b->width && a->height==b->height &&
        a->source_x==b->source_x && a->source_y==b->source_y && a->image==b->image &&
        a->blend==b->blend && a->condition==b->condition && a->key_argb==b->key_argb &&
        a->brightness_reduction==b->brightness_reduction;
}
SnesRecompPpuResult snesrecomp_ppu_job_freeze_sources(SnesRecompPpuJob *j,
    const SnesRecompPpuFrame *frame,const SnesRecompPpuJobImage *images,
    uint16_t image_count,const SnesRecompPpuOverlay *overlays,
    uint16_t overlay_count,const SnesRecompPpuSource *sources,
    uint16_t source_count,uint64_t sequence) {
    if(!j) return SNESRECOMP_PPU_INVALID;
    if(j->references!=1) return SNESRECOMP_PPU_BUSY;
    const SnesPpuFrameCapture *c=snesrecomp_ppu_frame_capture(frame);
    if(!c || (image_count && !images) || (overlay_count && !overlays) ||
        (source_count && !sources)) return SNESRECOMP_PPU_INVALID;
    if(image_count>SNESRECOMP_PPU_JOB_MAX_IMAGES || overlay_count>SNESRECOMP_PPU_JOB_MAX_OVERLAYS ||
        source_count>SNESRECOMP_PPU_JOB_MAX_SOURCES)
        return SNESRECOMP_PPU_RESOURCE_LIMIT;
    /* Copy metadata before any commit: it may alias the previous owned view. */
    SnesRecompPpuJobImage source[SNESRECOMP_PPU_JOB_MAX_IMAGES];
    SnesRecompPpuOverlay rectangles[SNESRECOMP_PPU_JOB_MAX_OVERLAYS];
    SnesRecompPpuSource replacements[SNESRECOMP_PPU_JOB_MAX_SOURCES];
    uint64_t versions[SNESRECOMP_PPU_JOB_MAX_IMAGES];
    bool premultiplied[SNESRECOMP_PPU_JOB_MAX_IMAGES]={false};
    if(image_count) memcpy(source,images,image_count*sizeof(*source));
    if(overlay_count) memcpy(rectangles,overlays,overlay_count*sizeof(*rectangles));
    if(source_count) memcpy(replacements,sources,source_count*sizeof(*replacements));
    uint32_t pixels=0;
    for(unsigned i=0;i<image_count;++i) {
        const SnesRecompPpuJobImage *s=source+i;
        if(!s->pixels || !s->width || s->width>SNESRECOMP_PPU_MAX_WIDTH ||
            !s->height || s->height>SNES_PPU_MAX_BANDS || s->pitch<s->width ||
            s->pitch>SIZE_MAX/sizeof(uint32_t)/s->height) return SNESRECOMP_PPU_INVALID;
        const uint32_t size=(uint32_t)s->width*s->height;
        if(size>j->capacity-pixels) return SNESRECOMP_PPU_RESOURCE_LIMIT;
        pixels+=size;
    }
    bool same_geometry=j->valid && overlay_count==j->view.overlay_count && image_count==j->view.image_count &&
        source_count==j->view.source_count;
    /* Detaching the frame releases its memory, not owned overlay geometry.
     * Keep the successful canvas contract independently of a borrowed view. */
    same_geometry=same_geometry && j->canvas_width==c->canvas_width &&
        j->canvas_height==c->visible_height;
    for(unsigned i=0;same_geometry && i<image_count;++i)
        same_geometry=source[i].width==j->images[i].width && source[i].height==j->images[i].height;
    uint8_t row_counts[2][3][SNES_PPU_MAX_BANDS]={{{0}}};
    for(unsigned i=0;i<source_count;++i) {
        const SnesRecompPpuSource *r=replacements+i;
        if(r->image>=image_count || r->layer>=3 || !r->screens || r->screens>3 || r->priority>1 ||
            !r->width || !r->height || (uint32_t)r->x+r->width>c->canvas_width ||
            (uint32_t)r->y+r->height>c->visible_height ||
            (r->source_width!=1 && r->source_width!=r->width) ||
            (r->source_height!=1 && r->source_height!=r->height) ||
            (uint32_t)r->source_x+r->source_width>source[r->image].width ||
            (uint32_t)r->source_y+r->source_height>source[r->image].height)
            return SNESRECOMP_PPU_INVALID;
        for(unsigned k=0;k<i;++k) {
            const SnesRecompPpuSource *p=replacements+k;
            if(p->layer==r->layer && (p->screens&r->screens) && p->x<r->x+r->width &&
                r->x<p->x+p->width && p->y<r->y+r->height && r->y<p->y+p->height)
                return SNESRECOMP_PPU_INVALID;
        }
        for(unsigned screen=0;screen<2;++screen) if(r->screens&(1u<<screen))
            for(unsigned y=r->y;y<r->y+r->height;++y)
                if(++row_counts[screen][r->layer][y]>SNESRECOMP_PPU_JOB_SOURCES_PER_ROW)
                    return SNESRECOMP_PPU_RESOURCE_LIMIT;
        if(same_geometry && memcmp(r,j->sources+i,sizeof(*r))) same_geometry=false;
    }
    for(unsigned i=0;i<overlay_count;++i) {
        const SnesRecompPpuOverlay *r=rectangles+i;
        if(r->image>=image_count || !r->width || !r->height || r->brightness_reduction>15u ||
            r->blend>SNESRECOMP_PPU_OVERLAY_REPLACE || r->condition>SNESRECOMP_PPU_OVERLAY_KEY_OR_BLACK ||
            (uint32_t)r->x+r->width>c->canvas_width || (uint32_t)r->y+r->height>c->visible_height ||
            (uint32_t)r->source_x+r->width>source[r->image].width ||
            (uint32_t)r->source_y+r->height>source[r->image].height ||
            (r->condition==SNESRECOMP_PPU_OVERLAY_KEY_OR_BLACK && r->key_argb>>24!=255u))
            return SNESRECOMP_PPU_INVALID;
        if(r->blend==SNESRECOMP_PPU_OVERLAY_PREMULTIPLIED) premultiplied[r->image]=true;
        if(same_geometry && !SameOverlay(r,j->overlays+i)) same_geometry=false;
    }
    bool same_payload=j->valid && image_count==j->view.image_count;
    for(unsigned i=0;i<image_count;++i) {
        const SnesRecompPpuJobImage *s=source+i;
        const bool same=j->valid && i<j->view.image_count && SameImage(s,j->images+i);
        same_payload &= same;
        if(premultiplied[i] && !(same && j->premultiplied_valid[i]))
            for(unsigned y=0;y<s->height;++y) for(unsigned x=0;x<s->width;++x) {
            const uint32_t p=s->pixels[(size_t)y*s->pitch+x],a=p>>24;
            if((p&255u)>a || ((p>>8)&255u)>a || ((p>>16)&255u)>a)
                return SNESRECOMP_PPU_UNSUPPORTED;
        }
        if(!same && j->versions[i]==UINT64_MAX) return SNESRECOMP_PPU_RESOURCE_LIMIT;
        versions[i]=j->versions[i]+!same;
    }
    if(!same_geometry && j->view.geometry_version==UINT64_MAX) return SNESRECOMP_PPU_RESOURCE_LIMIT;
    const unsigned next=same_payload?j->active:j->active^1u;
    pixels=0;
    for(unsigned i=0;i<image_count;++i) {
        const SnesRecompPpuJobImage *s=source+i;
        if(!same_payload) {
            uint32_t *out=j->payload[next]+pixels;
            /* Rotating buffers retain immutable images independently: an
             * animated HUD must not recopy a large unchanged world asset.
             * Exact input comparison produced versions above; geometry also
             * certifies every image's dimensions and packed offset. */
            if(!same_geometry || j->payload_geometry[next]!=j->view.geometry_version ||
                j->payload_versions[next][i]!=versions[i])
                for(unsigned y=0;y<s->height;++y)
                    memcpy(out+(size_t)y*s->width,s->pixels+(size_t)y*s->pitch,(size_t)s->width*sizeof(uint32_t));
            j->images[i]=(SnesRecompPpuJobImage){out,s->width,s->width,s->height};
        }
        j->payload_versions[next][i]=versions[i];
        j->premultiplied_valid[i]=premultiplied[i] ||
            (versions[i]==j->versions[i] && j->premultiplied_valid[i]);
        j->versions[i]=versions[i];pixels+=(uint32_t)s->width*s->height;
    }
    if(overlay_count) memcpy(j->overlays,rectangles,overlay_count*sizeof(*rectangles));
    if(source_count) memcpy(j->sources,replacements,source_count*sizeof(*replacements));
    snesrecomp_ppu_frame_retain(frame);snesrecomp_ppu_frame_release(j->view.frame);
    j->view.frame=frame;j->view.images=j->images;j->view.overlays=j->overlays;
    j->view.image_versions=j->versions;j->view.image_count=image_count;j->view.overlay_count=overlay_count;
    j->view.sources=j->sources;j->view.source_count=source_count;
    j->view.geometry_version+=!same_geometry;j->view.sequence=sequence;
    j->payload_geometry[next]=j->view.geometry_version;
    j->canvas_width=c->canvas_width;j->canvas_height=c->visible_height;
    j->view.blits=NULL;j->blits_valid=false;
    snesrecomp_ppu_frame_release(j->view.graphics_frame);
    j->view.graphics_frame=NULL;j->view.primitives=NULL;j->view.materials=NULL;
    j->view.primitive_count=0;j->view.material_count=0;j->primitives_valid=false;
    j->view.indexed_layout=NULL;j->indexed_valid=false;
    j->active=next;j->valid=true;return SNESRECOMP_PPU_OK;
}
SnesRecompPpuResult snesrecomp_ppu_job_freeze_primitives(SnesRecompPpuJob *j,
    const SnesRecompPpuFrame *frame,const SnesRecompPpuFrame *graphics,
    const SnesRecompPpuJobImage *images,uint16_t image_count,
    const SnesRecompPpuPrimitive *primitives,uint32_t count,
    const SnesRecompPpuMaterial *materials,uint16_t material_count,uint64_t sequence) {
    if(!j) return SNESRECOMP_PPU_INVALID;
    if(j->references!=1) return SNESRECOMP_PPU_BUSY;
    const SnesPpuFrameCapture *base=snesrecomp_ppu_frame_capture(frame);
    const SnesPpuFrameCapture *c=snesrecomp_ppu_frame_capture(graphics);
    if(!base || !c || (count && !primitives) || (material_count && !materials) ||
        (image_count && !images) || base->canvas_width!=c->canvas_width ||
        base->visible_height!=c->visible_height || base->canvas_extra!=c->canvas_extra)
        return SNESRECOMP_PPU_INVALID;
    if(!j->primitive_capacity || count>j->primitive_capacity ||
        material_count>SNESRECOMP_PPU_JOB_MAX_MATERIALS || image_count>SNESRECOMP_PPU_JOB_MAX_IMAGES)
        return SNESRECOMP_PPU_RESOURCE_LIMIT;
    for(unsigned i=0;i<material_count;++i) {
        const SnesRecompPpuMaterial *m=materials+i;
        if(m->reserved || m->flags>SNESRECOMP_PPU_MATERIAL_PALETTE_COLOUR ||
            (m->main_enable&~0x17u) || m->brightness_reduction>15u ||
            (m->occluding_bg!=255u && m->occluding_bg>=3u)) return SNESRECOMP_PPU_INVALID;
    }
    const SnesRecompObjFrame *obj=snesrecomp_ppu_frame_obj(graphics);
    uint16_t line_band[SNES_PPU_MAX_BANDS];
    /* Immutable frames already have contiguous, validated raster bands.
     * Resolve their row ownership once instead of searching every band for
     * each of thousands of UI primitives. Source bounds are checked below. */
    for(unsigned b=0;count && b<c->band_count;++b)
        for(unsigned y=c->bands[b].y_begin;y<c->bands[b].y_end;++y) line_band[y]=(uint16_t)b;
    for(uint32_t i=0;i<count;++i) {
        const SnesRecompPpuPrimitive *r=primitives+i;
        if(r->reserved || r->reserved_byte || r->kind>SNESRECOMP_PPU_PRIMITIVE_OBJ_CAPTURE ||
            r->material>=material_count || !r->width || !r->height ||
            (uint32_t)r->x+r->width>base->canvas_width ||
            (uint32_t)r->y+r->height>base->visible_height) return SNESRECOMP_PPU_INVALID;
        const SnesRecompPpuMaterial *m=materials+r->material;
        if(r->kind==SNESRECOMP_PPU_PRIMITIVE_IMAGE) {
            if(r->source>=image_count || m->flags || m->main_enable || m->occluding_bg!=255u ||
                (uint32_t)r->source_x+r->width>images[r->source].width ||
                (uint32_t)r->source_y+r->height>images[r->source].height)
                return SNESRECOMP_PPU_INVALID;
            continue;
        }
        if((m->flags!=SNESRECOMP_PPU_MATERIAL_SOURCE_COLOUR && m->flags!=SNESRECOMP_PPU_MATERIAL_BASE_COLOUR && m->flags!=SNESRECOMP_PPU_MATERIAL_PALETTE_COLOUR) || m->brightness_reduction ||
            (uint32_t)r->source_x+r->width>c->canvas_width ||
            (uint32_t)r->source_y+r->height>c->visible_height) return SNESRECOMP_PPU_INVALID;
        if((uint32_t)r->source_y+r->height>c->bands[line_band[r->source_y]].y_end)
            return SNESRECOMP_PPU_INVALID;
        if(r->kind==SNESRECOMP_PPU_PRIMITIVE_BG) {
            if(r->source>=3 || !(m->main_enable&(1u<<r->source))) return SNESRECOMP_PPU_INVALID;
        } else if(r->kind==SNESRECOMP_PPU_PRIMITIVE_OBJ_CAPTURE) {
            if(r->source || !c->obj_pixels || !(m->main_enable&16u)) return SNESRECOMP_PPU_INVALID;
        } else {
            if(!obj || r->source>=obj->count || !(m->main_enable&16u))
                return SNESRECOMP_PPU_INVALID;
            const SnesRecompObjSliver *s=obj->slivers+r->source;
            const int left=s->screen_x+c->canvas_extra;
            if(r->owner!=s->oam_slot || r->height!=1 || r->source_y!=s->line ||
                (int)r->source_x<left || (int)r->source_x+r->width>left+8)
                return SNESRECOMP_PPU_INVALID;
        }
    }
    const bool same=j->primitives_valid && j->view.primitive_count==count &&
        j->view.material_count==material_count && j->canvas_width==base->canvas_width &&
        j->canvas_height==base->visible_height &&
        (!count || snesrecomp_u8_equal((const uint8_t *)primitives,(const uint8_t *)j->view.primitives,(size_t)count*sizeof(*primitives))) &&
        (!material_count || snesrecomp_u8_equal((const uint8_t *)materials,(const uint8_t *)j->view.materials,(size_t)material_count*sizeof(*materials)));
    if(!same && j->view.primitive_geometry_version==UINT64_MAX) return SNESRECOMP_PPU_RESOURCE_LIMIT;
    const unsigned next=same?j->primitive_active:j->primitive_active^1u;
    if(!same) {
        if(count) memmove(j->primitives[next],primitives,(size_t)count*sizeof(*primitives));
        if(material_count) memmove(j->materials[next],materials,(size_t)material_count*sizeof(*materials));
    }
    /* Protect an aliased graphics view across ordinary freeze's release. */
    snesrecomp_ppu_frame_retain(graphics);
    const SnesRecompPpuResult result=snesrecomp_ppu_job_freeze(j,frame,images,image_count,NULL,0,sequence);
    if(result!=SNESRECOMP_PPU_OK) {snesrecomp_ppu_frame_release(graphics);return result;}
    j->view.graphics_frame=graphics;j->view.primitives=j->primitives[next];
    j->view.materials=j->materials[next];j->view.primitive_count=count;
    j->view.material_count=material_count;j->view.primitive_geometry_version+=!same;
    j->primitive_active=next;j->primitives_valid=true;
    return SNESRECOMP_PPU_OK;
}
SnesRecompPpuResult snesrecomp_ppu_job_freeze_blits(SnesRecompPpuJob *j,
    const SnesRecompPpuFrame *frame,const SnesRecompBlitPlan *plan,uint64_t sequence) {
    if(!j) return SNESRECOMP_PPU_INVALID;
    if(j->references!=1) return SNESRECOMP_PPU_BUSY;
    const SnesPpuFrameCapture *capture=snesrecomp_ppu_frame_capture(frame);
    if(!capture || !snesrecomp_blit_validate(plan) || plan->width!=capture->canvas_width ||
        plan->height!=capture->visible_height) return SNESRECOMP_PPU_INVALID;
    if(!j->blit_capacity || plan->rect_count>j->blit_capacity) return SNESRECOMP_PPU_RESOURCE_LIMIT;
    const bool same=j->blits_valid && j->blits.width==plan->width && j->blits.height==plan->height &&
        j->blits.image_count==plan->image_count && j->blits.rect_count==plan->rect_count &&
        (!plan->rect_count || !memcmp(j->blits.rects,plan->rects,plan->rect_count*sizeof *plan->rects));
    if(!same && j->view.blit_geometry_version==UINT64_MAX) return SNESRECOMP_PPU_RESOURCE_LIMIT;
    const unsigned next=same?j->blit_active:j->blit_active^1u;
    const uint32_t count=plan->rect_count;
    const uint16_t image_count=plan->image_count,width=plan->width,height=plan->height;
    if(!same && count) memcpy(j->blit_rects[next],plan->rects,count*sizeof *plan->rects);
    const SnesRecompPpuResult result=snesrecomp_ppu_job_freeze(j,frame,plan->images,image_count,NULL,0,sequence);
    if(result!=SNESRECOMP_PPU_OK) return result;
    j->blits=(SnesRecompBlitPlan){j->images,j->blit_rects[next],count,image_count,width,height,0};
    j->blit_active=next;j->blits_valid=true;j->view.blits=&j->blits;
    j->view.blit_geometry_version+=!same;
    return SNESRECOMP_PPU_OK;
}
SnesRecompPpuResult snesrecomp_ppu_job_freeze(SnesRecompPpuJob *j,
    const SnesRecompPpuFrame *frame,const SnesRecompPpuJobImage *images,
    uint16_t image_count,const SnesRecompPpuOverlay *overlays,
    uint16_t overlay_count,uint64_t sequence) {
    return snesrecomp_ppu_job_freeze_sources(j,frame,images,image_count,overlays,
        overlay_count,NULL,0,sequence);
}
const SnesRecompPpuJobView *snesrecomp_ppu_job_view(const SnesRecompPpuJob *j) {
    return j && j->valid && j->view.frame?&j->view:NULL;
}
#include "snes_ppu_indexed_job.inc"
SnesRecompPpuResult snesrecomp_ppu_job_rebind(SnesRecompPpuJob *j,
    const SnesRecompPpuFrame *frame,uint64_t sequence) {
    if(!j) return SNESRECOMP_PPU_INVALID;
    if(j->references!=1) return SNESRECOMP_PPU_BUSY;
    /* The source context cannot silently remain on the preceding frame. */
    if(j->view.primitive_count || j->view.indexed_layout) return SNESRECOMP_PPU_UNSUPPORTED;
    const SnesPpuFrameCapture *next=snesrecomp_ppu_frame_capture(frame);
    const SnesPpuFrameCapture *old=j->valid?snesrecomp_ppu_frame_capture(j->view.frame):NULL;
    if(!next || !old || next->canvas_width!=old->canvas_width ||
        next->visible_height!=old->visible_height) return SNESRECOMP_PPU_INVALID;
    snesrecomp_ppu_frame_retain(frame);snesrecomp_ppu_frame_release(j->view.frame);
    j->view.frame=frame;j->view.sequence=sequence;
    return SNESRECOMP_PPU_OK;
}
static uint32_t Over(uint32_t s,uint32_t d,unsigned blend) {
    if(blend==SNESRECOMP_PPU_OVERLAY_REPLACE) return s|0xff000000u;
    const unsigned a=s>>24;
    if(!a) return d;
    if(a==255) return s;
    uint32_t out=0xff000000u;
    for(unsigned shift=0;shift<24;shift+=8) {
        unsigned value=((s>>shift)&255u)*(blend==SNESRECOMP_PPU_OVERLAY_STRAIGHT?a:255u);
        value+=((d>>shift)&255u)*(255u-a);
        out|=((value+127u)/255u)<<shift;
    }
    return out;
}
SnesRecompPpuResult snesrecomp_ppu_job_apply(const SnesRecompPpuJob *j,
    uint32_t *argb,uint16_t width,uint16_t height,uint32_t pitch) {
    const SnesRecompPpuJobView *v=snesrecomp_ppu_job_view(j);
    const SnesPpuFrameCapture *c=v?snesrecomp_ppu_frame_capture(v->frame):NULL;
    if(!c || !argb || width!=c->canvas_width || height!=c->visible_height || pitch<width ||
        pitch>SIZE_MAX/sizeof(uint32_t)/height) return SNESRECOMP_PPU_INVALID;
    /* A finished image has lost its individual BG planes. Applying sources
     * here would silently put art above sprites and bypass colour math. */
    if(v->source_count || v->primitive_count || v->indexed_layout) return SNESRECOMP_PPU_UNSUPPORTED;
    const uintptr_t begin=(uintptr_t)argb;
    const size_t bytes=((size_t)(height-1)*pitch+width)*sizeof(uint32_t);
    if(begin>UINTPTR_MAX-bytes) return SNESRECOMP_PPU_INVALID;
    for(unsigned i=0;i<v->image_count;++i) {
        const uintptr_t image=(uintptr_t)v->images[i].pixels;
        const size_t size=(size_t)v->images[i].width*v->images[i].height*sizeof(uint32_t);
        if(begin<image+size && image<begin+bytes) return SNESRECOMP_PPU_INVALID;
    }
    for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x)
        if(argb[(size_t)y*pitch+x]>>24!=255u) return SNESRECOMP_PPU_UNSUPPORTED;
    if(v->blits) return snesrecomp_blit_apply(v->blits,argb,pitch)?SNESRECOMP_PPU_OK:SNESRECOMP_PPU_INVALID;
    for(unsigned i=0;i<v->overlay_count;++i) {
        const SnesRecompPpuOverlay *r=v->overlays+i;
        const SnesRecompPpuJobImage *image=v->images+r->image;
        for(unsigned y=0;y<r->height;++y) for(unsigned x=0;x<r->width;++x) {
            uint32_t *d=argb+(size_t)(r->y+y)*pitch+r->x+x;
            if(r->condition==SNESRECOMP_PPU_OVERLAY_KEY_OR_BLACK && *d!=r->key_argb && (*d&0xffffffu)) continue;
            uint32_t source=image->pixels[(size_t)(r->source_y+y)*image->pitch+r->source_x+x];
            if(r->brightness_reduction) {
                uint32_t shaded=source&0xff000000u;
                for(unsigned shift=0;shift<24;shift+=8)
                    shaded|=(((source>>shift)&255u)*(15u-r->brightness_reduction)/15u)<<shift;
                source=shaded;
            }
            *d=Over(source,*d,r->blend);
        }
    }
    return SNESRECOMP_PPU_OK;
}
