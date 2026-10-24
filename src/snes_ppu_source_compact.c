#include "snesrecomp_platform/snes_ppu_source_compact.h"
#include <string.h>

static bool Overlap(const void *a,size_t na,const void *b,size_t nb) {
    const uintptr_t pa=(uintptr_t)a,pb=(uintptr_t)b;
    return na && nb && (pa<=pb?pb-pa<na:pa-pb<nb);
}

SnesRecompPpuResult snesrecomp_ppu_source_image_compact(
    const SnesRecompPpuJobImage *image,uint16_t image_index,
    const SnesRecompPpuSource *sources,uint16_t source_count,
    uint32_t *packed,uint32_t pixel_capacity,uint32_t flags,
    SnesRecompPpuJobImage *output_image,SnesRecompPpuSource *output_sources) {
    if(!image || !sources || !packed || !output_image || !output_sources ||
        !image->pixels || !image->width || image->width>SNESRECOMP_PPU_MAX_WIDTH ||
        !image->height || image->height>SNES_PPU_MAX_BANDS || image->pitch<image->width ||
        image_index>=SNESRECOMP_PPU_JOB_MAX_IMAGES || !source_count ||
        source_count>SNESRECOMP_PPU_JOB_MAX_SOURCES ||
        flags&~SNESRECOMP_PPU_SOURCE_COMPACT_EDGE_TILES) return SNESRECOMP_PPU_INVALID;
    const SnesRecompPpuJobImage input=*image;
    const uint64_t span=((uint64_t)(input.height-1u)*input.pitch+input.width)*sizeof(uint32_t);
    if(span>SIZE_MAX || span>UINTPTR_MAX-(uintptr_t)input.pixels) return SNESRECOMP_PPU_INVALID;
    uint8_t xs[SNESRECOMP_PPU_MAX_WIDTH]={0},ys[SNES_PPU_MAX_BANDS]={0};
    uint16_t xmap[SNESRECOMP_PPU_MAX_WIDTH],ymap[SNES_PPU_MAX_BANDS];
    for(unsigned i=0;i<source_count;++i) {
        const SnesRecompPpuSource *r=sources+i;
        if(r->image!=image_index) continue;
        if(!r->width || !r->height || !r->source_width || !r->source_height ||
            (r->source_width!=1 && r->source_width!=r->width) ||
            (r->source_height!=1 && r->source_height!=r->height) ||
            (uint32_t)r->source_x+r->source_width>input.width ||
            (uint32_t)r->source_y+r->source_height>input.height) return SNESRECOMP_PPU_INVALID;
        memset(xs+r->source_x,1,r->source_width);memset(ys+r->source_y,1,r->source_height);
    }
    if(flags&SNESRECOMP_PPU_SOURCE_COMPACT_EDGE_TILES) {
        unsigned left=0,right=input.width;
        const unsigned middle=input.width/2u;
        for(unsigned x=0;x<middle;++x) if(xs[x]) left=x+1u;
        for(unsigned x=input.width;x>middle;--x) if(xs[x-1u]) right=x-1u;
        left=(left+7u)&~7u;if(right<input.width) right&=~7u;
        if(left>input.width) left=input.width;
        if(left) memset(xs,1,left);
        if(right<input.width) memset(xs+right,1,input.width-right);
        bool any=false;for(unsigned y=0;y<input.height;++y) any|=ys[y]!=0;
        if(any) memset(ys,1,input.height);
    }
    unsigned width=0,height=0;
    for(unsigned x=0;x<input.width;++x) {xmap[x]=(uint16_t)width;width+=xs[x];}
    for(unsigned y=0;y<input.height;++y) {ymap[y]=(uint16_t)height;height+=ys[y];}
    if(!width || !height || (width==input.width && height==input.height)) return SNESRECOMP_PPU_UNSUPPORTED;
    if(width*height>pixel_capacity) return SNESRECOMP_PPU_RESOURCE_LIMIT;
    const size_t bytes=(size_t)width*height*sizeof(uint32_t);
    const size_t records=(size_t)source_count*sizeof *sources;
    if(bytes>UINTPTR_MAX-(uintptr_t)packed ||
        Overlap(packed,bytes,input.pixels,(size_t)span) ||
        Overlap(packed,bytes,sources,records) || Overlap(packed,bytes,output_sources,records) ||
        Overlap(packed,bytes,output_image,sizeof *output_image) ||
        Overlap(output_image,sizeof *output_image,sources,records) ||
        Overlap(output_image,sizeof *output_image,output_sources,records) ||
        Overlap(output_image,sizeof *output_image,input.pixels,(size_t)span) ||
        Overlap(output_sources,records,input.pixels,(size_t)span) ||
        (output_sources!=sources && Overlap(output_sources,records,sources,records))) return SNESRECOMP_PPU_INVALID;
    struct Run {uint16_t begin,length;} runs[(SNESRECOMP_PPU_MAX_WIDTH+1)/2];
    unsigned count=0;
    for(unsigned x=0;x<input.width;) {
        if(!xs[x]) {++x;continue;}
        const unsigned begin=x;while(x<input.width && xs[x]) ++x;
        runs[count++]=(struct Run){(uint16_t)begin,(uint16_t)(x-begin)};
    }
    for(unsigned y=0;y<input.height;++y) if(ys[y]) for(unsigned run=0;run<count;++run) {
        const unsigned x=runs[run].begin;
        memcpy(packed+(size_t)ymap[y]*width+xmap[x],input.pixels+(size_t)y*input.pitch+x,
            (size_t)runs[run].length*sizeof(uint32_t));
    }
    for(unsigned i=0;i<source_count;++i) {
        SnesRecompPpuSource r=sources[i];
        if(r.image==image_index) {r.source_x=xmap[r.source_x];r.source_y=ymap[r.source_y];}
        output_sources[i]=r;
    }
    *output_image=(SnesRecompPpuJobImage){packed,width,(uint16_t)width,(uint16_t)height};
    return SNESRECOMP_PPU_OK;
}
