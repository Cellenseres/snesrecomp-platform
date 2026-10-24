#include "snesrecomp_platform/snes_ppu_index_row.h"
#include "snesrecomp_platform/snes_bitplanes.h"
#include <stddef.h>

const SnesRecompPpuIndexRow *snesrecomp_ppu_index_row4(
    SnesRecompPpuIndexRowCache *cache, const uint16_t vram[32768],
    const SnesPpuBgState *bg, unsigned source_y, unsigned h_scroll,
    unsigned columns) {
    const unsigned scroll=h_scroll&511u;
    if(!cache || !vram || !bg || bg->bpp!=4 || bg->big_tiles ||
        (columns!=32 && columns!=64) || (columns==32 && scroll)) return NULL;
    SnesRecompPpuIndexRow *out=&cache->output;
    if(out->scroll!=scroll) cache->valid=0;
    out->scroll=scroll;out->row=source_y&7u;
    const unsigned map_row=(source_y&511u)>>3;
    unsigned map=bg->tilemap_word_addr+(map_row&31u)*32u;
    if(bg->tall && map_row>=32u) map+=bg->wide?2048u:1024u;
    unsigned hits=0,decodes=0;
    for(unsigned column=0;column<columns;++column) {
        const unsigned address=map+(column&31u)+(bg->wide && column>=32u?1024u:0u);
        const unsigned word=vram[address&32767u];
        const unsigned row=word&0x8000u?7u-out->row:out->row;
        const unsigned at=(bg->char_word_addr+(word&1023u)*16u+row)&32767u;
        const uint32_t planes=vram[at]|((uint32_t)vram[(at+8u)&32767u]<<16);
        const uint64_t bit=UINT64_C(1)<<column;
        if((cache->valid&bit) && out->words[column]==word && cache->planes[column]==planes) {
            ++hits;continue;
        }
        const uint32_t packed=snesrecomp_ppu_unpack_4bpp(planes);
        for(unsigned pixel=0;pixel<8;++pixel) {
            const unsigned source=word&0x4000u?7u-pixel:pixel;
            out->pixels[(column*8u+pixel-scroll)&511u]=
                (uint8_t)(((word>>6)&0x70u)|((packed>>((7u-source)*4u))&15u));
        }
        out->words[column]=(uint16_t)word;cache->planes[column]=planes;
        cache->valid|=bit;++decodes;
    }
    if(decodes) {
        out->has_visible_pixels=false;
        for(unsigned x=0;x<256;++x) if(out->pixels[x]&15u) {out->has_visible_pixels=true;break;}
    }
    cache->hits+=hits;cache->decodes+=decodes;
    return out;
}
