#include "snesrecomp_platform/snes_ppu_obj_geometry_plan.h"
#include <stddef.h>
#include <string.h>

static bool Disjoint(const void *a,size_t an,const void *b,size_t bn) {
    const uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x<=UINTPTR_MAX-an && y<=UINTPTR_MAX-bn && (x+an<=y || y+bn<=x);
}
static bool RunValid(const SnesRecompPpuObjRun *r,unsigned height) {
    const SnesRecompObjSliver *s=&r->sliver;
    return s->line<height && s->tile<2048 && s->row<8 && s->priority<4 &&
        s->flip_h<2 && s->oam_slot<128 && s->palette_base>=128 &&
        !(s->palette_base&15) && s->math_eligible==(s->palette_base>=192) &&
        !r->reserved && !(r->flags&~7u) && r->begin<=r->end && r->end<=8 &&
        r->overlay_begin>=r->begin && r->overlay_begin<=r->overlay_end &&
        r->overlay_end<=r->end &&
        (!(r->flags&SNESRECOMP_OBJ_RUN_REMOVE_OVERLAY) || (r->flags&SNESRECOMP_OBJ_RUN_OVERLAY)) &&
        ((r->flags&SNESRECOMP_OBJ_RUN_OVERLAY) || r->overlay_begin==r->overlay_end);
}
static unsigned Segments(const SnesRecompPpuObjRun *r,unsigned begin[2],unsigned end[2]) {
    begin[0]=r->begin;end[0]=r->end;
    if(!(r->flags&SNESRECOMP_OBJ_RUN_REMOVE_OVERLAY))return 1;
    end[0]=r->overlay_begin;begin[1]=r->overlay_end;end[1]=r->end;return 2;
}
static bool Quad(const SnesRecompPpuObjRun *r,unsigned run,unsigned winner,
    unsigned begin,unsigned end,unsigned width,unsigned extra,int clip0,int clip1,
    SnesRecompPpuObjGeometryQuad *out) {
    const int origin=(int)r->sliver.screen_x+(int)extra;
    int x0=origin+(int)begin,x1=origin+(int)end;
    if(clip0<0)clip0=0;
    if(clip1>(int)width)clip1=(int)width;
    if(x0<clip0)x0=clip0;
    if(x1>clip1)x1=clip1;
    if(x0>=x1)return false;
    const unsigned u0=(unsigned)(x0-origin),u1=(unsigned)(x1-origin);
    if(out)*out=(SnesRecompPpuObjGeometryQuad){
        .x0=(int16_t)x0,.x1=(int16_t)x1,.line=r->sliver.line,.tile=r->sliver.tile,
        .u0=(uint8_t)(r->sliver.flip_h?8u-u0:u0),
        .u1=(uint8_t)(r->sliver.flip_h?8u-u1:u1),.row=r->sliver.row,
        .palette_base=r->sliver.palette_base,.priority=r->sliver.priority,
        .math_eligible=r->sliver.math_eligible,.winner=(uint8_t)winner,.run=run};
    return true;
}
SnesRecompPpuResult snesrecomp_ppu_obj_geometry_plan(
    const SnesRecompPpuObjRun *runs,uint32_t count,const SnesRecompPpuObjGeometryClip *clips,
    uint16_t width,uint16_t height,uint16_t extra,
    SnesRecompPpuObjGeometryQuad *winners,uint32_t winner_capacity,
    SnesRecompPpuObjGeometryQuad *colours,uint32_t colour_capacity,SnesRecompPpuObjGeometryPlan *out) {
    if(!out || (!runs && count) || (!winners && winner_capacity) || (!colours && colour_capacity) ||
        count>SNESRECOMP_OBJ_MAX_SLIVERS || width<256 || width>SNESRECOMP_PPU_MAX_WIDTH ||
        !height || height>SNES_PPU_MAX_BANDS || extra>width-256u ||
        winner_capacity>2u*SNESRECOMP_OBJ_MAX_SLIVERS || colour_capacity>2u*SNESRECOMP_OBJ_MAX_SLIVERS)
        return SNESRECOMP_PPU_INVALID;
    const void *inputs[]={runs,clips,out};
    const size_t input_bytes[]={(size_t)count*sizeof *runs,clips?(size_t)count*sizeof *clips:0,sizeof *out};
    const size_t wb=(size_t)winner_capacity*sizeof *winners,cb=(size_t)colour_capacity*sizeof *colours;
    if(wb && cb && !Disjoint(winners,wb,colours,cb))return SNESRECOMP_PPU_INVALID;
    for(unsigned k=0;k<3;++k)if(input_bytes[k]) {
        if((wb && !Disjoint(winners,wb,inputs[k],input_bytes[k])) ||
           (cb && !Disjoint(colours,cb,inputs[k],input_bytes[k])))return SNESRECOMP_PPU_INVALID;
    }
    if(count && !Disjoint(out,sizeof *out,runs,input_bytes[0]))return SNESRECOMP_PPU_INVALID;
    if(clips && count && !Disjoint(out,sizeof *out,clips,input_bytes[1]))return SNESRECOMP_PPU_INVALID;
    SnesRecompPpuObjGeometryPlan plan={.width=width,.height=height};
    /* The first pass validates/counts everything; no caller data is changed. */
    for(unsigned pass=0;pass<2;++pass) {
        unsigned line=UINT16_MAX,last_slot=128,winner=0;
        uint32_t seen[4]={0};int previous_end=0;
        unsigned wi=0,ci=0;
        for(unsigned i=0;i<count;++i) {
            const SnesRecompPpuObjRun *r=runs+i;
            if(!pass && (!RunValid(r,height) || (clips && clips[i].x0>clips[i].x1)))return SNESRECOMP_PPU_INVALID;
            if(r->sliver.line!=line) {
                if(!pass && line!=UINT16_MAX && r->sliver.line<line)return SNESRECOMP_PPU_INVALID;
                line=r->sliver.line;last_slot=128;winner=0;memset(seen,0,sizeof seen);
            }
            if(r->sliver.oam_slot!=last_slot) {
                const unsigned slot=r->sliver.oam_slot;
                if(!pass && (seen[slot>>5]&(UINT32_C(1)<<(slot&31u))))return SNESRECOMP_PPU_INVALID;
                seen[slot>>5]|=UINT32_C(1)<<(slot&31u);last_slot=slot;previous_end=-32768;
                ++winner;if(winner>plan.winner_groups)plan.winner_groups=(uint8_t)winner;
            }
            const int begin=(int)r->sliver.screen_x+r->begin,end=(int)r->sliver.screen_x+r->end;
            if(!pass && begin<end && begin<previous_end)return SNESRECOMP_PPU_INVALID;
            if(begin<end)previous_end=end;
            unsigned a[2],z[2],n=Segments(r,a,z);
            for(unsigned segment=0;segment<n;++segment) {
                if(Quad(r,i,winner,a[segment],z[segment],width,extra,0,width,pass && winners?winners+wi:NULL))++wi;
                if(Quad(r,i,winner,a[segment],z[segment],width,extra,
                    clips?clips[i].x0:0,clips?clips[i].x1:width,pass && colours?colours+ci:NULL))++ci;
            }
        }
        if(!pass) {
            if(wi>winner_capacity || ci>colour_capacity)return SNESRECOMP_PPU_RESOURCE_LIMIT;
            plan.winner_count=wi;plan.colour_count=ci;
        }
    }
    *out=plan;return SNESRECOMP_PPU_OK;
}
