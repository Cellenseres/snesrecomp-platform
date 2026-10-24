#include "snesrecomp_platform/snes_ppu_obj_run.h"
#include "snesrecomp_platform/snes_bitplanes.h"
#include <stddef.h>
static bool Disjoint(const void *a,size_t an,const void *b,size_t bn) {
    const uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x<=UINTPTR_MAX-an && y<=UINTPTR_MAX-bn && (x+an<=y || y+bn<=x);
}
bool snesrecomp_ppu_obj_run_row(const uint16_t *vram,
    const SnesRecompPpuObjRun *runs,uint32_t count,uint16_t line,
    uint16_t width,uint16_t extra,uint16_t *native,uint16_t *overlay,uint16_t *owners) {
    if(!vram || (!runs && count) || !native || !overlay || line>=256 || width<256 ||
        width>SNESRECOMP_OBJ_RUN_MAX_ROW_WIDTH || extra>width-256u || count>SNESRECOMP_OBJ_MAX_SLIVERS) return false;
    const size_t bytes=(size_t)width*sizeof(uint16_t),run_bytes=(size_t)count*sizeof(*runs);
    const uint16_t *outputs[]={native,overlay,owners};
    for(unsigned i=0;i<3;++i) if(outputs[i]) {
        if(!Disjoint(outputs[i],bytes,vram,32768u*sizeof(uint16_t)) ||
            (count && !Disjoint(outputs[i],bytes,runs,run_bytes))) return false;
        for(unsigned k=0;k<i;++k) if(outputs[k] && !Disjoint(outputs[i],bytes,outputs[k],bytes)) return false;
    }
    for(unsigned i=0;i<count;++i) {
        const SnesRecompPpuObjRun *r=runs+i;const SnesRecompObjSliver *s=&r->sliver;
        if(s->line!=line || s->tile>=2048 || s->row>7 || s->priority>3 || s->flip_h>1 ||
            s->oam_slot>=128 || s->palette_base<128 || (s->palette_base&15) ||
            s->math_eligible!=(s->palette_base>=192) || r->reserved || r->flags>7 ||
            r->begin>r->end || r->end>8 || r->overlay_begin<r->begin ||
            r->overlay_begin>r->overlay_end || r->overlay_end>r->end ||
            ((r->flags&SNESRECOMP_OBJ_RUN_REMOVE_OVERLAY) && !(r->flags&SNESRECOMP_OBJ_RUN_OVERLAY)) ||
            (!(r->flags&SNESRECOMP_OBJ_RUN_OVERLAY) && r->overlay_begin!=r->overlay_end)) return false;
    }
    for(unsigned x=0;x<width;++x) {native[x]=overlay[x]=0x0500;if(owners) owners[x]=255;}
    for(unsigned i=0;i<count;++i) {
        const SnesRecompPpuObjRun *r=runs+i;const SnesRecompObjSliver *s=&r->sliver;
        const unsigned address=(unsigned)s->tile*16u+s->row;
        const uint32_t bits=snesrecomp_ppu_unpack_4bpp((uint32_t)vram[address]|(uint32_t)vram[address+8u]<<16);
        const uint16_t base=(uint16_t)(((s->priority*4u+2u)<<12)|
            ((s->math_eligible?4u:6u)<<8)|s->palette_base);
        for(unsigned px=r->begin;px<r->end;++px) {
            const int x=(int)s->screen_x+(int)px+extra;
            if(x<0 || x>=width) continue;
            const unsigned pixel=(bits>>(4u*(s->flip_h?px:7u-px)))&15u;
            if(!pixel) continue;
            const bool captured=(r->flags&SNESRECOMP_OBJ_RUN_OVERLAY) && px>=r->overlay_begin && px<r->overlay_end;
            const uint16_t z=(uint16_t)(base+pixel);
            if(captured) {overlay[x]=z;if(owners) owners[x]=r->owner;}
            if(!captured || !(r->flags&SNESRECOMP_OBJ_RUN_REMOVE_OVERLAY)) native[x]=z;
        }
    }
    return true;
}
