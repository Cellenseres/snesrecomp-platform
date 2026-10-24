#include "snesrecomp_platform/snes_ppu_indexed_brightness.h"
#include <stdlib.h>
#include <string.h>
struct SnesRecompIndexedBrightness {
    SnesRecompPpuFrame *frames[2];
    SnesRecompPpuJob *job;
    SnesPpuRasterBand bands[SNES_PPU_MAX_BANDS];
};
void snesrecomp_ppu_indexed_brightness_reset(SnesRecompIndexedBrightness *c) {
    if(!c)return;
    snesrecomp_ppu_job_detach_frame(c->job);
    for(unsigned i=0;i<2;++i)snesrecomp_ppu_frame_discard_raster_view(c->frames[i]);
}
void snesrecomp_ppu_indexed_brightness_destroy(SnesRecompIndexedBrightness *c) {
    if(!c)return;
    snesrecomp_ppu_job_release(c->job);
    for(unsigned i=0;i<2;++i)snesrecomp_ppu_frame_release(c->frames[i]);
    free(c);
}
SnesRecompIndexedBrightness *snesrecomp_ppu_indexed_brightness_create(void) {
    SnesRecompIndexedBrightness *c=calloc(1,sizeof *c);
    if(!c)return NULL;
    c->job=snesrecomp_ppu_job_create_indexed(SNESRECOMP_PPU_INDEXED_MAX_CLAIMS,
        SNESRECOMP_PPU_INDEXED_MAX_OBJECTS,SNESRECOMP_PPU_INDEXED_MAX_AXES);
    for(unsigned i=0;i<2;++i)c->frames[i]=snesrecomp_ppu_frame_create();
    if(!c->job || !c->frames[0] || !c->frames[1]) {
        snesrecomp_ppu_indexed_brightness_destroy(c);return NULL;
    }
    return c;
}
const SnesRecompPpuJob *snesrecomp_ppu_indexed_brightness_prepare(
    SnesRecompIndexedBrightness *c,const SnesRecompPpuJob *source,uint8_t *brightness) {
    if(!c || !brightness)return NULL;
    snesrecomp_ppu_indexed_brightness_reset(c);
    const SnesRecompPpuJobView *v=snesrecomp_ppu_job_view(source);
    if(!v || !v->indexed_layout || v->image_count || v->source_count ||
       v->overlay_count || v->primitive_count || (v->blits && v->blits->rect_count))return NULL;
    const SnesRecompPpuFrame *frames[]={v->frame,v->graphics_frame};
    const SnesPpuFrameCapture *captures[2];unsigned b=16;
    for(unsigned f=0;f<2;++f) {
        const SnesPpuFrameCapture *a=captures[f]=snesrecomp_ppu_frame_capture(frames[f]);
        if(!a || !a->band_count || a->band_count>SNES_PPU_MAX_BANDS || a->raster_memory_flags)return NULL;
        for(unsigned i=0;i<a->band_count;++i) {
            const SnesPpuRasterBand *r=a->bands+i;
            if(r->forced_blank || r->brightness>15 || (b!=16 && r->brightness!=b))return NULL;
            b=r->brightness;
        }
    }
    /* An independently reduced material is not uniform final brightness. */
    for(unsigned i=0;i<v->indexed_layout->material_count;++i)
        if(v->indexed_layout->materials[i].brightness_reduction)return NULL;
    for(unsigned f=0;f<2;++f) {
        const SnesPpuFrameCapture *a=captures[f];
        memcpy(c->bands,a->bands,a->band_count*sizeof *c->bands);
        for(unsigned i=0;i<a->band_count;++i) {
            c->bands[i].brightness=15;
            c->bands[i].regs[0]=(c->bands[i].regs[0]&0xf0u)|15u;
        }
        if(snesrecomp_ppu_frame_freeze_raster_view(c->frames[f],frames[f],c->bands,
            a->band_count,v->sequence)!=SNESRECOMP_PPU_OK)goto failure;
    }
    if(snesrecomp_ppu_job_freeze_indexed(c->job,c->frames[0],c->frames[1],
        v->indexed_layout,v->sequence)!=SNESRECOMP_PPU_OK)goto failure;
    *brightness=(uint8_t)b;return c->job;
failure:
    snesrecomp_ppu_indexed_brightness_reset(c);return NULL;
}
