#ifndef SNESRECOMP_PLATFORM_MODE7_CELLS_H
#define SNESRECOMP_PLATFORM_MODE7_CELLS_H
#include "snesrecomp_platform/snes_ppu_mode7_world_types.h"
#include <stdbool.h>
#include <string.h>

/* One bit per 8x8 cell of a 1024x1024 world. Inputs have passed the native span compiler's
 * coordinate, step and span bounds. Affine samples lie between the first and
 * last sample; one additional texel on each side covers nearest-filter edge
 * quantization. Solid RGB spans still sample texture alpha for coverage. */
typedef struct SnesRecompMode7Cells {
    uint32_t words[512];
    bool full;
} SnesRecompMode7Cells;

static inline bool snesrecomp_mode7_cell_used(const SnesRecompMode7Cells *cells,unsigned cell) {
    return (cells->words[cell >> 5] & (UINT32_C(1) << (cell & 31))) != 0;
}
static inline void snesrecomp_mode7_cells_rect(SnesRecompMode7Cells *cells,
                                     int x0,int x1,int y0,int y1) {
    if(cells->full) return;
    /* Broad perspective spans deliberately admit the whole world rather than
     * repeatedly walking a large rectangle. This bounds preparation work. */
    if((x1-x0+1)*(y1-y0+1)>=4096) {
        memset(cells->words,255,sizeof cells->words);cells->full=true;return;
    }
    const unsigned first=(unsigned)x0>>5,last=(unsigned)x1>>5;
    const uint32_t left=UINT32_MAX<<((unsigned)x0&31u);
    const uint32_t right=UINT32_MAX>>(31u-((unsigned)x1&31u));
    for(int y=y0;y<=y1;++y) {
        uint32_t *row=cells->words+(unsigned)y*4u;
        if(first==last) row[first]|=left&right;
        else {
            row[first]|=left;row[last]|=right;
            for(unsigned word=first+1;word<last;++word) row[word]=UINT32_MAX;
        }
    }
}
static inline void snesrecomp_mode7_cells_add(SnesRecompMode7Cells *cells,
                                    const SnesRecompMode7WorldSpan *s) {
    if(cells->full) return;
    const int n=(int)s->x1-s->x0-1;
    const int ex=s->start_x+s->step_x*n,ey=s->start_y+s->step_y*n;
    const int tx0=(s->start_x<ex?s->start_x:ex)>>8;
    const int tx1=(s->start_x>ex?s->start_x:ex)>>8;
    const int ty0=(s->start_y<ey?s->start_y:ey)>>8;
    const int ty1=(s->start_y>ey?s->start_y:ey)>>8;
    int xr[3][2]={{(tx0?tx0-1:0)>>3,(tx1<1023?tx1+1:1023)>>3}};
    int yr[3][2]={{(ty0?ty0-1:0)>>3,(ty1<1023?ty1+1:1023)>>3}};
    unsigned nx=1,ny=1;
    /* World textures repeat. Conservative padding at a seam must also mark
     * the opposite edge, including the corner when both axes wrap. */
    if(!tx0) {xr[nx][0]=xr[nx][1]=127;++nx;}
    if(tx1==1023) {xr[nx][0]=xr[nx][1]=0;++nx;}
    if(!ty0) {yr[ny][0]=yr[ny][1]=127;++ny;}
    if(ty1==1023) {yr[ny][0]=yr[ny][1]=0;++ny;}
    for(unsigned y=0;y<ny;++y) for(unsigned x=0;x<nx;++x)
        snesrecomp_mode7_cells_rect(cells,xr[x][0],xr[x][1],yr[y][0],yr[y][1]);
}
#endif
