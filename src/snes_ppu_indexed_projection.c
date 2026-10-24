#include "snesrecomp_platform/snes_ppu_indexed_projection.h"
#include <stdlib.h>
#include <string.h>
struct SnesRecompIndexedProjection {
    SnesRecompIndexedProjectionRect *rects;
    SnesRecompPpuIndexedObject *objects;
    SnesRecompPpuAxisRun *axes;
    uint64_t generation,owner,geometry_version;
    uint32_t capacity,count,object_count,axis_count;
    uint16_t width,height;
    uint8_t owner_planes;
    bool valid;
};
SnesRecompIndexedProjection *snesrecomp_ppu_indexed_projection_create(uint32_t capacity) {
    if(!capacity || capacity>SNESRECOMP_PPU_JOB_MAX_PRIMITIVES) return NULL;
    SnesRecompIndexedProjection *p=calloc(1,sizeof *p);if(!p) return NULL;
    p->rects=malloc((size_t)capacity*sizeof *p->rects);
    p->objects=malloc(SNESRECOMP_PPU_INDEXED_MAX_OBJECTS*sizeof *p->objects);
    p->axes=malloc(SNESRECOMP_PPU_INDEXED_MAX_AXES*sizeof *p->axes);
    if(!p->rects || !p->objects || !p->axes) {snesrecomp_ppu_indexed_projection_destroy(p);return NULL;}
    p->capacity=capacity;return p;
}
void snesrecomp_ppu_indexed_projection_destroy(SnesRecompIndexedProjection *p) {
    if(!p) return;
    free(p->rects);free(p->objects);free(p->axes);free(p);
}
void snesrecomp_ppu_indexed_projection_reset(SnesRecompIndexedProjection *p) {
    if(p) p->valid=false;
}
SnesRecompPpuResult snesrecomp_ppu_indexed_projection_prepare(
    SnesRecompIndexedProjection *p,const SnesRecompPpuJob *job,SnesRecompIndexedProjectionPlan *out) {
    if(!out) return SNESRECOMP_PPU_INVALID;
    memset(out,0,sizeof *out);
    const SnesRecompPpuJobView *v=snesrecomp_ppu_job_view(job);
    if(!p || !v || !v->indexed_layout) return SNESRECOMP_PPU_INVALID;
    const SnesRecompPpuIndexedLayout *l=v->indexed_layout;
    if(p->valid && p->owner==v->owner && p->geometry_version==v->indexed_geometry_version) {
        *out=(SnesRecompIndexedProjectionPlan){p->rects,p->generation,p->count,1};return SNESRECOMP_PPU_OK;
    }
    /* Equality is content-based: unrelated/recycled job pointers can have the
     * same local geometry version. Claims and OAM remain separate GPU masks. */
    const bool same=p->valid && p->width==l->width && p->height==l->height &&
        p->owner_planes==l->owner_planes && p->object_count==l->object_count && p->axis_count==l->axis_count &&
        (!l->object_count || !memcmp(p->objects,l->objects,l->object_count*sizeof *l->objects)) &&
        (!l->axis_count || !memcmp(p->axes,l->axes,l->axis_count*sizeof *l->axes));
    if(same) {
        p->owner=v->owner;p->geometry_version=v->indexed_geometry_version;
        *out=(SnesRecompIndexedProjectionPlan){p->rects,p->generation,p->count,1};return SNESRECOMP_PPU_OK;
    }
    uint64_t count=0;
    for(unsigned i=0;i<l->object_count;++i)
        count+=(uint32_t)l->objects[i].x_count*l->objects[i].y_count;
    if(count>p->capacity || p->generation==UINT64_MAX) return SNESRECOMP_PPU_RESOURCE_LIMIT;
    unsigned used=0;
    for(unsigned i=0;i<l->object_count;++i) {
        const SnesRecompPpuIndexedObject *o=l->objects+i;
        for(unsigned y=0;y<o->y_count;++y) for(unsigned x=0;x<o->x_count;++x) {
            const SnesRecompPpuAxisRun *xr=l->axes+o->x_first+x,*yr=l->axes+o->y_first+y;
            p->rects[used++]=(SnesRecompIndexedProjectionRect){
                (uint16_t)(o->x+xr->destination),(uint16_t)(o->y+yr->destination),
                (uint16_t)(o->x+xr->destination+xr->count),(uint16_t)(o->y+yr->destination+yr->count),
                (uint16_t)(o->source_x+xr->source),(uint16_t)(o->source_y+yr->source),
                o->owner,(uint16_t)i,o->plane,xr->step,yr->step,(uint8_t)((l->owner_planes>>o->plane)&1u)};
        }
    }
    if(l->object_count) memcpy(p->objects,l->objects,l->object_count*sizeof *l->objects);
    if(l->axis_count) memcpy(p->axes,l->axes,l->axis_count*sizeof *l->axes);
    p->width=l->width;p->height=l->height;p->owner_planes=l->owner_planes;
    p->owner=v->owner;p->geometry_version=v->indexed_geometry_version;
    p->object_count=l->object_count;p->axis_count=l->axis_count;p->count=used;p->valid=true;++p->generation;
    *out=(SnesRecompIndexedProjectionPlan){p->rects,p->generation,p->count,0};return SNESRECOMP_PPU_OK;
}
static bool ClipAxis(unsigned source,unsigned extent,unsigned step,unsigned first,unsigned end,
    unsigned *begin,unsigned *finish) {
    if(!step) {*begin=0;*finish=extent;return source>=first && source<end;}
    *begin=first>source?first-source:0;
    *finish=end>source?end-source:0;
    if(*finish>extent) *finish=extent;
    return *begin<*finish;
}
int32_t snesrecomp_ppu_indexed_projection_clip(const SnesRecompIndexedProjectionRect *rect,
    uint16_t sx0,uint16_t sy0,uint16_t sx1,uint16_t sy1,SnesRecompIndexedProjectionRect *out) {
    if(!rect || !out) return -1;
    SnesRecompIndexedProjectionRect q=*rect;
    if(sx0>=sx1 || sy0>=sy1 || sx1>512 || sy1>256 || q.x0>=q.x1 || q.y0>=q.y1 ||
        q.x1>512 || q.y1>256 || q.x_step>1 || q.y_step>1 || q.owner_required>1 || q.plane>=5 ||
        (uint32_t)q.source_x+(q.x_step?(uint32_t)(q.x1-q.x0):1u)>512 ||
        (uint32_t)q.source_y+(q.y_step?(uint32_t)(q.y1-q.y0):1u)>256) return -1;
    unsigned x0,x1,y0,y1;
    if(!ClipAxis(q.source_x,q.x1-q.x0,q.x_step,sx0,sx1,&x0,&x1) ||
        !ClipAxis(q.source_y,q.y1-q.y0,q.y_step,sy0,sy1,&y0,&y1)) return 0;
    q.x1=(uint16_t)(q.x0+x1);q.y1=(uint16_t)(q.y0+y1);
    q.x0=(uint16_t)(q.x0+x0);q.y0=(uint16_t)(q.y0+y0);
    q.source_x=(uint16_t)(q.source_x+x0*q.x_step);q.source_y=(uint16_t)(q.source_y+y0*q.y_step);
    *out=q;return 1;
}
