#include "snesrecomp_platform/snes_ppu_indexed_palette.h"
#include <stdlib.h>
#include <string.h>
struct SnesRecompIndexedPalette {
    uint32_t argb[256*256];
    uint16_t palette[256*256];
    uint64_t generations[256],generation,owner,palette_version;
    uint8_t brightness[256],encoding;
    uint16_t rows;
    SnesRecompIndexedPaletteSpan spans[256];
    uint16_t span_count;
    bool valid;
};
SnesRecompIndexedPalette *snesrecomp_ppu_indexed_palette_create(uint8_t encoding) {
    if(encoding!=SNESRECOMP_INDEXED_PALETTE_RAW_RGB5 && encoding!=SNESRECOMP_INDEXED_PALETTE_DISPLAY_RGB8) return NULL;
    SnesRecompIndexedPalette *p=calloc(1,sizeof *p);if(p) p->encoding=encoding;return p;
}
void snesrecomp_ppu_indexed_palette_destroy(SnesRecompIndexedPalette *p) {free(p);}
void snesrecomp_ppu_indexed_palette_reset(SnesRecompIndexedPalette *p) {if(p) p->valid=false;}
bool snesrecomp_ppu_indexed_palette_set_encoding(SnesRecompIndexedPalette *p,uint8_t encoding) {
    if(!p || (encoding!=SNESRECOMP_INDEXED_PALETTE_RAW_RGB5 && encoding!=SNESRECOMP_INDEXED_PALETTE_DISPLAY_RGB8))return false;
    if(p->encoding!=encoding){p->encoding=encoding;p->valid=false;}
    return true;
}
bool snesrecomp_ppu_indexed_palette_base_direct(const SnesRecompPpuFrame *frame) {
    const SnesPpuFrameCapture *c=snesrecomp_ppu_frame_capture(frame);
    if(!c || !c->bands || !c->band_count)return false;
    for(unsigned i=0;i<c->band_count;++i) {
        const SnesPpuRasterBand *b=c->bands+i;
        if(b->forced_blank)continue;
        if(b->bg_mode!=1 || (b->main_enable&16u) ||
           (b->cgadsub&63u) || (b->cgwsel&0xc1u))return false;
    }
    return true;
}
SnesRecompPpuResult snesrecomp_ppu_indexed_palette_prepare(
    SnesRecompIndexedPalette *p,const SnesRecompPpuFrame *frame,SnesRecompIndexedPalettePlan *out) {
    if(!out) return SNESRECOMP_PPU_INVALID;
    memset(out,0,sizeof *out);
    const SnesPpuFrameCapture *c=snesrecomp_ppu_frame_capture(frame);
    const SnesRecompPpuFrameVersions *v=snesrecomp_ppu_frame_versions(frame);
    if(!p || !c || !v || !c->cgram || !c->visible_height || c->visible_height>256 ||
        !c->bands || !c->band_count) return SNESRECOMP_PPU_INVALID;
    if(p->generation>UINT64_MAX-256u) return SNESRECOMP_PPU_RESOURCE_LIMIT;
    const bool same=p->valid && p->owner==v->owner && p->palette_version==v->palette;
    /* RGB depends on the palette and brightness, not on the scanline number.
     * Reuse a row computed/validated in this call. Sixteen bounded memo slots
     * suffice for ordinary SNES brightness; check the full byte too so unusual
     * captures retain the original conversion without an out-of-range index. */
    int16_t memo[16];for(unsigned i=0;i<16;++i)memo[i]=-1;
    unsigned changed=0,band=0;
    for(unsigned y=0;y<c->visible_height;++y) {
        while(y>=c->bands[band].y_end) ++band;
        const SnesPpuRasterBand *b=c->bands+band;
        const uint8_t brightness=p->encoding==SNESRECOMP_INDEXED_PALETTE_RAW_RGB5?15:
            b->forced_blank?0:b->brightness;
        const uint16_t *palette=c->cgram_lines?c->cgram_lines+y*256u:c->cgram;
        const unsigned key=brightness&15u;
        const bool same_brightness=p->valid && y<p->rows && p->brightness[y]==brightness;
        if(same_brightness && (same || !memcmp(p->palette+y*256u,palette,512))) {
            memo[key]=(int16_t)y;continue;
        }
        const int previous=memo[key];
        if(previous>=0 && p->brightness[previous]==brightness &&
            (!c->cgram_lines || !memcmp(p->palette+(unsigned)previous*256u,palette,512))) {
            memcpy(p->argb+y*256u,p->argb+(unsigned)previous*256u,1024);
        } else for(unsigned i=0;i<256;++i) {
            /* Palette animation usually changes a few entries. Preserve the
             * already exact RGB values for unchanged entries at this shade. */
            if(same_brightness && p->palette[y*256u+i]==palette[i])continue;
            const unsigned colour=palette[i];uint32_t argb=0xff000000u;
            for(unsigned channel=0;channel<3;++channel) {
                unsigned level=(colour>>(channel*5u))&31u;
                const unsigned value=p->encoding==SNESRECOMP_INDEXED_PALETTE_RAW_RGB5?level*8u:
                    (level*8u+level/4u)*brightness/15u;
                argb|=value<<(16u-channel*8u);
            }
            p->argb[y*256u+i]=argb;
        }
        memcpy(p->palette+y*256u,palette,512);p->brightness[y]=brightness;
        memo[key]=(int16_t)y;
        p->generations[y]=++p->generation;++changed;
    }
    const bool rebuild=!p->valid || changed || p->rows!=c->visible_height;
    if(rebuild) {
        p->span_count=0;
        for(unsigned y=0;y<c->visible_height;) {
            unsigned end=y+1u;
            while(end<c->visible_height && !memcmp(p->argb+y*256u,p->argb+end*256u,1024))++end;
            p->spans[p->span_count++]=(SnesRecompIndexedPaletteSpan){(uint16_t)y,(uint16_t)end};y=end;
        }
    }
    p->rows=c->visible_height;p->valid=true;p->owner=v->owner;p->palette_version=v->palette;
    *out=(SnesRecompIndexedPalettePlan){p->argb,p->generations,changed,changed*256u,p->rows,p->encoding,0,
        p->spans,p->span_count,0,(uint32_t)rebuild};
    return SNESRECOMP_PPU_OK;
}
