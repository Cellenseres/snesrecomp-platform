#include "snesrecomp_platform/snes_obj_ranges.h"
#include <stddef.h>
#include <string.h>

const uint32_t *snesrecomp_obj_range_row(SnesRecompObjRangeCache *c,
    const uint16_t oam[256],const uint8_t high[32],
    unsigned mode,bool interlace,unsigned line) {
    static const uint8_t sizes[8][2]={
        {8,16},{8,32},{8,64},{16,32},{16,64},{32,64},{16,32},{16,32}};
    if(!c || !oam || !high || mode>=8) return NULL;
    const bool rebuild=!c->valid || c->size_mode!=mode || c->interlace!=interlace;
    if(rebuild ||
        memcmp(c->oam,oam,sizeof c->oam) || memcmp(c->high_oam,high,sizeof c->high_oam)) {
        if(rebuild) memset(c->rows,0,sizeof c->rows);
        for(unsigned slot=0;slot<128;++slot) {
            const unsigned index=slot*2u;
            const unsigned large_shift=(index&7u)+1u;
            /* X, tile, palette, flips and priority do not change vertical
             * candidates. A moving cursor must not rebuild every OAM row.
             * Source equality is still checked on every request, including
             * direct writes and restores without a dirty counter. */
            if(!rebuild && !((c->oam[index]^oam[index])&0xff00u) &&
               !((c->high_oam[index>>3]^high[index>>3])&(1u<<large_shift))) continue;
            const uint32_t bit=UINT32_C(1)<<(slot&31u);
            if(!rebuild) {
                const unsigned old_y=c->oam[index]>>8;
                const unsigned old_large=(c->high_oam[index>>3]>>large_shift)&1u;
                const unsigned old_height=sizes[mode][old_large]>>(interlace?1u:0u);
                for(unsigned row=0;row<old_height;++row)
                    c->rows[(old_y+row)&255u][slot>>5]&=~bit;
            }
            const unsigned y=oam[index]>>8;
            const unsigned large=(high[index>>3]>>large_shift)&1u;
            const unsigned height=sizes[mode][large]>>(interlace?1u:0u);
            for(unsigned row=0;row<height;++row)
                c->rows[(y+row)&255u][slot>>5]|=bit;
        }
        memcpy(c->oam,oam,sizeof c->oam);memcpy(c->high_oam,high,sizeof c->high_oam);
        c->size_mode=(uint8_t)mode;c->interlace=interlace;c->valid=true;
    }
    return c->rows[line&255u];
}
