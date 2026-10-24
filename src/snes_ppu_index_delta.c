#include "snesrecomp_platform/snes_ppu_index_delta.h"
#include "snesrecomp_platform/memory_compare.h"
#include "snesrecomp_platform/snes_ppu_capture.h"
static bool Dimensions(size_t previous_pitch,size_t current_pitch,unsigned width,unsigned height) {
    return width && width<=512u && height && height<=SNES_PPU_MAX_BANDS &&
        previous_pitch>=width && current_pitch>=width &&
        previous_pitch<=SIZE_MAX/height && current_pitch<=SIZE_MAX/height;
}
#define INDEX_DELTA(TYPE,SUFFIX,EQUAL) \
bool snesrecomp_ppu_index_delta_##SUFFIX(const TYPE *previous,const TYPE *current, \
    size_t previous_pitch,size_t current_pitch,unsigned width,unsigned height,uint64_t *rows) { \
    if(!previous || !current || !rows || !Dimensions(previous_pitch,current_pitch,width,height) || \
       previous_pitch*height>SIZE_MAX/sizeof(TYPE) || current_pitch*height>SIZE_MAX/sizeof(TYPE)) return false; \
    for(unsigned y=0;y<height;++y) { \
        const TYPE *a=previous+(size_t)y*previous_pitch,*b=current+(size_t)y*current_pitch; \
        uint64_t changed=0; \
        if(!EQUAL(a,b,width)) for(unsigned x=0;x<width;x+=8u) { \
            const unsigned count=width-x<8u?width-x:8u; \
            if(!EQUAL(a+x,b+x,count)) changed|=UINT64_C(1)<<(x/8u); \
        } \
        rows[y]=changed; \
    } \
    return true; \
}
INDEX_DELTA(uint16_t,u16,snesrecomp_u16_equal)
INDEX_DELTA(uint8_t,u8,snesrecomp_u8_equal)
#undef INDEX_DELTA
bool snesrecomp_ppu_index_region_unchanged(const uint64_t *rows,unsigned width,unsigned height,
    unsigned x,unsigned y,unsigned rectangle_width,unsigned rectangle_height) {
    if(!rows || !width || width>512u || !height || height>SNES_PPU_MAX_BANDS ||
       !rectangle_width || !rectangle_height || x>=width || y>=height ||
       rectangle_width>width-x || rectangle_height>height-y) return false;
    const unsigned first=x/8u,last=(x+rectangle_width-1u)/8u;
    const uint64_t mask=(UINT64_MAX>>(63u-last+first))<<first;
    for(unsigned row=y;row<y+rectangle_height;++row) if(rows[row]&mask) return false;
    return true;
}
