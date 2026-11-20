#include "snesrecomp_platform/snes_ppu_frame_key.h"
#include "snesrecomp_platform/memory_compare.h"
#include <stdlib.h>
#include <string.h>

/* Canonical scalar key: structure padding is never compared. */
typedef struct FrameShape {
    uint32_t native_width, width, extra, height, obj_pitch;
    uint32_t bands, palette_lines, raster_flags, map_serial[SNES_PPU_BG_COUNT];
    uint32_t layout[21], maps, required, objects, oam, windows;
} FrameShape;
_Static_assert(sizeof(FrameShape)==38u*sizeof(uint32_t),"Frame key has padding");
struct SnesRecompPpuFrameKey {
    bool valid;
    SnesRecompPpuFrameVersions versions;
    FrameShape shape;
    SnesPpuRasterBand bands[SNES_PPU_MAX_BANDS];
    SnesPpuResolvedWindow windows[SNES_PPU_MAX_BANDS*6];
    uint16_t oam[SNES_PPU_OAM_WORDS];
    uint8_t high_oam[SNES_PPU_HIGH_OAM_BYTES];
    uint8_t maps[SNES_PPU_BG_COUNT][SNESRECOMP_PPU_RESOLVED_MAP_BYTES];
};
static FrameShape Shape(const SnesPpuFrameCapture *c) {
    FrameShape s={0};
    s.native_width=c->native_width;s.width=c->canvas_width;
    s.extra=c->canvas_extra;s.height=c->visible_height;s.obj_pitch=c->obj_pitch;
    s.bands=c->band_count;s.palette_lines=c->cgram_line_count;
    s.raster_flags=c->raster_memory_flags;
    const SnesPpuLayoutState *l=&c->layout;
    const uint32_t layout[21]={l->extra_left_right,l->extra_left_cur,l->extra_right_cur,
        l->layer_widen_mask,l->layer_clamp,l->layer_mirror,l->layer_repeat,l->bg3_widen_y,
        l->window_expand_layers,l->window_expand_windows,l->render_flags,
        l->hud_split_height,l->hud_left_end,l->hud_right_start,l->hud_oam_first_slot,
        l->hud_oam_slots,l->hud_oam_height,l->hud_oam_first_slot2,l->hud_oam_slots2,
        l->oam_left_hint_strict,l->oam_right_hint_strict};
    memcpy(s.layout,layout,sizeof layout);
    s.objects=c->obj_pixels!=NULL;s.oam=c->oam!=NULL;s.windows=c->resolved_windows!=NULL;
    s.required=c->resolved_bg_required;
    for(unsigned i=0;i<SNES_PPU_BG_COUNT;++i) {
        if(c->resolved_bg[i]) s.maps|=(uint8_t)(1u<<i);
        s.map_serial[i]=c->resolved_bg_serial[i];
    }
    return s;
}
SnesRecompPpuFrameKey *snesrecomp_ppu_frame_key_create(void) {
    return calloc(1,sizeof(SnesRecompPpuFrameKey));
}
void snesrecomp_ppu_frame_key_destroy(SnesRecompPpuFrameKey *key) {free(key);}
void snesrecomp_ppu_frame_key_clear(SnesRecompPpuFrameKey *key) {if(key)key->valid=false;}
static bool Matches(const SnesRecompPpuFrameKey *key,
    const SnesRecompPpuFrame *frame,bool objects) {
    if(!key || !key->valid || snesrecomp_ppu_frame_obj(frame)) return false;
    const SnesRecompPpuFrameVersions *v=snesrecomp_ppu_frame_versions(frame);
    const SnesPpuFrameCapture *c=snesrecomp_ppu_frame_capture(frame);
    if(!c || !v || v->owner!=key->versions.owner || v->vram!=key->versions.vram ||
        v->palette!=key->versions.palette || (objects && v->objects!=key->versions.objects)) return false;
    const FrameShape shape=Shape(c);
    if(!snesrecomp_u8_equal((const uint8_t *)&shape,(const uint8_t *)&key->shape,sizeof shape) ||
        !snesrecomp_u8_equal((const uint8_t *)c->bands,(const uint8_t *)key->bands,
            c->band_count*sizeof *c->bands)) return false;
    if(c->resolved_windows && !snesrecomp_u8_equal((const uint8_t *)c->resolved_windows,
        (const uint8_t *)key->windows,c->band_count*6u*sizeof *c->resolved_windows)) return false;
    if(objects && c->oam && (!snesrecomp_u16_equal(c->oam,key->oam,SNES_PPU_OAM_WORDS) ||
        !snesrecomp_u8_equal(c->high_oam,key->high_oam,SNES_PPU_HIGH_OAM_BYTES))) return false;
    for(unsigned i=0;i<SNES_PPU_BG_COUNT;++i) if(c->resolved_bg[i] &&
        !snesrecomp_u8_equal(c->resolved_bg[i],key->maps[i],sizeof key->maps[i])) return false;
    return true;
}
unsigned snesrecomp_ppu_frame_key_mismatch(const SnesRecompPpuFrameKey *key,
    const SnesRecompPpuFrame *frame) {
    const SnesRecompPpuFrameVersions *v=snesrecomp_ppu_frame_versions(frame);
    const SnesPpuFrameCapture *c=snesrecomp_ppu_frame_capture(frame);
    if(!key || !key->valid || !c || !v || snesrecomp_ppu_frame_obj(frame)) return 1u;
    unsigned result=0;
    if(v->owner!=key->versions.owner || v->vram!=key->versions.vram || v->palette!=key->versions.palette) result|=1u;
    const FrameShape shape=Shape(c);
    if(!snesrecomp_u8_equal((const uint8_t *)&shape,(const uint8_t *)&key->shape,sizeof shape)) return result|2u;
    if(!snesrecomp_u8_equal((const uint8_t *)c->bands,(const uint8_t *)key->bands,c->band_count*sizeof *c->bands)) result|=4u;
    if(c->resolved_windows && !snesrecomp_u8_equal((const uint8_t *)c->resolved_windows,
        (const uint8_t *)key->windows,c->band_count*6u*sizeof *c->resolved_windows)) result|=8u;
    if(c->oam && (!snesrecomp_u16_equal(c->oam,key->oam,SNES_PPU_OAM_WORDS) ||
        !snesrecomp_u8_equal(c->high_oam,key->high_oam,SNES_PPU_HIGH_OAM_BYTES))) result|=16u;
    for(unsigned i=0;i<SNES_PPU_BG_COUNT;++i) if(c->resolved_bg[i] &&
        !snesrecomp_u8_equal(c->resolved_bg[i],key->maps[i],sizeof key->maps[i])) result|=256u<<i;
    return result;
}
bool snesrecomp_ppu_frame_key_matches(const SnesRecompPpuFrameKey *key,
    const SnesRecompPpuFrame *frame) {return Matches(key,frame,true);}
