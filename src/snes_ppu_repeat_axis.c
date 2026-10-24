#include "snesrecomp_platform/snes_ppu_repeat_axis.h"
#include <stdbool.h>
static SnesRecompPpuAxisRun Next(uint32_t source,uint32_t target,uint32_t x,uint32_t end) {
    const uint32_t edge=source<32u?source/2u:16u;
    SnesRecompPpuAxisRun run={(uint16_t)x,0,0,1,0};
    uint32_t n;
    if(x<edge) {run.source=(uint16_t)x;n=edge-x;}
    else if(x>=target-edge) {run.source=(uint16_t)(source-(target-x));n=end-x;}
    else {
        const uint32_t body=source>edge*2u?source-edge*2u:1u;
        const uint32_t offset=(x-edge)%body;
        run.source=(uint16_t)(edge+offset);
        n=target-edge-x;
        if(body==1u) run.step=0;
        else if(n>body-offset) n=body-offset;
    }
    if(n>end-x) n=end-x;
    run.count=(uint16_t)n;return run;
}
static bool Join(const SnesRecompPpuAxisRun *a,const SnesRecompPpuAxisRun *b) {
    return a->step==b->step && a->destination+a->count==b->destination &&
        a->source+(a->step?a->count:0u)==b->source;
}
int32_t snesrecomp_ppu_repeat_axis(uint32_t source,uint32_t target,uint32_t begin,
    uint32_t end,SnesRecompPpuAxisRun *output,uint32_t capacity,uint32_t *count) {
    if(!count || !source || source>UINT16_MAX || !target || target>UINT16_MAX ||
        begin>end || end>target || (!output && capacity)) return SNESRECOMP_AXIS_INVALID;
    /* Count first: an allocation/budget refusal cannot leave half a plan. */
    uint32_t needed=0;SnesRecompPpuAxisRun previous={0};
    for(uint32_t x=begin;x<end;) {
        SnesRecompPpuAxisRun run=Next(source,target,x,end);
        if(needed && Join(&previous,&run)) previous.count=(uint16_t)(previous.count+run.count);
        else {++needed;previous=run;}
        x+=run.count;
    }
    *count=needed;
    if(needed>capacity) return SNESRECOMP_AXIS_CAPACITY;
    needed=0;
    for(uint32_t x=begin;x<end;) {
        const SnesRecompPpuAxisRun run=Next(source,target,x,end);
        if(needed && Join(output+needed-1u,&run)) output[needed-1u].count+=run.count;
        else output[needed++]=run;
        x+=run.count;
    }
    return SNESRECOMP_AXIS_OK;
}