bool snesrecomp_ppu_frame_key_matches_background(const SnesRecompPpuFrameKey *key,
    const SnesRecompPpuFrame *frame) {return Matches(key,frame,false);}
bool snesrecomp_ppu_frame_key_commit(SnesRecompPpuFrameKey *key,
    const SnesRecompPpuFrame *frame) {
    if(!key) return false;
    key->valid=false;
    const SnesPpuFrameCapture *c=snesrecomp_ppu_frame_capture(frame);
    const SnesRecompPpuFrameVersions *v=snesrecomp_ppu_frame_versions(frame);
    if(!c || !v || snesrecomp_ppu_frame_obj(frame)) return false;
    key->shape=Shape(c);key->versions=*v;
    memcpy(key->bands,c->bands,c->band_count*sizeof *c->bands);
    if(c->resolved_windows) memcpy(key->windows,c->resolved_windows,
        c->band_count*6u*sizeof *c->resolved_windows);
    if(c->oam) {
        memcpy(key->oam,c->oam,sizeof key->oam);
        memcpy(key->high_oam,c->high_oam,sizeof key->high_oam);
    }
    for(unsigned i=0;i<SNES_PPU_BG_COUNT;++i)
        if(c->resolved_bg[i]) memcpy(key->maps[i],c->resolved_bg[i],sizeof key->maps[i]);
    key->valid=true;return true;
}
