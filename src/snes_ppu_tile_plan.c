#include "snesrecomp_platform/snes_ppu_tile_plan.h"
#include "snesrecomp_platform/snes_ppu_indexed_tiles.h"
#include "snesrecomp_platform/snes_ppu_semantic_gpu.h"
#include "snesrecomp_platform/snes_ppu_semantics.h"
#include "snesrecomp_platform/snes_ppu_mode7.h"
#include "snesrecomp_platform/snes_bitplanes.h"
#include "snesrecomp_platform/snes_ppu_elastic.h"
#include "snes_ppu_memory.h"
#include "snes_ppu_windows.h"
#include <stdlib.h>
#include <string.h>
#if defined(SNESRECOMP_PPU_TILE_COSTS)
#include "snesrecomp_platform/task.h"
#include <stdio.h>
#define TILE_COST_MARK(n) do { if(profile_costs) { \
    const uint64_t now=snesrecomp_now_us();profile_times[n]=now-profile_mark;profile_mark=now; \
} } while(0)
#else
#define TILE_COST_MARK(n) do {} while(0)
#endif


enum { CACHE_WAYS = 8, CACHE_BUCKETS = SNESRECOMP_COLOUR_TILE_SLOTS / CACHE_WAYS };
typedef struct ColourTileEntry {
    uint32_t key, palette_hash, epoch;
    uint64_t image_owner, image_version;
    uint32_t palette[16];
    uint16_t source[16];
    bool valid;
} ColourTileEntry;
typedef struct BackgroundSnapshot {
    SnesRecompPpuFrameVersions versions;
    SnesPpuLayoutState layout;
    SnesPpuRasterBand bands[SNES_PPU_MAX_BANDS];
    SnesPpuResolvedWindow windows[SNES_PPU_MAX_BANDS*6];
    uint16_t vram[SNES_PPU_VRAM_WORDS];
    uint32_t vram_blocks[SNES_PPU_VRAM_WORDS / 64u / 32u];
    uint32_t layer_vram_blocks[3][SNES_PPU_VRAM_WORDS / 64u / 32u];
    uint32_t vram_copied[SNES_PPU_VRAM_WORDS / 64u / 32u];
    uint64_t vram_owner, vram_version; /* VRAM the copied blocks were taken from. */
    uint16_t palettes[SNES_PPU_MAX_BANDS][128];
    uint8_t maps[SNES_PPU_BG_COUNT][SNESRECOMP_PPU_RESOLVED_MAP_BYTES];
    uint32_t quad_count, backdrop_count;
    unsigned band_count;
    uint16_t width, height, extra;
    uint8_t map_mask;
    bool valid, palette_lines, palette_recorded, uniform_palette, resolved_windows;
} BackgroundSnapshot;
typedef struct ObjectColourEntry {
    uint32_t shade, argb;
    uint16_t colour;
    bool valid;
} ObjectColourEntry;
typedef struct TileMemo {
    uint16_t slots[8][1024];
    uint32_t stamps[8][1024], stamp;
    uint64_t generations[8][1024];
    uint16_t palette[128], char_addr, fixed;
    uint8_t bpp, brightness;
    bool valid, raw, indexed, rgb, math, half, subtract;
} TileMemo;
typedef struct NegativeRaster {
    SnesPpuRasterBand bands[SNES_PPU_MAX_BANDS];
    SnesPpuResolvedWindow windows[SNES_PPU_MAX_BANDS*6];
    SnesPpuLayoutState layout;
    uint32_t dependencies[SNES_PPU_VRAM_WORDS/64u/32u];
    uint16_t count,width,height,extra;
    uint8_t layer;
    bool valid,resolved;
} NegativeRaster;
/* A BG split by an HDMA wave, drawn once in map space. */
typedef struct PlaneState {
    SnesRecompTileQuad tiles[SNESRECOMP_PLANE_TILES];
    SnesRecompPlaneQuad quads[SNESRECOMP_PLANE_QUADS];
    uint16_t vram[SNES_PPU_VRAM_WORDS];
    uint32_t vram_blocks[SNES_PPU_VRAM_WORDS/64u/32u];
    uint8_t map[SNESRECOMP_PPU_RESOLVED_MAP_BYTES];
    uint16_t palette[128];
    uint64_t rows, tile_generation;
    uint64_t columns[2][64]; /* Opaque tile columns per priority and map row. */
    uint32_t tile_count, quad_count, epoch;
    uint16_t char_addr, tilemap_addr, width, height, retry;
    uint8_t layer, bpp;
    bool wide, tall, resolved, tiles_valid, active;
} PlaneState;
/* One direct-path layer as last drawn, for moving it by a scroll delta. */
typedef struct ShiftLayer {
    SnesPpuRasterBand band;
    SnesRecompWindowSpan span;
    uint32_t count;
    bool valid, resolved;
} ShiftLayer;
enum { SHIFT_ROWS = 32, SHIFT_COLUMNS = 64, SHIFT_QUADS = 4096 };
struct SnesRecompColourTileCache {
    NegativeRaster *negative;
    PlaneState *plane;
    SnesPpuRasterBand *unshaded_bands; /* Present once post brightness is enabled. */
    ColourTileEntry entries[SNESRECOMP_COLOUR_TILE_SLOTS];
    uint32_t tiles[SNESRECOMP_COLOUR_TILE_SLOTS * 64];
    uint8_t dirty[SNESRECOMP_COLOUR_TILE_SLOTS];
    uint64_t generations[SNESRECOMP_COLOUR_TILE_SLOTS];
    bool opaque[SNESRECOMP_COLOUR_TILE_SLOTS];
    uint16_t indexed_classes[SNESRECOMP_COLOUR_TILE_SLOTS];
    uint64_t generation;
    SnesRecompTileQuad quads[SNESRECOMP_TILE_PLAN_QUADS];
    uint16_t indexed_slots[SNESRECOMP_TILE_PLAN_QUADS];
    SnesRecompIndexedTileQuad indexed_quads[SNESRECOMP_TILE_PLAN_QUADS];
    SnesRecompTileBackdrop backdrops[SNES_PPU_MAX_BANDS];
    uint32_t obj_pixels[SNESRECOMP_PPU_MAX_WIDTH * SNES_PPU_MAX_BANDS];
    uint16_t obj_source[SNESRECOMP_PPU_MAX_WIDTH * SNES_PPU_MAX_BANDS];
    uint16_t obj_palettes[SNES_PPU_MAX_BANDS][128];
    bool obj_source_present, obj_palette_lines;
    uint64_t geometry_generation, object_generation;
    SnesRecompPpuFrameVersions object_versions;
    uint32_t obj_overlay_pixels;
    uint32_t epoch;
    BackgroundSnapshot background;
    ObjectColourEntry object_colours[4][128];
    TileMemo *tile_memos[12]; /* layer, screen and clipping are independent. */
    bool memo_attempted[12];
    SnesRecompWindowSpan windows[SNES_PPU_MAX_BANDS][SNESRECOMP_WINDOW_MAX_SPANS];
    unsigned window_counts[SNES_PPU_MAX_BANDS];
    bool raw_tiles, indexed_tiles;
    uint64_t source_owner, source_geometry;
    uint64_t source_versions[SNESRECOMP_PPU_JOB_MAX_IMAGES];
    uint16_t source_count;
    uint32_t native_quad_count;
    struct SourceClipRect { uint16_t x,y,width,height;uint8_t layer,screens; }
        source_clips[SNESRECOMP_PPU_JOB_MAX_SOURCES];
    uint8_t source_row_counts[2][3][SNES_PPU_MAX_BANDS];
    uint16_t source_rows[2][3][SNES_PPU_MAX_BANDS][SNESRECOMP_PPU_JOB_SOURCES_PER_ROW];
    const SnesRecompPpuJobView *source_job; /* Only during synchronous preparation. */
    SnesRecompImageQuad image_quads[SNESRECOMP_IMAGE_QUADS];
    uint32_t image_quad_count;
    bool image_active;
    ShiftLayer shift[3];
    SnesRecompTileQuad shift_quads[3][SHIFT_QUADS];
    uint8_t shift_palettes[3][SHIFT_QUADS];
    uint8_t quad_palettes[SNESRECOMP_TILE_PLAN_QUADS]; /* BG palette of each direct quad. */
    uint32_t shift_epoch;
    uint16_t shift_cells[3][SHIFT_ROWS][SHIFT_COLUMNS];
    /* Epoch in which a 2bpp/4bpp character at this VRAM word was found blank. */
    uint32_t blank_epoch[2][SNES_PPU_VRAM_WORDS / 8u];
};

SnesRecompColourTileCache *snesrecomp_colour_tiles_create(void) {
    SnesRecompColourTileCache *c=calloc(1,sizeof(*c));
    if(c)c->negative=calloc(1,sizeof(*c->negative)); /* Optional, once. */
    return c;
}
void snesrecomp_colour_tiles_destroy(SnesRecompColourTileCache *c) {
    if (c) for (unsigned i = 0; i < 12; ++i) free(c->tile_memos[i]);
    if(c)free(c->negative);
    if(c)free(c->plane);
    if(c)free(c->unshaded_bands);
    free(c);
}
void snesrecomp_colour_tiles_reset(SnesRecompColourTileCache *c) {
    if (c) {
        memset(c->entries, 0, sizeof(c->entries));
        memset(c->dirty, 0, sizeof(c->dirty));
        c->epoch = 0; c->background.valid = false;
        if(c->negative)c->negative->valid=false;
        if(c->plane)c->plane->tiles_valid=c->plane->active=false;
        memset(c->background.layer_vram_blocks,0,sizeof c->background.layer_vram_blocks);
        memset(c->background.vram_copied,0,sizeof c->background.vram_copied);
        memset(c->blank_epoch,0,sizeof c->blank_epoch);
        for (unsigned i = 0; i < 3; ++i) c->shift[i].valid = false;
        for (unsigned i = 0; i < 12; ++i)
            if (c->tile_memos[i]) c->tile_memos[i]->valid = false;
    }
}
void snesrecomp_colour_tiles_uploaded(SnesRecompColourTileCache *c) {
    if (c) memset(c->dirty, 0, sizeof(c->dirty));
}
bool snesrecomp_colour_tiles_enable_post_brightness(SnesRecompColourTileCache *c) {
    if(!c) return false;
    if(!c->unshaded_bands) c->unshaded_bands=calloc(SNES_PPU_MAX_BANDS,sizeof *c->unshaded_bands);
    return c->unshaded_bands!=NULL;
}
bool snesrecomp_colour_tiles_enable_planes(SnesRecompColourTileCache *c) {
    if(!c) return false;
    if(!c->plane) c->plane=calloc(1,sizeof *c->plane);
    return c->plane!=NULL;
}

static uint32_t Colour(uint16_t value, const SnesPpuRasterBand *band,
                       const SnesRecompWindowSpan *span, unsigned source) {
    const bool math = (span->mask & SNESRECOMP_SEMANTIC_MATH) &&
                      (band->cgadsub & (1u << source));
    return snesrecomp_ppu_compose_colour(value, band->fixed_colour,
        !(span->mask & SNESRECOMP_SEMANTIC_MAIN_RGB), math,
        (band->cgadsub & 128u) != 0,
        (band->cgadsub & 64u) && !(band->cgwsel & 2u), band->brightness);
}

/* Compare content, never pointer identity or advisory dirty counters. */
static bool LayerEquals(const SnesPpuRasterBand *,const SnesPpuRasterBand *,unsigned,bool);
/* Raster partitions may change for brightness or OBJ without changing a
 * single native BG primitive. Compare the mesh state over intersecting runs,
 * retaining exact coverage and all resolved windows. */
static bool BackgroundRawWindowsMatch(const SnesPpuResolvedWindow *a,
    const SnesPpuResolvedWindow *b,unsigned layers) {
    /* BG geometry uses its own window and the colour window. OBJ/BG4
     * windows belong to the independent raw composition plan. */
    if(memcmp(a+5,b+5,sizeof *a))return false;
    for(unsigned layer=0;layer<3;++layer)if(layers&(1u<<layer))
        if(memcmp(a+layer,b+layer,sizeof *a))return false;
    return true;
}
/* Raw main/sub planes have separate visibility and materials. Selecting a
 * different colour input changes the compositor, not the raw main BG mesh. */
static bool LayerEqualsRawPlane(const SnesPpuRasterBand *a,
    const SnesPpuRasterBand *b,unsigned layer,bool sub) {
    const unsigned bit=1u<<layer;
    if(memcmp(&a->bg[layer],&b->bg[layer],sizeof a->bg[layer]) ||
        a->forced_blank!=b->forced_blank || (layer==2u && a->bg3_priority!=b->bg3_priority))return false;
    if(sub) {
        if(((a->sub_enable^b->sub_enable)&bit) ||
            ((a->sub_window_enable^b->sub_window_enable)&bit) || ((a->cgwsel^b->cgwsel)&2u))return false;
    } else if(((a->main_enable^b->main_enable)&bit) ||
        ((a->main_window_enable^b->main_window_enable)&bit) ||
        ((a->cgwsel^b->cgwsel)&253u) || ((a->cgadsub^b->cgadsub)&bit))return false;
    const uint32_t selectors=(15u<<(layer*4u))|(15u<<20);
    return !((a->window_sel^b->window_sel)&selectors) &&
        a->window1_left==b->window1_left && a->window1_right==b->window1_right &&
        a->window2_left==b->window2_left && a->window2_right==b->window2_right &&
        !((a->regs[56]^b->regs[56])&(3u<<(layer*2u))) && !((a->regs[57]^b->regs[57])&12u);
}

static bool BackgroundRawRasterMatches(const BackgroundSnapshot *old,
    const SnesPpuFrameCapture *cap,unsigned layers,unsigned screens) {
    unsigned a=0,b=0,y=0;
    while(a<old->band_count && b<cap->band_count) {
        const SnesPpuRasterBand *p=old->bands+a,*n=cap->bands+b;
        if(p->y_begin>y || n->y_begin>y || p->y_end<=y || n->y_end<=y ||
            ((layers&1u) && !(screens==3u?LayerEquals(p,n,0,true):LayerEqualsRawPlane(p,n,0,screens==2u))) ||
            ((layers&2u) && !(screens==3u?LayerEquals(p,n,1,true):LayerEqualsRawPlane(p,n,1,screens==2u))) ||
            ((layers&4u) && !(screens==3u?LayerEquals(p,n,2,true):LayerEqualsRawPlane(p,n,2,screens==2u))) || (cap->resolved_windows &&
             !BackgroundRawWindowsMatch(old->windows+a*6u,
                cap->resolved_windows+b*6u,layers))) return false;
        y=p->y_end<n->y_end?p->y_end:n->y_end;
        if(y==p->y_end)++a;
        if(y==n->y_end)++b;
    }
    return y==cap->visible_height && a==old->band_count && b==cap->band_count;
}

/* Raw geometry uses resolved per-layer margins, not unrelated HUD/OBJ
 * policy bytes. Keep every layout input consumed by DrawLayer/PrepareWindow. */
static bool RawPlaneLayoutMatches(const SnesPpuLayoutState *a,
    const SnesPpuLayoutState *b,unsigned layers) {
    return a->extra_left_cur==b->extra_left_cur && a->extra_right_cur==b->extra_right_cur &&
        !((a->layer_mirror^b->layer_mirror)&layers) &&
        !((a->window_expand_layers^b->window_expand_layers)&(layers|32u)) &&
        !((a->window_expand_windows^b->window_expand_windows)&3u);
}

/* Memory checks depend only on the layers, not the screen. */
static bool BackgroundRasterMatches(const BackgroundSnapshot *old, const SnesPpuFrameCapture *cap,
    bool raw,unsigned layers,unsigned screens) {
    if (!old->valid || old->width != cap->canvas_width || old->height != cap->visible_height ||
        old->extra != cap->canvas_extra || (!raw && old->band_count != cap->band_count) ||
        (raw && screens!=3u?!RawPlaneLayoutMatches(&old->layout,&cap->layout,layers):
         memcmp(&old->layout,&cap->layout,sizeof old->layout))) return false;
    if(old->resolved_windows!=(cap->resolved_windows!=NULL)) return false;
    if(raw) {
        if(!BackgroundRawRasterMatches(old,cap,layers,screens)) return false;
    } else if(memcmp(old->bands,cap->bands,cap->band_count*sizeof(*cap->bands)) ||
        (cap->resolved_windows && memcmp(old->windows,cap->resolved_windows,
            cap->band_count*6u*sizeof(*cap->resolved_windows)))) return false;
    return true;
}
static bool BackgroundMemoryMatches(const BackgroundSnapshot *old, const SnesPpuFrameCapture *cap,
    const SnesRecompPpuFrameVersions *versions, bool raw, bool uniform_palette,unsigned layers) {
    if (!(old->versions.owner==versions->owner && old->versions.vram==versions->vram))
        for (unsigned block=0; block<SNES_PPU_VRAM_WORDS/64u; ++block) {
            uint32_t used=old->vram_blocks[block/32u];
            if(raw && layers!=7u) {
                used=0;
                for(unsigned layer=0;layer<3;++layer)if(layers&(1u<<layer))
                    used|=old->layer_vram_blocks[layer][block/32u];
            }
            if ((used & (1u<<(block%32u))) &&
                !snesrecomp_ppu_words_equal(old->vram+block*64u,cap->vram+block*64u,64u)) return false;
        }
    const unsigned palette_rows = old->palette_lines || cap->cgram_lines ? cap->visible_height : 1;
    /* A uniform raw palette changes atlas colours, never tile membership,
     * alpha, UVs or material buckets. Raster palettes can assign different
     * colours to one family and still require the ordinary full plan. */
    for (unsigned y = 0; !(raw && old->uniform_palette && uniform_palette) &&
        !(old->versions.owner==versions->owner &&
        old->versions.palette==versions->palette) && y < palette_rows; ++y) {
        const uint16_t *pal = cap->cgram_lines ? cap->cgram_lines + y * 256 : cap->cgram;
        if (!snesrecomp_ppu_words_equal(old->palettes[old->palette_lines ? y : 0],pal,128)) return false;
    }
    for (unsigned layer = 0; layer < SNES_PPU_BG_COUNT; ++layer) {
        if(raw && layers!=7u && !(layers&(1u<<layer)))continue;
        if (!!(old->map_mask & (1u << layer)) != !!cap->resolved_bg[layer]) return false;
        if (cap->resolved_bg[layer] && !snesrecomp_u8_equal(old->maps[layer], cap->resolved_bg[layer],
                                             sizeof(old->maps[layer]))) return false;
    }
    return true;
}
static bool BackgroundMatches(const BackgroundSnapshot *old, const SnesPpuFrameCapture *cap,
    const SnesRecompPpuFrameVersions *versions, bool raw, bool uniform_palette,unsigned layers,unsigned screens) {
    return BackgroundRasterMatches(old,cap,raw,layers,screens) &&
        BackgroundMemoryMatches(old,cap,versions,raw,uniform_palette,layers);
}

static void BackgroundRemember(BackgroundSnapshot *old, const SnesPpuFrameCapture *cap,
    const SnesRecompTilePlan *plan, const SnesRecompPpuFrameVersions *versions,
    bool record_palette) {
    const bool same_owner=old->versions.owner==versions->owner;
    const bool same_palette=old->palette_recorded && same_owner && old->versions.palette==versions->palette &&
        old->palette_lines==(cap->cgram_lines!=NULL);
    old->layout = cap->layout; old->width = cap->canvas_width;
    old->height = cap->visible_height; old->extra = cap->canvas_extra;
    old->band_count = cap->band_count; old->map_mask = 0;
    memcpy(old->bands, cap->bands, cap->band_count * sizeof(*cap->bands));
    old->resolved_windows=cap->resolved_windows!=NULL;
    if(cap->resolved_windows) memcpy(old->windows,cap->resolved_windows,
        cap->band_count*6u*sizeof(*cap->resolved_windows));
    /* Blocks first used under an unchanged VRAM version still need a copy. */
    const bool same_vram=old->vram_owner==versions->owner && old->vram_version==versions->vram;
    old->vram_owner=versions->owner;old->vram_version=versions->vram;
    for (unsigned word=0; word<SNES_PPU_VRAM_WORDS/64u/32u; ++word) {
        uint32_t copy=old->vram_blocks[word]&(same_vram?~old->vram_copied[word]:~0u);
        old->vram_copied[word]=(same_vram?old->vram_copied[word]:0u)|old->vram_blocks[word];
        while(copy) {
            const unsigned block=word*32u+(unsigned)__builtin_ctz(copy);
            copy&=copy-1u;
            memcpy(old->vram+block*64u,cap->vram+block*64u,64u*sizeof(uint16_t));
        }
    }
    old->palette_lines = cap->cgram_lines != NULL;
    for (unsigned y = 0; record_palette && !same_palette && y < (old->palette_lines ? cap->visible_height : 1); ++y) {
        const uint16_t *pal = cap->cgram_lines ? cap->cgram_lines + y * 256 : cap->cgram;
        memcpy(old->palettes[y], pal, sizeof(old->palettes[y]));
    }
    old->palette_recorded=record_palette;
    for (unsigned layer = 0; layer < SNES_PPU_BG_COUNT; ++layer)
        if (cap->resolved_bg[layer]) {
            old->map_mask |= (uint8_t)(1u << layer);
            memcpy(old->maps[layer], cap->resolved_bg[layer], sizeof(old->maps[layer]));
        }
    old->quad_count = plan->quad_count; old->backdrop_count = plan->backdrop_count;
    old->valid = true;
    old->versions=*versions;
}

/* changed_rows, when given, limits colouring to rows with new OBJ input. */
static void DrawObjects(SnesRecompColourTileCache *c, const SnesPpuFrameCapture *cap,
    const SnesPpuRasterBand *band, const SnesRecompWindowSpan *spans, unsigned ns,
    const uint64_t *coverage, const uint8_t *changed_rows) {
    if (!cap->obj_pixels) return;
    for (unsigned y = band->y_begin; y < band->y_end;) {
        const uint16_t *pal = cap->cgram_lines ? cap->cgram_lines + y * 256 : cap->cgram;
        unsigned end = band->y_end;
        if (cap->cgram_lines) {
            end = y + 1;
            while (end < band->y_end && snesrecomp_ppu_words_equal(pal+128,
                cap->cgram_lines+end*256+128,128)) ++end;
        }
        for (unsigned si = 0; si < ns; ++si) {
            const SnesRecompWindowSpan *span = &spans[si];
            const bool enabled = !band->forced_blank &&
                (span->obj_mask & SNESRECOMP_SEMANTIC_OBJ_MAIN);
            const unsigned clip = !(span->mask & SNESRECOMP_SEMANTIC_MAIN_RGB);
            const unsigned math_enabled = !!((span->mask & SNESRECOMP_SEMANTIC_MATH) &&
                                              (band->cgadsub & 16u));
            const bool half = (band->cgadsub & 64u) && !(band->cgwsel & 2u);
            const uint32_t shade = (band->fixed_colour & 32767u) |
                ((uint32_t)band->brightness << 15) | ((uint32_t)(band->cgadsub & 128u) << 12) |
                ((uint32_t)half << 20);
            for (unsigned py = y; py < end; ++py) {
                if (changed_rows && !changed_rows[py]) continue;
                for (unsigned px = span->begin; px < span->end; ++px) {
                    if(!(coverage[py]&(UINT64_C(1)<<(px>>3)))) {
                        px=((px>>3)+1u)*8u-1u;continue;
                    }
                    const uint16_t obj = cap->obj_pixels[py * cap->obj_pitch + px];
                    uint32_t colour = 0;
                    if (enabled && (obj & 0x800u)) {
                        const unsigned index = obj & 127u, math = math_enabled && (obj & 0x400u);
                        ObjectColourEntry *entry = &c->object_colours[clip * 2u + math][index];
                        const uint16_t value = pal[index + 128] & 32767u;
                        if (!entry->valid || entry->shade != shade || entry->colour != value) {
                            entry->argb = snesrecomp_ppu_compose_colour(value, band->fixed_colour,
                                clip != 0, math != 0, (band->cgadsub & 128u) != 0,
                                half, band->brightness) & 0xffffffu;
                            entry->colour = value; entry->shade = shade; entry->valid = true;
                        }
                        colour = entry->argb |
                            (((((obj >> 8) & 3u) + 1u) * 64u - 1u) << 24);
                    }
                    c->obj_pixels[py * cap->canvas_width + px] = colour;
                }
            }
        }
        y = end;
    }
}

static int Tile(SnesRecompColourTileCache *c, const SnesPpuFrameCapture *cap,
                const uint32_t *palette, uint32_t hash, unsigned word, unsigned bpp,
                unsigned palette_family, bool raw, SnesRecompTilePlan *plan) {
    const unsigned colours = 1u << bpp, words = bpp * 4u;
    const uint32_t key = word | (bpp << 15) | (raw ? palette_family << 18 : 0);
    /* Raw atlas coordinates follow source identity across palette animation.
     * Different colours used within this frame still occupy separate ways. */
    uint32_t mixed = raw ? key : key ^ hash;
    mixed = (mixed ^ (mixed >> 16)) * 0x7feb352du;
    mixed = (mixed ^ (mixed >> 15)) * 0x846ca68bu;
    const unsigned bucket = (mixed ^ (mixed >> 16)) & (CACHE_BUCKETS - 1u);
    const unsigned base = bucket * CACHE_WAYS;
    int chosen = -1;
    int family = -1;
    uint32_t newest_family = 0;
    bool update_exact = false;
    uint32_t oldest = UINT32_MAX;
    for (unsigned way = 0; way < CACHE_WAYS; ++way) {
        ColourTileEntry *e = &c->entries[base + way];
        if (e->valid && e->key == key && e->palette_hash == hash &&
            snesrecomp_u8_equal((const uint8_t *)(e->palette + 1), (const uint8_t *)(palette + 1),
                                (colours - 1) * sizeof(uint32_t))) {
            if (e->epoch == c->epoch || snesrecomp_ppu_words_equal(e->source, cap->vram + word, words)) {
                e->epoch = c->epoch;
                ++plan->cache_hits;
                return (int)(base + way);
            }
            chosen = (int)(base + way);
            update_exact = true;
            break;
        }
        if (raw && e->valid && e->key == key && e->epoch != c->epoch &&
            (family < 0 || e->epoch > newest_family)) {
            family = (int)(base + way);
            newest_family = e->epoch;
        }
        if (!e->valid || (e->epoch != c->epoch && e->epoch < oldest)) {
            chosen = (int)(base + way);
            oldest = e->valid ? e->epoch : 0;
        }
    }
    if (!update_exact && family >= 0) chosen = family;
    if (chosen < 0) return -1;
    ColourTileEntry *e = &c->entries[chosen];
    e->valid = true; e->key = key; e->palette_hash = hash; e->epoch = c->epoch;
    e->image_owner = e->image_version = 0;
    memcpy(e->source, cap->vram + word, words * sizeof(uint16_t));
    memcpy(e->palette, palette, colours * sizeof(uint32_t));
    uint32_t *pixels = c->tiles + (unsigned)chosen * 64u;
    c->opaque[chosen] = false;
    uint16_t classes=0;
    for (unsigned y = 0; y < 8; ++y) {
        const uint32_t row=snesrecomp_ppu_unpack_4bpp((uint32_t)e->source[y] |
            (bpp==4 ? (uint32_t)e->source[y+8]<<16 : 0u));
        for (unsigned x = 0; x < 8; ++x) {
            const unsigned index=(row>>(4u*(7u-x)))&15u;
            pixels[y * 8 + x] = palette[index];
            c->opaque[chosen] |= index != 0;
            if(raw && palette_family==4096u) classes|=(uint16_t)(1u<<index);
        }
    }
    c->dirty[chosen] = 1;
    c->indexed_classes[chosen]=(uint16_t)(classes&65534u);
    c->generations[chosen] = ++c->generation;
    ++plan->tiles_decoded;
    return chosen;
}

static void VramUsed(BackgroundSnapshot *background, unsigned word,unsigned layer) {
    const unsigned block=word/64u;
    background->vram_blocks[block/32u] |= 1u<<(block%32u);
    background->layer_vram_blocks[layer][block/32u] |= 1u<<(block%32u);
}

SnesRecompPpuResult snesrecomp_ppu_indexed_tiles(
    SnesRecompColourTileCache *c, const SnesRecompPpuFrame *frame,
    const SnesRecompIndexedTileRequest *requests, uint32_t count,
    SnesRecompIndexedTilePlan *out) {
    if (!out) return SNESRECOMP_PPU_INVALID;
    memset(out, 0, sizeof(*out));
    const SnesPpuFrameCapture *cap = snesrecomp_ppu_frame_capture(frame);
    if (!c || !cap || (!requests && count)) return SNESRECOMP_PPU_INVALID;
    if (count > SNESRECOMP_TILE_PLAN_QUADS) return SNESRECOMP_PPU_RESOURCE_LIMIT;
    for (unsigned i = 0; i < count; ++i) {
        const SnesRecompIndexedTileRequest *r = requests + i;
        if ((r->bpp != 2 && r->bpp != 4) || r->reserved ||
            (unsigned)r->word + r->bpp * 4u > SNES_PPU_VRAM_WORDS)
            return SNESRECOMP_PPU_INVALID;
    }
    /* Bit 30 separates index tiles from both colour keys and owned images
     * (bit 31). Keep their palette independent of CGRAM and material state. */
    uint32_t palette[16] = {0}, hashes[2] = {2166136261u, 2166136261u};
    for (unsigned i = 1; i < 16; ++i) {
        palette[i] = (i << 24) | (i << 16);
        hashes[1] = (hashes[1] ^ palette[i]) * 16777619u;
        if (i < 4) hashes[0] = (hashes[0] ^ palette[i]) * 16777619u;
    }
    /* Eviction during this preparation can invalidate a retained colour mesh.
     * Its next preparation must recertify references, not reuse stale slots. */
    c->background.valid = false;
    if (++c->epoch == 0) { snesrecomp_colour_tiles_reset(c); c->epoch = 1; }
    SnesRecompTilePlan work = {0};
    for (unsigned i = 0; i < count; ++i) {
        const SnesRecompIndexedTileRequest *r = requests + i;
        const int slot = Tile(c, cap, palette, hashes[r->bpp == 4],
                              r->word, r->bpp, 4096u, true, &work);
        if (slot < 0) return SNESRECOMP_PPU_RESOURCE_LIMIT;
        c->indexed_slots[i] = (uint16_t)slot;
    }
    *out = (SnesRecompIndexedTilePlan){c->indexed_slots, c->tiles,
        c->generations, c->dirty, count, work.tiles_decoded, work.cache_hits,c->indexed_classes};
    return SNESRECOMP_PPU_OK;
}
/* Recolour only slots referenced by the retained mesh. The source indices and
 * transparency are unchanged; decoding tilemaps or rebuilding geometry would
 * duplicate work. Clipped-black families and unused palette entries produce
 * no uploads. Raster palette variants use the existing planner instead. */
static void RecolourRawSlots(SnesRecompColourTileCache *c,const SnesPpuFrameCapture *cap,
    SnesRecompTilePlan *plan,const SnesRecompTileQuad *quads,unsigned count) {
    const uint16_t *colours_source=cap->cgram_lines?cap->cgram_lines:cap->cgram;
    uint32_t visited[SNESRECOMP_COLOUR_TILE_SLOTS/32u]={0};
    for(unsigned qi=0;qi<count;++qi) {
        const unsigned slot=quads[qi].slot;
        if(visited[slot/32u]&(1u<<(slot%32u))) continue;
        visited[slot/32u]|=1u<<(slot%32u);
        ColourTileEntry *entry=c->entries+slot;
        if(entry->key&0x80000000u) continue; /* Owned image, independent of CGRAM. */
        const unsigned bpp=(entry->key>>15)&7u, family=entry->key>>18;
        const unsigned colours=1u<<bpp;
        uint32_t palette[16]={0},hash=2166136261u;bool changed=false;
        for(unsigned i=1;i<colours;++i) {
            const unsigned value=family&8u?0u:colours_source[((family&7u)<<bpp)+i];
            palette[i]=0xff000000u|((value&31u)<<19)|
                (((value>>5)&31u)<<11)|(((value>>10)&31u)<<3);
            changed|=palette[i]!=entry->palette[i];hash=(hash^palette[i])*16777619u;
        }
        if(!changed) continue;
        memcpy(entry->palette,palette,colours*sizeof(*palette));entry->palette_hash=hash;
        uint32_t *pixels=c->tiles+slot*64u;
        for(unsigned y=0;y<8;++y) {
            const uint32_t row=snesrecomp_ppu_unpack_4bpp((uint32_t)entry->source[y]|
                (bpp==4?(uint32_t)entry->source[y+8]<<16:0u));
            for(unsigned x=0;x<8;++x) pixels[y*8u+x]=palette[(row>>(4u*(7u-x)))&15u];
        }
        c->dirty[slot]=1;c->generations[slot]=++c->generation;++plan->tiles_decoded;
    }
}
static void RefreshRawPalette(SnesRecompColourTileCache *c,
    const SnesPpuFrameCapture *cap,SnesRecompTilePlan *plan) {
    RecolourRawSlots(c,cap,plan,c->quads,c->background.quad_count);
}

static uint16_t Entry(const SnesPpuFrameCapture *cap, const SnesPpuBgState *bg,
                      unsigned layer, unsigned tx, unsigned ty) {
    if (cap->resolved_bg[layer]) {
        const uint8_t *p = cap->resolved_bg[layer] + (((ty & 63u) * 64u + (tx & 63u)) * 4u);
        return (uint16_t)(p[0] | (p[1] << 8) | ((p[2] & 7u) << 10) |
                          ((p[3] & 3u) << 14) | ((p[3] & 4u) << 11));
    }
    tx &= bg->wide ? 63u : 31u; ty &= bg->tall ? 63u : 31u;
    unsigned screen = (ty >> 5) * (bg->wide ? 2u : 1u) + (tx >> 5);
    return cap->vram[(bg->tilemap_word_addr + screen * 1024 + (ty & 31) * 32 + (tx & 31)) & 32767u];
}

static SnesPpuUnsupported Supports(const SnesPpuFrameCapture *cap, bool mode7, bool subscreen,bool external_objects) {
    if (mode7) {
        SnesPpuUnsupported reason = snesrecomp_ppu_mode7_supports(cap);
        if (reason) return reason;
    }
    const SnesPpuLayoutState *l = &cap->layout;
    if (l->hud_split_height || l->hud_left_end || l->hud_right_start ||
        l->hud_oam_first_slot || l->hud_oam_slots || l->hud_oam_height ||
        l->hud_oam_first_slot2 || l->hud_oam_slots2 ||
        (!external_objects && !cap->obj_pixels && (l->oam_left_hint_strict || l->oam_right_hint_strict)) ||
        l->extra_left_cur > cap->canvas_extra ||
        l->extra_right_cur > cap->canvas_width - cap->canvas_extra - 256u)
        return SNES_PPU_UNSUPPORTED_LAYOUT_POLICY;
    for (unsigned bi = 0; bi < cap->band_count; ++bi) {
        const SnesPpuRasterBand *b = &cap->bands[bi];
        if (b->forced_blank) continue;
        if (b->bg_mode != (mode7 ? 7 : 1) || (b->regs[13] & 0x49u)) return SNES_PPU_UNSUPPORTED_MODE;
        if (!external_objects && ((b->main_enable | b->sub_enable) & 16u) && !cap->obj_pixels)
            return SNES_PPU_UNSUPPORTED_OBJ;
        if ((b->cgadsub & 0x3fu) && (b->cgwsel & 2u) && (b->sub_enable & 0x17u) && !mode7 && !subscreen)
            return SNES_PPU_UNSUPPORTED_SUBSCREEN;
        if (b->cgwsel & 1u) return SNES_PPU_UNSUPPORTED_COLOUR_MATH;
        if (b->mosaic & 0xf0u) return SNES_PPU_UNSUPPORTED_MOSAIC;
        if (b->brightness > 15u) return SNES_PPU_UNSUPPORTED_RASTER_STATE;
        for (unsigned i = 0; !mode7 && i < 3; ++i) {
            const SnesPpuBgState *bg = &b->bg[i];
            if (!((b->main_enable | (subscreen ? b->sub_enable : 0u)) & (1u << i))) continue;
            if (bg->char_word_addr & 4095u) return SNES_PPU_UNSUPPORTED_RASTER_STATE;
            if (bg->bpp != (i == 2 ? 2 : 4)) return SNES_PPU_UNSUPPORTED_MODE;
        }
    }
    return SNES_PPU_SUPPORTED;
}

SnesPpuUnsupported snesrecomp_ppu_mode1_raw_supports(const SnesPpuFrameCapture *cap) {
    if (!cap) return SNES_PPU_UNSUPPORTED_RASTER_STATE;
    SnesPpuUnsupported reason=Supports(cap,false,true,false);
    if(reason) return reason;
    return SNES_PPU_SUPPORTED;
}
SnesPpuUnsupported snesrecomp_ppu_mode1_indexed_supports(const SnesPpuFrameCapture *cap) {
    if(!cap) return SNES_PPU_UNSUPPORTED_RASTER_STATE;
    return Supports(cap,false,true,true);
}

static const SnesRecompWindowSpan *SampleSpan(const SnesRecompWindowSpan *spans,
    unsigned count,unsigned x) {
    for(unsigned i=0;i<count;++i)
        if(x>=spans[i].begin && x<spans[i].end) return spans+i;
    return NULL;
}
SnesRecompPpuResult snesrecomp_ppu_mode1_layer_sample(const SnesPpuFrameCapture *cap,
    const SnesPpuRasterBand *band,uint8_t layer,bool sub,uint16_t x,uint16_t y,
    bool *opaque,uint8_t *priority) {
    SnesRecompWindowSpan spans[SNESRECOMP_WINDOW_MAX_SPANS];
    const unsigned count=snesrecomp_ppu_window_spans(cap,band,spans,SNESRECOMP_WINDOW_MAX_SPANS);
    return snesrecomp_ppu_mode1_layer_sample_windows(cap,band,spans,count,layer,sub,x,y,opaque,priority);
}
static SnesRecompPpuResult LayerScreens(const SnesPpuFrameCapture *cap,
    const SnesPpuRasterBand *band,const SnesRecompWindowSpan *spans,unsigned count,
    uint8_t layer,uint8_t screens,uint16_t x,uint16_t y,bool *opaque,uint8_t *priority,uint8_t *opaque_screens) {
    if(!cap || !band || !opaque || !priority || !opaque_screens || !cap->vram || layer>=3 ||
        x>=cap->canvas_width || y>=cap->visible_height || y<band->y_begin || y>=band->y_end)
        return SNESRECOMP_PPU_INVALID;
    if(band->bg_mode!=1 || band->bg[layer].elastic_count || band->bg[layer].margin_repeats ||
        (cap->layout.layer_mirror&(1u<<layer))) return SNESRECOMP_PPU_UNSUPPORTED;
    *opaque=false;*priority=0;*opaque_screens=0;
    uint8_t active=(uint8_t)(screens&(((band->main_enable>>layer)&1u)|(((band->sub_enable>>layer)&1u)<<1)));
    if(band->forced_blank || !active) return SNESRECOMP_PPU_OK;
    const SnesPpuBgState *bg=band->bg+layer;
    const int screen=(int)x-cap->canvas_extra;
    if(screen<-(int)bg->margin_left || screen>=256+bg->margin_right) return SNESRECOMP_PPU_OK;
    if(!spans || !count || count>SNESRECOMP_WINDOW_MAX_SPANS) return SNESRECOMP_PPU_INVALID;
    const SnesRecompWindowSpan *span=SampleSpan(spans,count,x);
    if(!span) return SNESRECOMP_PPU_INVALID;
    active&=(uint8_t)(((span->mask>>layer)&1u)|(((span->mask>>(layer+3u))&1u)<<1));
    if(!active) return SNESRECOMP_PPU_OK;
    const unsigned shift=bg->big_tiles?4u:3u,mask=bg->big_tiles?1023u:511u;
    const unsigned sx=(unsigned)(screen+bg->h_scroll)&mask,sy=(y+1u+bg->v_scroll)&mask;
    const uint16_t entry=Entry(cap,bg,layer,sx>>shift,sy>>shift);
    unsigned character=entry&1023u;
    if(bg->big_tiles) character=(character+(((sx>>3)&1u)^!!(entry&0x4000u))+
        ((((sy>>3)&1u)^!!(entry&0x8000u))<<4))&1023u;
    const unsigned col=entry&0x4000u?7u-(sx&7u):sx&7u;
    const unsigned row=entry&0x8000u?7u-(sy&7u):sy&7u;
    unsigned index=0;
    for(unsigned plane=0;plane<bg->bpp;++plane) {
        const unsigned word=(bg->char_word_addr+character*bg->bpp*4u+row+(plane/2u)*8u)&32767u;
        index|=((cap->vram[word]>>(7u-col+(plane%2u)*8u))&1u)<<plane;
    }
    *opaque=index!=0;*opaque_screens=index?active:0;*priority=(uint8_t)((entry>>13)&1u);return SNESRECOMP_PPU_OK;
}
SnesRecompPpuResult snesrecomp_ppu_mode1_layer_sample_windows(const SnesPpuFrameCapture *cap,
    const SnesPpuRasterBand *band,const SnesRecompWindowSpan *spans,unsigned count,
    uint8_t layer,bool sub,uint16_t x,uint16_t y,bool *opaque,uint8_t *priority) {
    uint8_t screens;
    return LayerScreens(cap,band,spans,count,layer,(uint8_t)(sub?2u:1u),x,y,opaque,priority,&screens);
}
SnesRecompPpuResult snesrecomp_ppu_mode1_layer_screens_windows(const SnesPpuFrameCapture *cap,
    const SnesPpuRasterBand *band,const SnesRecompWindowSpan *spans,unsigned count,
    uint8_t layer,uint16_t x,uint16_t y,uint8_t *opaque_screens,uint8_t *priority) {
    bool opaque;
    return LayerScreens(cap,band,spans,count,layer,3u,x,y,&opaque,priority,opaque_screens);
}


static SnesRecompPpuResult Mode1Sample(const SnesRecompPpuFrame *frame,
    uint16_t x,uint16_t y,uint32_t *argb,uint16_t *sub_colour,bool *genuine_sub) {
    const SnesPpuFrameCapture *cap=snesrecomp_ppu_frame_capture(frame);
    if(!cap || (!argb && (!sub_colour || !genuine_sub)) || x>=cap->canvas_width || y>=cap->visible_height)
        return SNESRECOMP_PPU_INVALID;
    if(Supports(cap,false,true,false)!=SNES_PPU_SUPPORTED) return SNESRECOMP_PPU_UNSUPPORTED;
    const SnesPpuRasterBand *band=NULL;
    for(unsigned i=0;i<cap->band_count;++i)
        if(y>=cap->bands[i].y_begin && y<cap->bands[i].y_end) {band=cap->bands+i;break;}
    if(!band) return SNESRECOMP_PPU_INVALID;
    if(band->forced_blank) {
        if(argb) *argb=0xff000000u;
        if(sub_colour) *sub_colour=0;
        if(genuine_sub) *genuine_sub=false;
        return SNESRECOMP_PPU_OK;
    }
    SnesRecompWindowSpan spans[SNESRECOMP_WINDOW_MAX_SPANS];
    const unsigned count=snesrecomp_ppu_window_spans(cap,band,spans,SNESRECOMP_WINDOW_MAX_SPANS);
    const SnesRecompWindowSpan *destination=SampleSpan(spans,count,x);
    if(!destination) return SNESRECOMP_PPU_INVALID;
    const uint16_t *pal=cap->cgram_lines?cap->cgram_lines+(size_t)y*256:cap->cgram;
    unsigned winners[2]={0,0},main_source=5;
    int best[2]={-1,-1};
    for(unsigned layer=0;layer<3;++layer) {
        const SnesPpuBgState *bg=band->bg+layer;
        int screen=(int)x-cap->canvas_extra;
        if (bg->elastic_count) {
            int source = -1;
            if (screen < -(int)cap->layout.extra_left_cur ||
                screen >= 256 + (int)cap->layout.extra_right_cur) continue;
            for (unsigned i=0;i<bg->elastic_count;++i) {
                const SnesPpuElasticSegment *s=&bg->elastic[i];
                if(screen>=s->destination_begin && screen<s->destination_end) {
                    source=s->source_begin+((2*(screen-s->destination_begin)+1)*
                        (s->source_end-s->source_begin))/(2*(s->destination_end-s->destination_begin));
                    break;
                }
            }
            if(source<0) continue;
            screen=source;
        }
        if(!((band->main_enable|band->sub_enable)&(1u<<layer)) || screen<-(int)bg->margin_left ||
            screen>=256+(int)bg->margin_right) continue;
        if(!bg->elastic_count && (bg->margin_repeats || (cap->layout.layer_mirror&(1u<<layer)))) {
            if(screen<0) screen=bg->margin_repeats?256+screen:-screen;
            else if(screen>=256) screen=bg->margin_repeats?screen-256:510-screen;
        }
        const SnesRecompWindowSpan *source=SampleSpan(spans,count,(unsigned)(screen+cap->canvas_extra));
        if(!source || !(source->mask&((1u<<layer)|(1u<<(layer+3u))))) continue;
        const unsigned shift=bg->big_tiles?4u:3u,mask=bg->big_tiles?1023u:511u;
        const unsigned sx=(unsigned)(screen+bg->h_scroll)&mask,sy=(y+1u+bg->v_scroll)&mask;
        const uint16_t entry=Entry(cap,bg,layer,sx>>shift,sy>>shift);
        unsigned character=entry&1023u;
        if(bg->big_tiles) character=(character+(((sx>>3)&1u)^!!(entry&0x4000u))+
            ((((sy>>3)&1u)^!!(entry&0x8000u))<<4))&1023u;
        const unsigned col=entry&0x4000u?7u-(sx&7u):sx&7u;
        const unsigned row=entry&0x8000u?7u-(sy&7u):sy&7u;
        unsigned index=0;
        for(unsigned plane=0;plane<bg->bpp;++plane) {
            const unsigned word=(bg->char_word_addr+character*bg->bpp*4u+row+(plane/2u)*8u)&32767u;
            index|=((cap->vram[word]>>(7u-col+(plane%2u)*8u))&1u)<<plane;
        }
        if(!index) continue;
        const unsigned priority=(entry>>13)&1u;
        const unsigned pass=layer==2?(priority?(band->bg3_priority?6u:1u):0u):
            layer==1?(priority?4u:2u):(priority?5u:3u);
        for(unsigned sub=0;sub<2;++sub)
            if((source->mask&(1u<<(layer+sub*3u))) && (int)(pass*2u)>best[sub]) {
                best[sub]=(int)(pass*2u);
                winners[sub]=(((entry>>10)&7u)<<bg->bpp)+index;
                if(!sub) main_source=layer;
            }
    }
    if(cap->obj_pixels) {
        const uint16_t obj=cap->obj_pixels[(size_t)y*cap->obj_pitch+x];
        static const unsigned passes[4]={0,1,3,5};
        for(unsigned sub=0;sub<2;++sub)
            if((obj&0x800u) && (destination->obj_mask&(1u<<sub)) &&
               (int)(passes[(obj>>8)&3u]*2u+1u)>best[sub]) {
                winners[sub]=obj&255u;
                if(!sub) main_source=(obj&0x400u)?4u:6u;
            }
    }
    const bool genuine=(band->cgwsel&2u) && winners[1]!=0;
    if(sub_colour) *sub_colour=pal[winners[1]]&0x7fffu;
    if(genuine_sub) *genuine_sub=winners[1]!=0;
    const bool math=(destination->mask&SNESRECOMP_SEMANTIC_MATH) &&
        main_source<6u && (band->cgadsub&(1u<<main_source));
    if(argb) *argb=snesrecomp_ppu_compose_colour(pal[winners[0]],
        genuine?pal[winners[1]]:band->fixed_colour,
        !(destination->mask&SNESRECOMP_SEMANTIC_MAIN_RGB),math,
        (band->cgadsub&128u)!=0,(band->cgadsub&64u) && (!(band->cgwsel&2u) || genuine),
        band->brightness);
    return SNESRECOMP_PPU_OK;
}
SnesRecompPpuResult snesrecomp_ppu_mode1_sample(const SnesRecompPpuFrame *frame,
    uint16_t x,uint16_t y,uint32_t *argb) {
    if(!argb) return SNESRECOMP_PPU_INVALID;
    return Mode1Sample(frame,x,y,argb,NULL,NULL);
}
SnesRecompPpuResult snesrecomp_ppu_mode1_sub_sample(const SnesRecompPpuFrame *frame,
    uint16_t x,uint16_t y,uint16_t *colour,bool *genuine) {
    if(!colour || !genuine) return SNESRECOMP_PPU_INVALID;
    return Mode1Sample(frame,x,y,NULL,colour,genuine);
}

static bool LayerEquals(const SnesPpuRasterBand *a, const SnesPpuRasterBand *b, unsigned layer, bool raw) {
    const unsigned bit = 1u << layer;
    /* Selecting the subscreen as colour input does not change a raw main
     * plane when this BG has no subscreen output in either raster state.
     * Clip/prevent-math bits and actually enabled sub planes remain exact. */
    const unsigned colour_mask=raw && !((a->sub_enable|b->sub_enable)&bit)?253u:255u;
    const uint32_t selectors = (15u << (layer * 4)) | (15u << 20);
    const unsigned logic = 3u << (layer * 2);
    return !memcmp(&a->bg[layer], &b->bg[layer], sizeof(a->bg[layer])) &&
        a->forced_blank == b->forced_blank && (raw || a->brightness == b->brightness) &&
        (layer != 2 || a->bg3_priority == b->bg3_priority) &&
        !((a->main_enable ^ b->main_enable) & bit) &&
        !((a->main_window_enable ^ b->main_window_enable) & bit) &&
        (!raw || (!((a->sub_enable ^ b->sub_enable) & bit) &&
                  !((a->sub_window_enable ^ b->sub_window_enable) & bit))) &&
        !((a->cgwsel ^ b->cgwsel) & colour_mask) && !((a->cgadsub ^ b->cgadsub) & (bit | (raw?0u:192u))) &&
        (raw || a->fixed_colour == b->fixed_colour) && !((a->window_sel ^ b->window_sel) & selectors) &&
        a->window1_left == b->window1_left && a->window1_right == b->window1_right &&
        a->window2_left == b->window2_left && a->window2_right == b->window2_right &&
        !((a->regs[56] ^ b->regs[56]) & logic) && !((a->regs[57] ^ b->regs[57]) & 12u);
}

/* Raw passes have a unique originating BG layer. Retaining one layer can
 * reorder it relative to other layers, never within a priority/pass plane. */
static unsigned RawQuadLayer(const SnesRecompTileQuad *quad) {
    const unsigned rank=quad->pass>=SNESRECOMP_RAW_TILE_MAIN_PASSES?
        quad->pass-SNESRECOMP_RAW_TILE_MAIN_PASSES:quad->pass/2u;
    if(rank==3u || rank==5u)return 0u;
    if(rank==2u || rank==4u)return 1u;
    return 2u;
}
static bool SourcePlaneClipsMatch(const SnesRecompColourTileCache *c,
    const SnesRecompPpuJobView *job,unsigned layer,bool sub) {
    unsigned next=0;
    for(unsigned i=0;i<c->source_count;++i) {
        const struct SourceClipRect *old=c->source_clips+i;
        if(old->layer!=layer || !(old->screens&(1u<<sub)))continue;
        while(next<job->source_count && (job->sources[next].layer!=layer || !(job->sources[next].screens&(1u<<sub))))++next;
        if(next==job->source_count)return false;
        const SnesRecompPpuSource *n=job->sources+next++;
        if(old->x!=n->x || old->y!=n->y || old->width!=n->width ||
            old->height!=n->height)return false;
    }
    for(;next<job->source_count;++next)
        if(job->sources[next].layer==layer && (job->sources[next].screens&(1u<<sub)))return false;
    return true;
}

static bool WindowsEqual(const SnesPpuRasterBand *a, const SnesPpuRasterBand *b) {
    return snesrecomp_ppu_windows_equal(a,b);
}

/* Everything the coloured OBJ plane depends on except the OBJ pixels. */
static bool ObjectsContextMatch(const SnesRecompColourTileCache *c, const SnesPpuFrameCapture *cap,
    const SnesRecompPpuFrameVersions *versions) {
    const BackgroundSnapshot *old = &c->background;
    if (!old->valid || !cap->obj_pixels || !c->obj_source_present ||
        old->width != cap->canvas_width || old->height != cap->visible_height ||
        old->extra != cap->canvas_extra || old->band_count != cap->band_count ||
        memcmp(&old->layout, &cap->layout, sizeof(old->layout))) return false;
    for (unsigned bi = 0; bi < cap->band_count; ++bi) {
        const SnesPpuRasterBand *a = &old->bands[bi], *b = &cap->bands[bi];
        if (a->y_begin != b->y_begin || a->y_end != b->y_end || !WindowsEqual(a,b) ||
            a->forced_blank != b->forced_blank || a->brightness != b->brightness ||
            a->fixed_colour != b->fixed_colour || ((a->cgadsub ^ b->cgadsub) & 0xd0u) ||
            ((a->cgwsel ^ b->cgwsel) & 2u)) return false;
    }
    const unsigned palette_rows = c->obj_palette_lines || cap->cgram_lines ? cap->visible_height : 1;
    for (unsigned y = 0; !(c->object_versions.owner==versions->owner &&
        c->object_versions.palette==versions->palette) && y < palette_rows; ++y) {
        const uint16_t *pal = cap->cgram_lines ? cap->cgram_lines + y * 256 : cap->cgram;
        if (!snesrecomp_ppu_words_equal(c->obj_palettes[c->obj_palette_lines ? y : 0],pal+128,128)) return false;
    }
    return true;
}
static bool ObjectsMatch(const SnesRecompColourTileCache *c, const SnesPpuFrameCapture *cap,
    const SnesRecompPpuFrameVersions *versions) {
    if (!ObjectsContextMatch(c, cap, versions)) return false;
    return (c->object_versions.owner==versions->owner && c->object_versions.objects==versions->objects) ||
        snesrecomp_ppu_words_equal(c->obj_source,cap->obj_pixels,
                   (size_t)cap->canvas_width*cap->visible_height);
}

static void ObjectsRemember(SnesRecompColourTileCache *c, const SnesPpuFrameCapture *cap,
    const SnesRecompPpuFrameVersions *versions, const uint8_t *changed_rows) {
    c->obj_source_present = cap->obj_pixels != NULL;
    if (!cap->obj_pixels) return;
    const bool same_owner=c->object_versions.owner==versions->owner;
    const bool same_palette=same_owner && c->object_versions.palette==versions->palette &&
        c->obj_palette_lines==(cap->cgram_lines!=NULL);
    c->obj_palette_lines = cap->cgram_lines != NULL;
    for (unsigned y = 0; !same_palette && y < (c->obj_palette_lines ? cap->visible_height : 1); ++y) {
        const uint16_t *pal = cap->cgram_lines ? cap->cgram_lines + y * 256 : cap->cgram;
        memcpy(c->obj_palettes[y], pal + 128, sizeof(c->obj_palettes[y]));
    }
    if(changed_rows) {
        for (unsigned y = 0; y < cap->visible_height; ++y)
            if (changed_rows[y])
                memcpy(c->obj_source + y * cap->canvas_width, cap->obj_pixels + y * cap->canvas_width,
                       cap->canvas_width * sizeof(uint16_t));
    } else if(!(same_owner && c->object_versions.objects==versions->objects))
        memcpy(c->obj_source, cap->obj_pixels,cap->canvas_width * cap->visible_height * sizeof(uint16_t));
    c->object_versions=*versions;
}

static bool Backdrop(SnesRecompColourTileCache *c, SnesRecompTilePlan *out,
    unsigned x0, unsigned y0, unsigned x1, unsigned y1, uint32_t argb) {
    for (unsigned i = 0; i < out->backdrop_count; ++i) {
        SnesRecompTileBackdrop *r = &c->backdrops[i];
        if (r->x0 == x0 && r->x1 == x1 && r->y1 == y0 && r->argb == argb) {
            r->y1 = (uint16_t)y1; return true;
        }
    }
    if (out->backdrop_count == SNES_PPU_MAX_BANDS) return false;
    c->backdrops[out->backdrop_count++] = (SnesRecompTileBackdrop){
        (uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1, argb};
    return true;
}

#include "snes_ppu_source_tiles.inc"

/* True when every tilemap entry of the layer names an all-zero character, so
 * no scroll or window can show a texel. Fields often keep such a BG enabled.
 * Marks the whole map and the characters it names, a superset of a walk. */
static bool LayerTransparent(SnesRecompColourTileCache *c, const SnesPpuFrameCapture *cap,
    const SnesPpuBgState *bg, unsigned layer) {
    if (bg->big_tiles || (bg->bpp != 2 && bg->bpp != 4)) return false;
    const unsigned screens = (bg->wide ? 2u : 1u) * (bg->tall ? 2u : 1u);
    const unsigned words = bg->bpp * 4u;
    uint32_t checked[1024 / 32] = {0};
    for (unsigned screen = 0; screen < screens; ++screen)
        for (unsigned i = 0; i < 1024u; ++i) {
            const unsigned character =
                cap->vram[(bg->tilemap_word_addr + screen * 1024u + i) & 32767u] & 1023u;
            if (checked[character >> 5] & (1u << (character & 31u))) continue;
            const unsigned word = (bg->char_word_addr + character * words) & 32767u;
            for (unsigned w = 0; w < words; ++w)
                if (cap->vram[(word + w) & 32767u]) return false;
            checked[character >> 5] |= 1u << (character & 31u);
        }
    for (unsigned screen = 0; screen < screens; ++screen)
        for (unsigned i = 0; i < 1024u; i += 64u)
            VramUsed(&c->background, (bg->tilemap_word_addr + screen * 1024u + i) & 32767u, layer);
    for (unsigned character = 0; character < 1024u; ++character) {
        if (!(checked[character >> 5] & (1u << (character & 31u)))) continue;
        const unsigned word = (bg->char_word_addr + character * words) & 32767u;
        VramUsed(&c->background, word, layer);
        VramUsed(&c->background, (word + words - 1u) & 32767u, layer);
    }
    return true;
}

/* Repeated margins sample native columns; a gap there fails the walk. */
static bool SpansCover(const SnesRecompWindowSpan *spans, unsigned ns,
    unsigned begin, unsigned end) {
    unsigned at = begin;
    for (unsigned si = 0; si < ns && at < end; ++si) {
        if (spans[si].end <= at) continue;
        if (spans[si].begin > at) return false;
        at = spans[si].end;
    }
    return at >= end;
}

/* x_begin/x_end clip the drawn columns for partial redraws; a shifted layer
 * has already checked transparency once for the whole frame. */
static bool DrawLayer(SnesRecompColourTileCache *c, const SnesPpuFrameCapture *cap,
    const SnesPpuRasterBand *band, unsigned layer, unsigned y, unsigned end,
    const uint16_t *pal, const SnesRecompWindowSpan *spans, unsigned ns,
    SnesRecompTilePlan *out, bool raw, bool sub, int x_begin, int x_end, bool check_transparent) {
    const SnesPpuBgState *bg = &band->bg[layer];
    const bool elastic = bg->elastic_count != 0;
    const bool repeat = !elastic && bg->margin_repeats;
    const bool mirror = !elastic && !repeat && (cap->layout.layer_mirror & (1u << layer));
    if (check_transparent && !raw && !c->indexed_tiles && !elastic && !mirror && !c->source_job &&
        !cap->resolved_bg[layer] &&
        (!repeat || SpansCover(spans, ns, cap->canvas_extra, cap->canvas_extra + 256u)) &&
        LayerTransparent(c, cap, bg, layer))
        return true;
    int16_t mapped[SNESRECOMP_PPU_MAX_WIDTH];
    if (elastic && !snesrecomp_ppu_elastic_columns(bg,cap->canvas_width,cap->canvas_extra,
        cap->layout.extra_left_cur,cap->layout.extra_right_cur,mapped)) return false;
    for (unsigned si = 0; si < ns; ++si) {
        const SnesRecompWindowSpan *span = &spans[si];
        const unsigned visibility=1u<<(layer+(sub?3u:0u));
        if (!elastic && !repeat && !mirror && !(span->mask & visibility)) continue;
        /* Scroll and window sections share decoded source tiles. Reset the
         * small memo only when its actual colour/source context changes, not
         * at every section boundary. VRAM is immutable throughout this plan. */
        const bool rgb=raw?sub || (span->mask&SNESRECOMP_SEMANTIC_MAIN_RGB):
            (span->mask&SNESRECOMP_SEMANTIC_MAIN_RGB)!=0;
        const bool math=!raw && (span->mask&SNESRECOMP_SEMANTIC_MATH) &&
            (band->cgadsub&(1u<<layer));
        const bool half=math && (band->cgadsub&64u) && !(band->cgwsel&2u);
        const bool subtract=math && (band->cgadsub&128u);
        const unsigned palette_words=bg->bpp==2?32u:128u;
        unsigned memo_index=layer*4u+(sub?2u:0u)+(rgb?1u:0u);
        TileMemo *memo=c->tile_memos[memo_index];
        if(!memo && !c->memo_attempted[memo_index]) {
            c->memo_attempted[memo_index]=true;
            memo=c->tile_memos[memo_index]=calloc(1,sizeof(*memo));
        }
        const bool same_memo=memo && memo->valid && memo->char_addr==bg->char_word_addr &&
            memo->bpp==bg->bpp && memo->raw==raw && memo->indexed==c->indexed_tiles && memo->rgb==rgb &&
            memo->math==math && memo->half==half && memo->subtract==subtract &&
            (raw || (memo->brightness==band->brightness &&
                      (!math || memo->fixed==band->fixed_colour))) &&
            (c->indexed_tiles || (raw && !rgb) || snesrecomp_ppu_words_equal(memo->palette,pal,palette_words));
        if(memo && !same_memo) {
            if (++memo->stamp == 0) {
                memset(memo->stamps, 0, sizeof(memo->stamps));
                memo->stamp = 1;
            }
            memo->valid=true;memo->char_addr=bg->char_word_addr;
            memo->bpp=bg->bpp;memo->raw=raw;memo->rgb=rgb;
            memo->indexed=c->indexed_tiles;
            memo->math=math;memo->half=half;memo->subtract=subtract;
            memo->brightness=band->brightness;memo->fixed=band->fixed_colour;
            if(!c->indexed_tiles) memcpy(memo->palette,pal,palette_words*sizeof(*pal));
        }
        uint32_t palettes[8][16];
        uint32_t palette_hashes[8];
        unsigned prepared_palettes = 0;
        int left = (int)cap->canvas_extra - bg->margin_left;
        int right = (int)cap->canvas_extra + 256 + bg->margin_right;
        if(elastic) {left=(int)cap->canvas_extra-cap->layout.extra_left_cur;
            right=(int)cap->canvas_extra+256+cap->layout.extra_right_cur;}
        if (left < span->begin) left = span->begin;
        if (right > span->end) right = span->end;
        if (left < x_begin) left = x_begin;
        if (right > x_end) right = x_end;
        for (unsigned py = y; py < end;) {
            const unsigned tile_shift = bg->big_tiles ? 4u : 3u;
            const unsigned coordinate_mask = bg->big_tiles ? 1023u : 511u;
            unsigned sy = (bg->v_scroll + py + 1u) & coordinate_mask;
            unsigned row = sy & 7u, height = 8u - row;
            if (height > end - py) height = end - py;
            const unsigned covered_count=c->source_job?c->source_row_counts[sub][layer][py]:0u;
            const uint16_t *covered=c->source_rows[sub][layer][py];
            unsigned covered_at=0,last_map_block=UINT32_MAX;
            for (int px = left; px < right;) {
                /* An owned source erases this BG even where its texel is
                 * transparent. Do not decode or allocate native tiles that
                 * the final source clip would discard in their entirety. */
                while(covered_at<covered_count) {
                    const SnesRecompPpuSource *r=c->source_job->sources+covered[covered_at];
                    const int source_end=r->x+r->width;
                    if(source_end<=px || r->y+r->height<py+height) {++covered_at;continue;}
                    if(r->x>px) break;
                    px=source_end<right?source_end:right;++covered_at;
                }
                if(px>=right)break;
                int screen_x = px - cap->canvas_extra;
                bool reverse = false;
                bool repeated_pixel = false;
                unsigned segment = (unsigned)(right - px);
                if (elastic) {
                    screen_x=mapped[px];
                    if(screen_x<0) {++px;continue;}
                    const SnesRecompWindowSpan *source=SampleSpan(spans,ns,
                        (unsigned)(screen_x+cap->canvas_extra));
                    if(!source) return false;
                    if(!(source->mask&visibility)) {++px;continue;}
                    repeated_pixel=px+1<right && mapped[px+1]==screen_x;
                    segment=1;
                    while(px+(int)segment<right &&
                        mapped[px+segment]==screen_x+(repeated_pixel?0:(int)segment) &&
                        (unsigned)(mapped[px+segment]+cap->canvas_extra)<source->end) ++segment;
                } else if (repeat || mirror) {
                    if (screen_x < 0) {
                        segment = (unsigned)-screen_x;
                        screen_x = repeat ? 256 + screen_x : -screen_x;
                        reverse = mirror;
                    } else if (screen_x >= 256) {
                        screen_x = repeat ? screen_x - 256 : 510 - screen_x;
                        reverse = mirror;
                    } else segment = 256u - (unsigned)screen_x;
                    /* Padding copies the isolated native layer after its window. */
                    unsigned source_span = 0;
                    const unsigned source_column = (unsigned)(screen_x + cap->canvas_extra);
                    while (source_span < ns && source_column >= spans[source_span].end) ++source_span;
                    if (source_span == ns || source_column < spans[source_span].begin) return false;
                    unsigned available = reverse ? source_column - spans[source_span].begin + 1u :
                                                   spans[source_span].end - source_column;
                    if (segment > available) segment = available;
                    if (!(spans[source_span].mask & visibility)) {
                        if (segment > (unsigned)(right - px)) segment = (unsigned)(right - px);
                        px += (int)segment; continue;
                    }
                }
                unsigned sx = (unsigned)(screen_x + bg->h_scroll) & coordinate_mask;
                unsigned col = sx & 7u, width = reverse ? col + 1u : 8u - col;
                if(repeated_pixel) width=segment;
                if (width > segment) width = segment;
                if (width > (unsigned)(right - px)) width = (unsigned)(right - px);
                uint16_t entry = Entry(cap, bg, layer, sx >> tile_shift, sy >> tile_shift);
                if (!cap->resolved_bg[layer]) {
                    const unsigned tx=(sx>>tile_shift)&(bg->wide?63u:31u);
                    const unsigned ty=(sy>>tile_shift)&(bg->tall?63u:31u);
                    const unsigned screen=(ty>>5)*(bg->wide?2u:1u)+(tx>>5);
                    const unsigned map_word=(bg->tilemap_word_addr+screen*1024u+(ty&31u)*32u+(tx&31u))&32767u;
                    if(map_word/64u!=last_map_block) {VramUsed(&c->background,map_word,layer);last_map_block=map_word/64u;}
                }
                unsigned character = entry & 1023u;
                if (bg->big_tiles) {
                    /* Flip the entire 16x16 character before selecting a quadrant. */
                    const unsigned cell_x = ((sx >> 3) & 1u) ^ !!(entry & 0x4000u);
                    const unsigned cell_y = ((sy >> 3) & 1u) ^ !!(entry & 0x8000u);
                    character = (character + cell_x + (cell_y << 4)) & 1023u;
                }
                unsigned word = (bg->char_word_addr + character * bg->bpp * 4u) & 32767u;
                VramUsed(&c->background,word,layer);
                if((word&63u)+bg->bpp*4u>64u) VramUsed(&c->background,(word+bg->bpp*4u-1u)&32767u,layer);
                /* VRAM is fixed within a plan; a character found blank needs no slot. */
                uint32_t *const blank=bg->bpp==2?c->blank_epoch[0]+(word>>3):
                    bg->bpp==4?c->blank_epoch[1]+(word>>4):NULL;
                if(blank && *blank==c->epoch) {px+=(int)width;continue;}
                unsigned palette_number = (entry >> 10) & 7u;
                int slot=-1;
                if (memo && memo->stamps[palette_number][character] == memo->stamp) {
                    unsigned saved=memo->slots[palette_number][character];
                    ColourTileEntry *entry=&c->entries[saved];
                    /* Atlas slots can be evicted or recoloured between frames.
                     * A generation match plus exact source words certifies reuse.
                     * Retire the entry in this epoch before another tile evicts it. */
                    if(entry->valid && memo->generations[palette_number][character]==c->generations[saved] &&
                        (entry->epoch==c->epoch ||
                         snesrecomp_ppu_words_equal(entry->source,cap->vram+word,bg->bpp*4u))) {
                        slot=(int)saved;entry->epoch=c->epoch;
                    }
                }
                if(slot>=0) {
                    ++out->cache_hits;
                } else {
                    /* Shade only palettes this section actually references.
                     * Their content/hash is invariant across its tile lookups;
                     * keep the exact byte comparison in Tile for collisions. */
                    if (!(prepared_palettes & (1u << palette_number))) {
                        uint32_t hash = 2166136261u;
                        palettes[palette_number][0] = 0;
                        for (unsigned index = 1; index < (1u << bg->bpp); ++index) {
                            uint32_t colour;
                            if(c->indexed_tiles) colour=(index<<24)|(index<<16);
                            else if (raw) {
                                const unsigned value = (sub || (span->mask & SNESRECOMP_SEMANTIC_MAIN_RGB)) ?
                                    pal[(palette_number << bg->bpp) + index] : 0u;
                                colour = 0xff000000u | ((value & 31u) << 19) |
                                    (((value >> 5) & 31u) << 11) | (((value >> 10) & 31u) << 3);
                            } else colour = Colour(pal[(palette_number << bg->bpp) + index], band, span, layer);
                            palettes[palette_number][index] = colour;
                            hash = (hash ^ colour) * 16777619u;
                        }
                        palette_hashes[palette_number] = hash;
                        prepared_palettes |= 1u << palette_number;
                    }
                    slot = Tile(c, cap, palettes[palette_number], palette_hashes[palette_number],
                                word, bg->bpp, c->indexed_tiles?4096u:palette_number |
                                (raw && !sub && !(span->mask & SNESRECOMP_SEMANTIC_MAIN_RGB) ? 8u : 0u), raw, out);
                    if (memo && slot >= 0) {
                        memo->slots[palette_number][character] = (uint16_t)slot;
                        memo->stamps[palette_number][character] = memo->stamp;
                        memo->generations[palette_number][character]=c->generations[slot];
                    }
                }
                if (slot < 0) return false;
                if (!c->opaque[slot]) {
                    if(blank) *blank=c->epoch;
                    px += (int)width; continue;
                }
                if (out->quad_count >= SNESRECOMP_TILE_PLAN_QUADS) return false;
                unsigned priority = (entry >> 13) & 1u;
                unsigned pass = layer == 2 ? (priority ? (band->bg3_priority ? 6u : 1u) : 0u) :
                                layer == 1 ? (priority ? 4u : 2u) : (priority ? 5u : 3u);
                if(raw) pass=sub?SNESRECOMP_RAW_TILE_MAIN_PASSES+pass:
                    pass*2u+!!((span->mask&SNESRECOMP_SEMANTIC_MATH) && (band->cgadsub&(1u<<layer)));
                /* Field stores: a 16-byte struct copy is a memcpy call on ARM11. */
                SnesRecompTileQuad quad;
                SnesRecompTileQuad *const q = c->source_job ? &quad : c->quads + out->quad_count;
                const unsigned first_u = (entry & 0x4000u) ? 7u - col : col;
                const bool flip_x = !!(entry & 0x4000u) ^ reverse;
                const unsigned texture_width=repeated_pixel?1u:width;
                q->x0=(int16_t)px; q->y0=(int16_t)py;
                q->x1=(int16_t)(px + width); q->y1=(int16_t)(py + height);
                q->slot=(uint16_t)slot;
                q->u0=(uint8_t)(flip_x ? first_u + 1u : first_u);
                q->v0=(uint8_t)((entry & 0x8000) ? 8u - row : row);
                q->u1=(uint8_t)(flip_x ? first_u + 1u - texture_width : first_u + texture_width);
                q->v1=(uint8_t)((entry & 0x8000) ? 8u - row - height : row + height);
                q->pass=(uint8_t)pass;
                q->reserved=c->indexed_tiles?(uint8_t)((palette_number<<bg->bpp)|(rgb?0u:128u)):0;
                if(c->source_job) {
                    if(!SourceClipNative(c,out,q,layer,sub)) return false;
                } else {
                    c->quad_palettes[out->quad_count++]=(uint8_t)palette_number;
                }
                px += (int)width;
            }
            py += height;
        }
    }
    return true;
}

static bool ShiftEligible(const SnesPpuFrameCapture *cap,const SnesPpuBgState *bg,unsigned layer) {
    return !bg->elastic_count && !bg->margin_repeats && !bg->big_tiles &&
        (bg->bpp==2 || bg->bpp==4) && !(cap->layout.layer_mirror&(1u<<layer));
}
static bool ShiftContextMatches(const ShiftLayer *s,const SnesPpuFrameCapture *cap,
    const SnesPpuRasterBand *band,unsigned layer,const SnesRecompWindowSpan *span) {
    SnesPpuRasterBand was=s->band;
    was.bg[layer].h_scroll=band->bg[layer].h_scroll;
    was.bg[layer].v_scroll=band->bg[layer].v_scroll;
    return s->resolved==(cap->resolved_bg[layer]!=NULL) && s->span.begin==span->begin &&
        s->span.end==span->end && s->span.mask==span->mask && LayerEquals(&was,band,layer,false);
}

/* Moving last frame's quads of a layer by its scroll delta. Quads never
 * cross a tile edge, so whole tiles kept inside a tile-aligned rectangle plus
 * a redraw of everything else give exactly the quads of a full DrawLayer.
 * Kept cells whose map entry, character or palette changed are redrawn too.
 * All layers are prepared before any tile is drawn: only quads that stay
 * valid may pin their atlas slots, since a pinned slot counts as certified
 * for this epoch. Quads move in place field by field; this target has no
 * unaligned access and a struct copy per quad costs two memcpy calls. */
typedef struct ShiftFrame {
    int left, right, rx0, rx1, ry0, ry1, gx0, gy0, rows, columns;
    bool dirty_any;
} ShiftFrame;
enum { SHIFT_FULL, SHIFT_NOTHING, SHIFT_READY };
enum { SHIFT_EMPTY = 0xffffu, SHIFT_DIRTY = 0xfffeu };
static int ShiftLayerPrepare(SnesRecompColourTileCache *c,const SnesPpuFrameCapture *cap,
    const SnesPpuRasterBand *band,unsigned layer,const SnesRecompWindowSpan *span,
    const uint32_t *changed_blocks,unsigned changed_palettes,ShiftFrame *f) {
    const ShiftLayer *s=&c->shift[layer];
    const SnesPpuBgState *bg=&band->bg[layer],*was=&s->band.bg[layer];
    const BackgroundSnapshot *snap=&c->background;
    if(!cap->resolved_bg[layer] && LayerTransparent(c,cap,bg,layer)) return SHIFT_NOTHING;
    const int extra=cap->canvas_extra,height=cap->visible_height;
    const unsigned h=bg->h_scroll,v=bg->v_scroll+1u;
    const int dx=(int)(((h-was->h_scroll)&511u)^256u)-256;
    const int dy=(int)(((bg->v_scroll-was->v_scroll)&511u)^256u)-256;
    int left=extra-bg->margin_left,right=extra+256+bg->margin_right;
    if(left<span->begin) left=span->begin;
    if(right>span->end) right=span->end;
#define SHIFT_COL(p) (((unsigned)((p)-extra)+h)&7u)
#define SHIFT_ROW(p) (((unsigned)(p)+v)&7u)
    int rx0=left,rx1=right,ry0=0,ry1=height;
    if(dx>0) {rx1=right-dx;rx1-=(int)SHIFT_COL(rx1);}
    else if(dx<0) {rx0=left-dx;rx0+=(int)((8u-SHIFT_COL(rx0))&7u);}
    if(dy>0) {ry1=height-dy;ry1-=(int)SHIFT_ROW(ry1);}
    else if(dy<0) {ry0=-dy;ry0+=(int)((8u-SHIFT_ROW(ry0))&7u);}
    const int gx0=rx0-(int)SHIFT_COL(rx0),gy0=ry0-(int)SHIFT_ROW(ry0);
    const int columns=(rx1-gx0+7)>>3,rows=(ry1-gy0+7)>>3;
#undef SHIFT_COL
#undef SHIFT_ROW
    if(left>=right || rx0>=rx1 || ry0>=ry1 || rows>SHIFT_ROWS || columns>SHIFT_COLUMNS) return SHIFT_FULL;
    *f=(ShiftFrame){left,right,rx0,rx1,ry0,ry1,gx0,gy0,rows,columns,false};
    uint16_t (*const cells)[SHIFT_COLUMNS]=c->shift_cells[layer];
    for(int r=0;r<rows;++r) for(int col=0;col<columns;++col) cells[r][col]=SHIFT_EMPTY;
    SnesRecompTileQuad *const store=c->shift_quads[layer];
    const uint8_t *const store_palettes=c->shift_palettes[layer];
    const uint32_t count=s->count;
    for(uint32_t i=0;i<count;++i) {
        SnesRecompTileQuad *const q=store+i;
        const int ox0=q->x0-dx,ox1=q->x1-dx,oy0=q->y0-dy,oy1=q->y1-dy;
        if(ox0<rx0 || ox1>rx1 || oy0<ry0 || oy1>ry1) {
            const int x0=ox0<rx0?rx0:ox0,x1=ox1>rx1?rx1:ox1;
            const int y0=oy0<ry0?ry0:oy0,y1=oy1>ry1?ry1:oy1;
            if(x0>=x1 || y0>=y1) {q->x1=q->x0;continue;}
            const int du=q->u1>q->u0?1:-1,dv=q->v1>q->v0?1:-1;
            q->u0=(uint8_t)(q->u0+(x0-ox0)*du);q->u1=(uint8_t)(q->u1-(ox1-x1)*du);
            q->v0=(uint8_t)(q->v0+(y0-oy0)*dv);q->v1=(uint8_t)(q->v1-(oy1-y1)*dv);
            q->x0=(int16_t)x0;q->x1=(int16_t)x1;q->y0=(int16_t)y0;q->y1=(int16_t)y1;
        } else {
            q->x0=(int16_t)ox0;q->x1=(int16_t)ox1;q->y0=(int16_t)oy0;q->y1=(int16_t)oy1;
        }
        cells[(q->y0-gy0)>>3][(q->x0-gx0)>>3]=(uint16_t)i;
    }
#define SHIFT_MARK(r,col) do { \
        const uint16_t at_=cells[r][col]; \
        if(at_!=SHIFT_DIRTY) { \
            if(at_!=SHIFT_EMPTY) store[at_].x1=store[at_].x0; \
            cells[r][col]=SHIFT_DIRTY;f->dirty_any=true; \
        } \
    } while(0)
    /* A palette change only recolours quads; transparency stays the same. */
    if(changed_palettes) for(uint32_t i=0;i<count;++i) {
        const SnesRecompTileQuad *const q=store+i;
        if(q->x0!=q->x1 && ((changed_palettes>>store_palettes[i])&1u))
            SHIFT_MARK((q->y0-gy0)>>3,(q->x0-gx0)>>3);
    }
    /* Characters whose data changed since last frame. */
    const unsigned words=bg->bpp*4u;
    uint32_t changed_chars[1024/32]={0};
    bool chars_changed=false;
    for(unsigned w=0;!chars_changed && w<SNES_PPU_VRAM_WORDS/64u/32u;++w)
        chars_changed=(changed_blocks[w]&snap->layer_vram_blocks[layer][w])!=0;
    if(chars_changed)
        for(unsigned character=0;character<1024u;++character) {
            const unsigned word=(bg->char_word_addr+character*words)&32767u;
            if(((changed_blocks[word/2048u]>>((word/64u)%32u))&1u) &&
                !snesrecomp_ppu_words_equal(snap->vram+word,cap->vram+word,words))
                changed_chars[character/32u]|=1u<<(character%32u);
        }
    /* Map entries and characters of every kept cell, transparent ones included. */
    const unsigned ty0=(((unsigned)gy0+v)&511u)>>3,tx0=(((unsigned)(gx0-extra)+h)&511u)>>3;
    const uint8_t *const map=cap->resolved_bg[layer];
    if(map) {
        for(int r=0;r<rows;++r) {
            const unsigned ty=(ty0+(unsigned)r)&63u;
            const uint8_t *const now=map+ty*256u,*const then=snap->maps[layer]+ty*256u;
            for(int col=0;col<columns;++col) {
                const unsigned at=((tx0+(unsigned)col)&63u)*4u;
                const unsigned character=now[at]|((now[at+1]&3u)<<8);
                if(now[at]!=then[at] || now[at+1]!=then[at+1] || now[at+2]!=then[at+2] ||
                    now[at+3]!=then[at+3] || ((changed_chars[character/32u]>>(character%32u))&1u))
                    SHIFT_MARK(r,col);
            }
        }
    } else {
        const unsigned tx_mask=bg->wide?63u:31u,ty_mask=bg->tall?63u:31u;
        for(int r=0;r<rows;++r) {
            const unsigned ty=(ty0+(unsigned)r)&ty_mask;
            for(int col=0;col<columns;++col) {
                const unsigned tx=(tx0+(unsigned)col)&tx_mask;
                const unsigned screen=(ty>>5)*(bg->wide?2u:1u)+(tx>>5);
                const unsigned word=(bg->tilemap_word_addr+screen*1024u+(ty&31u)*32u+(tx&31u))&32767u;
                const unsigned character=cap->vram[word]&1023u;
                if((((changed_blocks[word/2048u]>>((word/64u)%32u))&1u) && snap->vram[word]!=cap->vram[word]) ||
                    ((changed_chars[character/32u]>>(character%32u))&1u))
                    SHIFT_MARK(r,col);
            }
        }
    }
#undef SHIFT_MARK
    return SHIFT_READY;
}
/* Kept quads in runs, then the dirty cells and the newly exposed edges. */
static bool ShiftLayerEmit(SnesRecompColourTileCache *c,const SnesPpuFrameCapture *cap,
    const SnesPpuRasterBand *band,unsigned layer,const SnesRecompWindowSpan *span,
    const ShiftFrame *f,SnesRecompTilePlan *out) {
    const SnesRecompTileQuad *const store=c->shift_quads[layer];
    const uint8_t *const store_palettes=c->shift_palettes[layer];
    const uint16_t (*const cells)[SHIFT_COLUMNS]=(const uint16_t (*)[SHIFT_COLUMNS])c->shift_cells[layer];
    const uint32_t count=c->shift[layer].count;
    const uint16_t *pal=cap->cgram;
    const int height=cap->visible_height;
    uint32_t n=out->quad_count;
    for(uint32_t i=0;i<count;) {
        while(i<count && store[i].x0==store[i].x1) ++i;
        uint32_t end=i;
        while(end<count && store[end].x0!=store[end].x1) ++end;
        if(end==i) break;
        if(n+(end-i)>SNESRECOMP_TILE_PLAN_QUADS) return false;
        memcpy(c->quads+n,store+i,(end-i)*sizeof *store);
        memcpy(c->quad_palettes+n,store_palettes+i,end-i);
        n+=end-i;i=end;
    }
    out->quad_count=n;
    if(f->dirty_any) for(int r=0;r<f->rows;++r) {
        const int top=f->gy0+r*8;
        const int y0=top<f->ry0?f->ry0:top,y1=top+8>f->ry1?f->ry1:top+8;
        for(int col=0;col<f->columns;) {
            if(cells[r][col]!=SHIFT_DIRTY) {++col;continue;}
            const int first=col;
            while(col<f->columns && cells[r][col]==SHIFT_DIRTY) ++col;
            const int x0=f->gx0+first*8<f->rx0?f->rx0:f->gx0+first*8;
            const int x1=f->gx0+col*8>f->rx1?f->rx1:f->gx0+col*8;
            if(!DrawLayer(c,cap,band,layer,(unsigned)y0,(unsigned)y1,pal,span,1,out,false,false,
                x0,x1,false)) return false;
        }
    }
    if(f->ry0>0 && !DrawLayer(c,cap,band,layer,0,(unsigned)f->ry0,pal,span,1,out,false,false,
        f->left,f->right,false)) return false;
    if(f->ry1<height && !DrawLayer(c,cap,band,layer,(unsigned)f->ry1,(unsigned)height,pal,span,1,out,
        false,false,f->left,f->right,false)) return false;
    if(f->rx0>f->left && !DrawLayer(c,cap,band,layer,(unsigned)f->ry0,(unsigned)f->ry1,pal,span,1,out,
        false,false,f->left,f->rx0,false)) return false;
    if(f->rx1<f->right && !DrawLayer(c,cap,band,layer,(unsigned)f->ry0,(unsigned)f->ry1,pal,span,1,out,
        false,false,f->rx1,f->right,false)) return false;
    return true;
}

/* Layers with many raster runs always use a plane. Fewer runs only pay off
 * while the map stays put, so a plane that did not last waits RETRY frames. */
enum { PLANE_NONE = 3u, PLANE_MIN_RUNS = 16u, PLANE_STABLE_MIN_RUNS = 2u,
       PLANE_RETRY_FRAMES = 60u };
static bool PlaneLayerVisible(const SnesPpuRasterBand *b,unsigned layer) {
    return !b->forced_blank && ((b->main_enable&(1u<<layer)) ||
        ((b->sub_enable&(1u<<layer)) && (b->cgwsel&2u)));
}
/* Every visible run must share one plain map: no elastic, repeated or
 * mirrored margins and no 16x16 tiles. */
static bool PlaneBandUsable(const SnesPpuFrameCapture *cap,const SnesPpuRasterBand *b,
    const SnesPpuRasterBand *first,unsigned layer) {
    const SnesPpuBgState *bg=b->bg+layer,*f=first->bg+layer;
    return !bg->elastic_count && !bg->margin_repeats && !(cap->layout.layer_mirror&(1u<<layer)) &&
        !bg->big_tiles && (bg->bpp==2 || bg->bpp==4) &&
        bg->tilemap_word_addr==f->tilemap_word_addr && bg->char_word_addr==f->char_word_addr &&
        bg->bpp==f->bpp && bg->wide==f->wide && bg->tall==f->tall;
}
static unsigned PlaneChoose(const SnesPpuFrameCapture *cap,const SnesPpuRasterBand **first_band,
    unsigned *best_runs_out) {
    unsigned best=PLANE_NONE,best_runs=PLANE_STABLE_MIN_RUNS-1u;
    for(unsigned layer=0;layer<3;++layer) {
        const SnesPpuRasterBand *first=NULL,*previous=NULL;
        unsigned runs=0;bool usable=true;
        for(unsigned bi=0;usable && bi<cap->band_count;++bi) {
            const SnesPpuRasterBand *b=cap->bands+bi;
            if(!PlaneLayerVisible(b,layer)) {previous=NULL;continue;}
            if(!first) first=b;
            usable=PlaneBandUsable(cap,b,first,layer);
            if(!previous || !LayerEquals(previous,b,layer,true)) ++runs;
            previous=b;
        }
        if(usable && first && runs>best_runs) {best=layer;best_runs=runs;*first_band=first;}
    }
    *best_runs_out=best_runs;
    return best;
}
static uint64_t PlaneRows(const SnesPpuFrameCapture *cap,unsigned layer,unsigned row_mask) {
    uint64_t rows=0;
    for(unsigned bi=0;bi<cap->band_count;++bi) {
        const SnesPpuRasterBand *b=cap->bands+bi;
        if(!PlaneLayerVisible(b,layer)) continue;
        for(unsigned y=b->y_begin;y<b->y_end;++y)
            rows|=UINT64_C(1)<<((((b->bg[layer].v_scroll+y+1u)&511u)>>3)&row_mask);
    }
    return rows;
}
static void PlaneMark(PlaneState *s,BackgroundSnapshot *background,unsigned word,unsigned layer) {
    const unsigned block=word/64u;
    s->vram_blocks[block/32u]|=1u<<(block%32u);
    VramUsed(background,word,layer);
}
/* Slots stay pinned only if no other plan ran since. */
static bool PlaneTilesMatch(const SnesRecompColourTileCache *c,const SnesPpuFrameCapture *cap,
    const SnesPpuBgState *bg,unsigned layer,uint64_t rows) {
    const PlaneState *s=c->plane;
    if(!s->tiles_valid || s->epoch!=c->epoch || s->layer!=layer ||
        s->char_addr!=bg->char_word_addr || s->tilemap_addr!=bg->tilemap_word_addr ||
        s->bpp!=bg->bpp || s->wide!=bg->wide || s->tall!=bg->tall ||
        s->resolved!=(cap->resolved_bg[layer]!=NULL) || (s->rows&rows)!=rows) return false;
    for(unsigned block=0;block<SNES_PPU_VRAM_WORDS/64u;++block)
        if((s->vram_blocks[block/32u]&(1u<<(block%32u))) &&
            !snesrecomp_ppu_words_equal(s->vram+block*64u,cap->vram+block*64u,64u)) return false;
    if(s->resolved) for(unsigned row=0;row<64;++row)
        if((s->rows&(UINT64_C(1)<<row)) &&
            memcmp(s->map+row*256u,cap->resolved_bg[layer]+row*256u,256u)) return false;
    return true;
}
static bool PlaneBuildTiles(SnesRecompColourTileCache *c,const SnesPpuFrameCapture *cap,
    const SnesPpuBgState *bg,unsigned layer,uint64_t rows,const uint16_t *pal,SnesRecompTilePlan *out) {
    PlaneState *s=c->plane;
    const bool resolved=cap->resolved_bg[layer]!=NULL;
    const unsigned columns=resolved || bg->wide?64u:32u;
    s->tiles_valid=false;s->tile_count=0;
    memset(s->vram_blocks,0,sizeof s->vram_blocks);
    memset(s->columns,0,sizeof s->columns);
    /* Same memo and colours as the raw main RGB plane of this layer. */
    const unsigned palette_words=bg->bpp==2?32u:128u,memo_index=layer*4u+1u;
    TileMemo *memo=c->tile_memos[memo_index];
    if(!memo && !c->memo_attempted[memo_index]) {
        c->memo_attempted[memo_index]=true;
        memo=c->tile_memos[memo_index]=calloc(1,sizeof(*memo));
    }
    const bool same_memo=memo && memo->valid && memo->char_addr==bg->char_word_addr &&
        memo->bpp==bg->bpp && memo->raw && memo->indexed==c->indexed_tiles && memo->rgb &&
        !memo->math && !memo->half && !memo->subtract &&
        (c->indexed_tiles || snesrecomp_ppu_words_equal(memo->palette,pal,palette_words));
    if(memo && !same_memo) {
        if (++memo->stamp == 0) {
            memset(memo->stamps, 0, sizeof(memo->stamps));
            memo->stamp = 1;
        }
        memo->valid=true;memo->char_addr=bg->char_word_addr;
        memo->bpp=bg->bpp;memo->raw=true;memo->rgb=true;
        memo->indexed=c->indexed_tiles;
        memo->math=memo->half=memo->subtract=false;
        memo->brightness=0;memo->fixed=0;
        if(!c->indexed_tiles) memcpy(memo->palette,pal,palette_words*sizeof(*pal));
    }
    uint32_t palettes[8][16]={{0}},palette_hashes[8];
    unsigned prepared_palettes=0;
    for(unsigned ty=0;ty<64;++ty) {
        if(!(rows&(UINT64_C(1)<<ty))) continue;
        for(unsigned tx=0;tx<columns;++tx) {
            const uint16_t entry=Entry(cap,bg,layer,tx,ty);
            if(!resolved) {
                const unsigned x=tx&(bg->wide?63u:31u),y=ty&(bg->tall?63u:31u);
                const unsigned screen=(y>>5)*(bg->wide?2u:1u)+(x>>5);
                PlaneMark(s,&c->background,(bg->tilemap_word_addr+screen*1024u+(y&31u)*32u+(x&31u))&32767u,layer);
            }
            const unsigned character=entry&1023u;
            const unsigned word=(bg->char_word_addr+character*bg->bpp*4u)&32767u;
            PlaneMark(s,&c->background,word,layer);
            PlaneMark(s,&c->background,(word+bg->bpp*4u-1u)&32767u,layer);
            const unsigned palette_number=(entry>>10)&7u;
            int slot=-1;
            if(memo && memo->stamps[palette_number][character]==memo->stamp) {
                const unsigned saved=memo->slots[palette_number][character];
                ColourTileEntry *e=&c->entries[saved];
                if(e->valid && memo->generations[palette_number][character]==c->generations[saved] &&
                    (e->epoch==c->epoch || snesrecomp_ppu_words_equal(e->source,cap->vram+word,bg->bpp*4u))) {
                    slot=(int)saved;e->epoch=c->epoch;
                }
            }
            if(slot>=0) ++out->cache_hits;
            else {
                if(!(prepared_palettes&(1u<<palette_number))) {
                    uint32_t hash=2166136261u;
                    for(unsigned index=1;index<(1u<<bg->bpp);++index) {
                        uint32_t colour;
                        if(c->indexed_tiles) colour=(index<<24)|(index<<16);
                        else {
                            const unsigned value=pal[(palette_number<<bg->bpp)+index];
                            colour=0xff000000u|((value&31u)<<19)|
                                (((value>>5)&31u)<<11)|(((value>>10)&31u)<<3);
                        }
                        palettes[palette_number][index]=colour;
                        hash=(hash^colour)*16777619u;
                    }
                    palette_hashes[palette_number]=hash;
                    prepared_palettes|=1u<<palette_number;
                }
                slot=Tile(c,cap,palettes[palette_number],palette_hashes[palette_number],word,bg->bpp,
                    c->indexed_tiles?4096u:palette_number,true,out);
                if(slot<0) return false;
                if(memo) {
                    memo->slots[palette_number][character]=(uint16_t)slot;
                    memo->stamps[palette_number][character]=memo->stamp;
                    memo->generations[palette_number][character]=c->generations[slot];
                }
            }
            if(!c->opaque[slot]) continue;
            if(s->tile_count==SNESRECOMP_PLANE_TILES) return false;
            const bool flip_x=(entry&0x4000u)!=0,flip_y=(entry&0x8000u)!=0;
            s->columns[(entry>>13)&1u][ty]|=UINT64_C(1)<<tx;
            s->tiles[s->tile_count++]=(SnesRecompTileQuad){(int16_t)(tx*8u),(int16_t)(ty*8u),
                (int16_t)(tx*8u+8u),(int16_t)(ty*8u+8u),(uint16_t)slot,
                (uint8_t)(flip_x?8u:0u),(uint8_t)(flip_y?8u:0u),(uint8_t)(flip_x?0u:8u),
                (uint8_t)(flip_y?0u:8u),(uint8_t)((entry>>13)&1u),0};
        }
    }
    for(unsigned block=0;block<SNES_PPU_VRAM_WORDS/64u;++block)
        if(s->vram_blocks[block/32u]&(1u<<(block%32u)))
            memcpy(s->vram+block*64u,cap->vram+block*64u,64u*sizeof(uint16_t));
    if(resolved) memcpy(s->map,cap->resolved_bg[layer],sizeof s->map);
    memcpy(s->palette,pal,sizeof s->palette);
    s->rows=rows;s->layer=(uint8_t)layer;s->bpp=bg->bpp;
    s->char_addr=bg->char_word_addr;s->tilemap_addr=bg->tilemap_word_addr;
    s->wide=bg->wide;s->tall=bg->tall;s->resolved=resolved;
    s->width=(uint16_t)(columns*8u);s->height=(uint16_t)(resolved || bg->tall?512u:256u);
    s->epoch=c->epoch;++s->tile_generation;s->tiles_valid=true;
    return true;
}
/* Retained map tiles follow a uniform palette change in place. */
static void PlaneRecolour(SnesRecompColourTileCache *c,const SnesPpuFrameCapture *cap,
    const uint16_t *pal,SnesRecompTilePlan *out) {
    PlaneState *s=c->plane;
    if(snesrecomp_ppu_words_equal(s->palette,pal,128)) return;
    RecolourRawSlots(c,cap,out,s->tiles,s->tile_count);
    memcpy(s->palette,pal,sizeof s->palette);
    ++s->tile_generation; /* The map texture must be redrawn. */
}
static void PlaneKeepMarks(SnesRecompColourTileCache *c) {
    const PlaneState *s=c->plane;
    for(unsigned word=0;word<SNES_PPU_VRAM_WORDS/64u/32u;++word) {
        c->background.vram_blocks[word]|=s->vram_blocks[word];
        c->background.layer_vram_blocks[s->layer][word]|=s->vram_blocks[word];
    }
}
/* Quads stop at the map edge; no repeating sampler needed. */
static bool PlaneEmit(PlaneState *s,const SnesPpuFrameCapture *cap,const SnesPpuBgState *bg,
    int x0,int x1,unsigned y0,unsigned y1,const uint8_t pass[2]) {
    for(unsigned y=y0;y<y1;) {
        const unsigned v=((bg->v_scroll+y+1u)&511u)&(s->height-1u);
        unsigned rows=s->height-v;
        if(rows>y1-y) rows=y1-y;
        for(int x=x0;x<x1;) {
            const unsigned u=((unsigned)(x-(int)cap->canvas_extra+(int)bg->h_scroll)&511u)&(s->width-1u);
            int columns=(int)(s->width-u);
            if(columns>x1-x) columns=x1-x;
            /* Only tile columns holding opaque tiles of a priority draw it. */
            for(unsigned priority=0;priority<2;++priority) {
                uint64_t used=0;
                for(unsigned ty=v>>3;ty<=(v+rows-1u)>>3;++ty) used|=s->columns[priority][ty];
                for(unsigned tx=u>>3;tx<=(u+(unsigned)columns-1u)>>3;) {
                    if(!(used>>tx&1u)) {++tx;continue;}
                    unsigned end=tx;
                    while(end+1u<=(u+(unsigned)columns-1u)>>3 && (used>>(end+1u)&1u)) ++end;
                    const unsigned left=tx*8u>u?tx*8u:u;
                    const unsigned right=(end+1u)*8u<u+(unsigned)columns?(end+1u)*8u:u+(unsigned)columns;
                    if(s->quad_count==SNESRECOMP_PLANE_QUADS) return false;
                    SnesRecompPlaneQuad quad={(int16_t)(x+(int)(left-u)),(int16_t)y,
                        (int16_t)(x+(int)(right-u)),(int16_t)(y+rows),(uint16_t)left,(uint16_t)v,
                        {SNESRECOMP_PLANE_PASS_NONE,SNESRECOMP_PLANE_PASS_NONE},0};
                    quad.pass[priority]=pass[priority];
                    s->quads[s->quad_count++]=quad;
                    tx=end+1u;
                }
            }
            x+=columns;
        }
        y+=rows;
    }
    return true;
}
/* Same runs, spans, margins and source clipping as the native layer. */
static bool PlaneBuildQuads(SnesRecompColourTileCache *c,const SnesPpuFrameCapture *cap,unsigned layer) {
    PlaneState *s=c->plane;
    s->quad_count=0;
    for(unsigned screen=0;screen<2;++screen)
        for(unsigned bi=0;bi<cap->band_count;) {
            const unsigned first=bi;
            SnesPpuRasterBand merged=cap->bands[bi++];
            while(bi<cap->band_count && LayerEquals(&merged,&cap->bands[bi],layer,true) &&
                snesrecomp_ppu_resolved_windows_equal(cap,first,bi))
                merged.y_end=cap->bands[bi++].y_end;
            if(merged.forced_blank || !((screen?merged.sub_enable:merged.main_enable)&(1u<<layer)) ||
                (screen && !(merged.cgwsel&2u))) continue;
            const SnesPpuBgState *bg=merged.bg+layer;
            const SnesRecompWindowSpan *spans=c->windows[first];
            const unsigned ns=c->window_counts[first];
            const unsigned low=layer==2?0u:layer==1?2u:3u;
            const unsigned high=layer==2?(merged.bg3_priority?6u:1u):layer==1?4u:5u;
            for(unsigned si=0;si<ns;++si) {
                const SnesRecompWindowSpan *span=spans+si;
                if(!(span->mask&(1u<<(layer+(screen?3u:0u))))) continue;
                /* Clipped-black main colours use separate atlas families. */
                if(!screen && !(span->mask&SNESRECOMP_SEMANTIC_MAIN_RGB)) return false;
                int left=(int)cap->canvas_extra-bg->margin_left;
                int right=(int)cap->canvas_extra+256+bg->margin_right;
                if(left<span->begin) left=span->begin;
                if(right>span->end) right=span->end;
                if(left>=right) continue;
                const unsigned math=!screen && (span->mask&SNESRECOMP_SEMANTIC_MATH) &&
                    (merged.cgadsub&(1u<<layer));
                const uint8_t pass[2]={
                    (uint8_t)(screen?SNESRECOMP_RAW_TILE_MAIN_PASSES+low:low*2u+math),
                    (uint8_t)(screen?SNESRECOMP_RAW_TILE_MAIN_PASSES+high:high*2u+math)};
                for(unsigned y=merged.y_begin;y<merged.y_end;) {
                    const unsigned count=c->source_job?c->source_row_counts[screen][layer][y]:0u;
                    const uint16_t *row=c->source_rows[screen][layer][y];
                    unsigned end=y+1u;
                    if(!c->source_job) end=merged.y_end;
                    else while(end<merged.y_end && c->source_row_counts[screen][layer][end]==count &&
                        !memcmp(row,c->source_rows[screen][layer][end],count*sizeof(*row))) ++end;
                    int x=left;
                    for(unsigned i=0;i<count && x<right;++i) {
                        const SnesRecompPpuSource *r=c->source_job->sources+row[i];
                        if(r->x+r->width<=x) continue;
                        if(r->x>=right) break;
                        if(!PlaneEmit(s,cap,bg,x,r->x<right?r->x:right,y,end,pass)) return false;
                        x=r->x+r->width;
                    }
                    if(!PlaneEmit(s,cap,bg,x,right,y,end,pass)) return false;
                    y=end;
                }
            }
        }
    return true;
}

/* Compose only qualified subscreen OBJ pixels that change the main result.
 * The background remains GPU-generated. Sparse final pixels share the retained
 * OBJ texture through reserved alpha 15, after the ordinary priority ladder. */
static bool Mode7Subscreen(SnesRecompColourTileCache *c,const SnesPpuFrameCapture *cap,
    const uint64_t *coverage) {
    enum { MAX_SPARSE_PIXELS = 8192 };
    SnesRecompMode7Line lines[SNES_PPU_MAX_BANDS];
    if (!snesrecomp_ppu_mode7_compile_lines(cap,lines,SNES_PPU_MAX_BANDS)) return false;
    for (unsigned bi=0;bi<cap->band_count;++bi) {
        const SnesPpuRasterBand *b=&cap->bands[bi];
        if (b->forced_blank || !(b->cgwsel&2u) || !(b->sub_enable&16u) || !(b->cgadsub&0x31u)) continue;
        for (unsigned y=b->y_begin;y<b->y_end;++y) {
            const uint16_t *pal=cap->cgram_lines?cap->cgram_lines+y*256:cap->cgram;
            for (unsigned si=0;si<c->window_counts[bi];++si) {
                const SnesRecompWindowSpan *span=&c->windows[bi][si];
                if (!(span->obj_mask&SNESRECOMP_SEMANTIC_OBJ_SUB) || !(span->mask&SNESRECOMP_SEMANTIC_MATH)) continue;
                for (unsigned x=span->begin;x<span->end;++x) {
                    if(!(coverage[y]&(UINT64_C(1)<<(x>>3)))) {
                        x=((x>>3)+1u)*8u-1u;continue;
                    }
                    uint16_t obj=cap->obj_pixels[y*cap->obj_pitch+x];
                    if (!(obj&0x800u)) continue;
                    int screen=(int)x-cap->canvas_extra;
                    unsigned bg=0;
                    if ((span->mask&SNESRECOMP_SEMANTIC_BG1_MAIN) &&
                        screen>=-(int)b->bg[0].margin_left && screen<256+b->bg[0].margin_right) {
                        unsigned sx=(unsigned)((int64_t)lines[y].start_x+(int64_t)lines[y].step_x*screen)&0x3ffffu;
                        unsigned sy=(unsigned)((int64_t)lines[y].start_y+(int64_t)lines[y].step_y*screen)&0x3ffffu;
                        unsigned character=cap->vram[(sy>>11)*128+(sx>>11)]&255u;
                        bg=cap->vram[character*64+((sy>>8)&7u)*8+((sx>>8)&7u)]>>8;
                    }
                    unsigned index=bg,source=bg?0:5;
                    if ((span->obj_mask&SNESRECOMP_SEMANTIC_OBJ_MAIN) && (!bg || ((obj>>8)&3u)>0)) {
                        index=obj&255u;source=obj&0x400u?4:6;
                    }
                    if (source>=6 || !(b->cgadsub&(1u<<source))) continue;
                    if (c->obj_overlay_pixels==MAX_SPARSE_PIXELS) return false;
                    uint32_t argb=snesrecomp_ppu_compose_colour(pal[index],pal[obj&255u],
                        !(span->mask&SNESRECOMP_SEMANTIC_MAIN_RGB),true,(b->cgadsub&128u)!=0,
                        (b->cgadsub&64u)!=0,b->brightness);
                    c->obj_pixels[y*cap->canvas_width+x]=(argb&0xffffffu)|0x0f000000u;
                    ++c->obj_overlay_pixels;
                }
            }
        }
    }
    return true;
}


/* A zero native layer is a NEGATIVE result over a recorded sampling domain,
 * not proof that its entire tilemap is blank. Preserve that domain across
 * margin contraction. Dependencies stay in the owned VRAM snapshot even if
 * a later viewport no longer visits the outer samples. */
static bool NegativeNoSource(const SnesRecompPpuJobView *job,unsigned layer) {
    if(job)for(unsigned i=0;i<job->source_count;++i)if(job->sources[i].layer==layer)return false;
    return true;
}
static void NegativeValidate(SnesRecompColourTileCache *c,const SnesPpuFrameCapture *cap,
    const SnesRecompPpuFrameVersions *v) {
    NegativeRaster *n=c->negative;
    if(!n || !n->valid)return;
    if(!c->background.valid){n->valid=false;return;}
    if(c->background.versions.owner==v->owner && c->background.versions.vram==v->vram)return;
    for(unsigned block=0;block<SNES_PPU_VRAM_WORDS/64u;++block)
        if((n->dependencies[block/32u]&(1u<<(block%32u))) &&
           !snesrecomp_ppu_words_equal(c->background.vram+block*64u,cap->vram+block*64u,64u)) {
            n->valid=false;return;
        }
}
static bool NegativeBandMatches(const SnesPpuRasterBand *old,const SnesPpuRasterBand *now,unsigned layer) {
    const unsigned bit=1u<<layer;
    if(now->bg[layer].margin_left>old->bg[layer].margin_left ||
       now->bg[layer].margin_right>old->bg[layer].margin_right)return false;
    SnesPpuBgState bg=old->bg[layer];
    bg.margin_left=now->bg[layer].margin_left;bg.margin_right=now->bg[layer].margin_right;
    if(memcmp(&bg,now->bg+layer,sizeof bg) || old->forced_blank!=now->forced_blank ||
       ((old->main_enable^now->main_enable)&bit) || ((old->sub_enable^now->sub_enable)&bit) ||
       ((old->main_window_enable^now->main_window_enable)&bit) ||
       ((old->sub_window_enable^now->sub_window_enable)&bit) || ((old->cgwsel^now->cgwsel)&2u))return false;
    /* Colour/brightness/priority cannot make a missing index nontransparent.
     * BG clipping CAN hide a nonzero index, so preserve its complete policy. */
    if((old->main_window_enable|old->sub_window_enable)&bit) {
        if(((old->window_sel^now->window_sel)&(15u<<(layer*4u))) ||
           old->window1_left!=now->window1_left || old->window1_right!=now->window1_right ||
           old->window2_left!=now->window2_left || old->window2_right!=now->window2_right ||
           ((old->regs[56]^now->regs[56])&(3u<<(layer*2u))))return false;
    }
    return true;
}

/* Only the sampling projection must match for a proven zero layer. A changed
 * window or disabled screen is safe when ALL new visible intervals were
 * already visited. Colour-window state cannot make index zero opaque. */
static bool NegativeWindowSubset(const NegativeRaster *n,const SnesPpuFrameCapture *cap,
    unsigned a,unsigned b,unsigned layer) {
    const SnesPpuRasterBand *old=n->bands+a,*now=cap->bands+b;
    if(old->bg[layer].elastic_count || now->bg[layer].elastic_count ||
       old->bg[layer].margin_repeats || now->bg[layer].margin_repeats ||
       ((n->layout.layer_mirror|cap->layout.layer_mirror)&(1u<<layer)))return false;
    SnesPpuBgState bg=old->bg[layer];
    bg.margin_left=now->bg[layer].margin_left;bg.margin_right=now->bg[layer].margin_right;
    if(memcmp(&bg,now->bg+layer,sizeof bg))return false;
    SnesPpuFrameCapture prior={.bands=n->bands,.band_count=n->count,.native_width=256,
        .canvas_width=n->width,.visible_height=n->height,.canvas_extra=n->extra,.layout=n->layout,
        .resolved_windows=n->resolved?n->windows:NULL};
    SnesRecompWindowSpan before[SNESRECOMP_WINDOW_MAX_SPANS],after[SNESRECOMP_WINDOW_MAX_SPANS];
    const unsigned nb=snesrecomp_ppu_window_spans(&prior,old,before,SNESRECOMP_WINDOW_MAX_SPANS);
    const unsigned nn=snesrecomp_ppu_window_spans(cap,now,after,SNESRECOMP_WINDOW_MAX_SPANS);
    if(!nb || !nn)return false;
    for(unsigned sub=0;sub<2;++sub) {
        const unsigned bit=1u<<(layer+sub*3u);
        const bool active=!now->forced_blank && ((sub?now->sub_enable:now->main_enable)&(1u<<layer)) &&
            (!sub || (now->cgwsel&2u));
        if(!active)continue;
        const bool was_active=!old->forced_blank && ((sub?old->sub_enable:old->main_enable)&(1u<<layer)) &&
            (!sub || (old->cgwsel&2u));
        for(unsigned i=0;i<nn;++i) if(after[i].mask&bit) {
            int x=(int)cap->canvas_extra-now->bg[layer].margin_left;
            int right=(int)cap->canvas_extra+256+now->bg[layer].margin_right;
            if(x<after[i].begin)x=after[i].begin;
            if(right>after[i].end)right=after[i].end;
            for(unsigned j=0;was_active && x<right && j<nb;++j) if(before[j].mask&bit) {
                int left=(int)n->extra-old->bg[layer].margin_left;
                int end=(int)n->extra+256+old->bg[layer].margin_right;
                if(left<before[j].begin)left=before[j].begin;
                if(end>before[j].end)end=before[j].end;
                if(end<=x)continue;
                if(left>x)break;
                x=end;
            }
            if(x<right)return false;
        }
    }
    return true;
}

static bool NegativeMatches(const NegativeRaster *n,const SnesPpuFrameCapture *cap,
    const SnesRecompPpuJobView *job,unsigned layer) {
    if(!n || !n->valid || n->layer!=layer || n->width!=cap->canvas_width ||
       n->height!=cap->visible_height || n->extra!=cap->canvas_extra || cap->resolved_bg[layer] ||
       n->resolved!=(cap->resolved_windows!=NULL) || !NegativeNoSource(job,layer) ||
       !RawPlaneLayoutMatches(&n->layout,&cap->layout,1u<<layer))return false;
    unsigned a=0,b=0,y=0;
    while(a<n->count && b<cap->band_count) {
        const SnesPpuRasterBand *old=n->bands+a,*now=cap->bands+b;
        if(old->y_begin>y || now->y_begin>y || old->y_end<=y || now->y_end<=y ||
           !((NegativeBandMatches(old,now,layer) &&
              (!n->resolved || !memcmp(n->windows+a*6u+layer,
                  cap->resolved_windows+b*6u+layer,sizeof(*n->windows)))) ||
             NegativeWindowSubset(n,cap,a,b,layer)))return false;
        y=old->y_end<now->y_end?old->y_end:now->y_end;
        if(y==old->y_end)++a;
        if(y==now->y_end)++b;
    }
    return y==cap->visible_height && a==n->count && b==cap->band_count;
}
static void NegativeRemember(SnesRecompColourTileCache *c,const SnesPpuFrameCapture *cap,
    const SnesRecompPpuJobView *job,unsigned empty,unsigned covered) {
    NegativeRaster *n=c->negative;if(!n)return;
    /* Keep a previously wider proof, including while another raster state is
     * temporarily nonempty. NegativeValidate has already checked its data. */
    if(n->valid && (!(empty&(1u<<n->layer)) || (covered&(1u<<n->layer)))) {
        for(unsigned i=0;i<sizeof(n->dependencies)/sizeof(uint32_t);++i) {
            c->background.layer_vram_blocks[n->layer][i]|=n->dependencies[i];
            c->background.vram_blocks[i]|=n->dependencies[i];
        }
        return;
    }
    for(unsigned li=3;li;--li) {
        const unsigned layer=li-1u;
        if(!(empty&(1u<<layer)) || cap->resolved_bg[layer] || !NegativeNoSource(job,layer))continue;
        n->layer=(uint8_t)layer;n->width=cap->canvas_width;n->height=cap->visible_height;
        n->extra=cap->canvas_extra;n->layout=cap->layout;n->count=cap->band_count;
        n->resolved=cap->resolved_windows!=NULL;
        memcpy(n->bands,cap->bands,n->count*sizeof(*n->bands));
        if(n->resolved)memcpy(n->windows,cap->resolved_windows,n->count*6u*sizeof(*n->windows));
        memcpy(n->dependencies,c->background.layer_vram_blocks[layer],sizeof(n->dependencies));
        n->valid=true;return;
    }
}

static SnesRecompPpuResult ColourPlan(SnesRecompColourTileCache *c,
    const SnesRecompPpuFrame *frame, SnesRecompTilePlan *out, bool mode7, bool raw,
    const SnesRecompPpuJobView *job, bool indexed) {
    if (!c || !out) return SNESRECOMP_PPU_INVALID;
    memset(out, 0, sizeof(*out));
    const SnesPpuFrameCapture *cap = snesrecomp_ppu_frame_capture(frame);
    const uint64_t *coverage=snesrecomp_ppu_frame_obj_coverage(frame);
    const SnesRecompPpuFrameVersions *versions=snesrecomp_ppu_frame_versions(frame);
    if (!cap) return SNESRECOMP_PPU_INVALID;
    out->shade_brightness = 15;
    /* A fade changes INIDISP every frame. Planning it at brightness 15 keeps
     * tiles and geometry cached; the backend then applies the one uniform
     * level to the finished image, which is exact for expanded RGB5. */
    SnesPpuFrameCapture unshaded;
    if(c->unshaded_bands && !raw && !indexed && cap->band_count<=SNES_PPU_MAX_BANDS) {
        int level=-1;
        for(unsigned bi=0;bi<cap->band_count;++bi) {
            const SnesPpuRasterBand *b=cap->bands+bi;
            if(b->forced_blank) continue;
            if(level<0) level=b->brightness;
            else if(level!=b->brightness) {level=-1;break;}
        }
        if(level>=0 && level<15) {
            memcpy(c->unshaded_bands,cap->bands,cap->band_count*sizeof *c->unshaded_bands);
            /* regs[0] is INIDISP; keep the raw copy consistent with the band. */
            for(unsigned bi=0;bi<cap->band_count;++bi) {
                c->unshaded_bands[bi].brightness=15;
                c->unshaded_bands[bi].regs[0]=(uint8_t)(c->unshaded_bands[bi].regs[0]|15u);
            }
            unshaded=*cap;unshaded.bands=c->unshaded_bands;cap=&unshaded;
            out->shade_brightness=(uint8_t)level;
        }
    }
    out->unsupported = indexed?snesrecomp_ppu_mode1_indexed_supports(cap):
        raw?snesrecomp_ppu_mode1_raw_supports(cap):Supports(cap, mode7, false,false);
    if (out->unsupported) return SNESRECOMP_PPU_UNSUPPORTED;
    if(c->raw_tiles!=raw || c->indexed_tiles!=indexed) {
        c->background.valid=false;c->obj_source_present=false;
        if(c->plane)c->plane->tiles_valid=c->plane->active=false;
        c->obj_overlay_pixels=0;c->raw_tiles=raw;
        c->indexed_tiles=indexed;
        for(unsigned i=0;i<12;++i) if(c->tile_memos[i]) c->tile_memos[i]->valid=false;
    }
#if defined(SNESRECOMP_PPU_TILE_COSTS)
    const bool profile_costs=getenv("LUFIA2_PPU_JOB_TRACE_ROOT")!=NULL;
    uint64_t profile_layers[6]={0};uint32_t profile_quads[6]={0};
    uint64_t profile_times[5]={0},profile_mark=profile_costs?snesrecomp_now_us():0;
#endif
    bool uniform_palette=indexed || !cap->cgram_lines;
    if(raw && !indexed && cap->cgram_lines) {
        if(c->background.valid && c->background.versions.owner==versions->owner &&
            c->background.versions.palette==versions->palette) uniform_palette=c->background.uniform_palette;
        else {
            uniform_palette=true;
            for(unsigned y=1;uniform_palette && y<cap->visible_height;++y)
                uniform_palette=snesrecomp_ppu_words_equal(cap->cgram_lines,cap->cgram_lines+y*256u,128);
        }
    }
    const unsigned source_count=job?job->source_count:0u;
    if(raw && !indexed && !mode7 && source_count)NegativeValidate(c,cap,versions);
    else if(c->negative)c->negative->valid=false;
    const bool same_sources=c->source_count==source_count && (!source_count ||
        (c->source_owner==job->owner && c->source_geometry==job->geometry_version));
    const bool mesh_matches=BackgroundMatches(&c->background,cap,versions,raw,uniform_palette,7u,3u);
    bool reuse = same_sources && mesh_matches;
    if(reuse && source_count && memcmp(c->source_versions,job->image_versions,job->image_count*sizeof(uint64_t)))
        reuse=SourceRefresh(c,job,out);
    bool reuse_native=!reuse && raw && !indexed && !mode7 && source_count &&
        mesh_matches && c->source_count==source_count;
    for(unsigned i=0;reuse_native && i<source_count;++i) {
        const SnesRecompPpuSource *n=job->sources+i;
        const struct SourceClipRect *o=c->source_clips+i;
        reuse_native=o->x==n->x && o->y==n->y && o->width==n->width &&
            o->height==n->height && o->layer==n->layer && o->screens==n->screens;
    }
    /* Plane layers have no native quads to retain. */
    const SnesPpuRasterBand *plane_first=NULL;
    unsigned plane_runs=0;
    unsigned plane_layer=c->plane && raw && !indexed && !mode7 && uniform_palette?
        PlaneChoose(cap,&plane_first,&plane_runs):PLANE_NONE;
    const unsigned previous_plane=c->plane && c->plane->active?c->plane->layer:PLANE_NONE;
    uint64_t plane_rows=plane_layer!=PLANE_NONE?PlaneRows(cap,plane_layer,
        cap->resolved_bg[plane_layer] || plane_first->bg[plane_layer].tall?63u:31u):0u;
    if(plane_layer!=PLANE_NONE && plane_runs<PLANE_MIN_RUNS &&
        !(previous_plane==plane_layer && (reuse ||
            PlaneTilesMatch(c,cap,plane_first->bg+plane_layer,plane_layer,plane_rows)))) {
        if(c->plane->retry) {--c->plane->retry;plane_layer=PLANE_NONE;plane_rows=0;}
        else c->plane->retry=PLANE_RETRY_FRAMES;
    }
    if(plane_layer!=previous_plane) reuse=reuse_native=false;
    const unsigned plane_mask=(plane_layer!=PLANE_NONE?1u<<plane_layer:0u)|
        (previous_plane!=PLANE_NONE?1u<<previous_plane:0u);
    const uint16_t *plane_palette=cap->cgram_lines?cap->cgram_lines:cap->cgram;
    bool plane_keep=plane_layer!=PLANE_NONE && !reuse &&
        PlaneTilesMatch(c,cap,plane_first->bg+plane_layer,plane_layer,plane_rows);
    if(reuse_native && plane_layer!=PLANE_NONE && !plane_keep) reuse_native=false;
    /* A BG or subscreen change must not regenerate unchanged raw planes.
     * Start conservatively: captured memory/maps and resolved windows
     * match each retained layer, palettes are uniform, and source clipping is exact per
     * retained layer. Unsupported/raster-memory cases keep full preparation. */
    unsigned retained_layers=0,retained_planes=0,negative_layers=0;
    if(!reuse && !reuse_native && raw && !indexed && !mode7 && source_count &&
        uniform_palette && c->background.uniform_palette) {
        for(unsigned layer=0;layer<3;++layer)
            if(NegativeMatches(c->negative,cap,job,layer)) {
                retained_layers|=1u<<layer;negative_layers|=1u<<layer;
            }
        int memory[3]={-1,-1,-1};
        for(unsigned sub=0;sub<2;++sub)for(unsigned layer=0;layer<3;++layer) {
            if(!(negative_layers&(1u<<layer)) &&
                BackgroundRasterMatches(&c->background,cap,true,1u<<layer,1u<<sub) &&
                (memory[layer]>=0?memory[layer]:(memory[layer]=
                    BackgroundMemoryMatches(&c->background,cap,versions,true,true,1u<<layer))) &&
                SourcePlaneClipsMatch(c,job,layer,sub)) {
                retained_layers|=1u<<layer;retained_planes|=1u<<(sub*3u+layer);
            }
    }
    }
    retained_layers&=~plane_mask;negative_layers&=~plane_mask;
    retained_planes&=~(plane_mask|plane_mask<<3);
    bool reuse_windows=reuse && c->background.band_count==cap->band_count;
    for(unsigned i=0;reuse_windows && i<cap->band_count;++i)
        reuse_windows=c->background.bands[i].y_begin==cap->bands[i].y_begin &&
            c->background.bands[i].y_end==cap->bands[i].y_end;
    c->source_job=source_count?job:NULL;
    if(!reuse && source_count) SourceRows(c,job);
    if((reuse || reuse_native || retained_layers) && raw && !indexed && uniform_palette && c->background.uniform_palette &&
        !(c->background.versions.owner==versions->owner && c->background.versions.palette==versions->palette)) {
        RefreshRawPalette(c,cap,out);
        memcpy(c->background.palettes[0],cap->cgram_lines?cap->cgram_lines:cap->cgram,
            sizeof c->background.palettes[0]);
        c->background.palette_lines=false;
    }
    bool subscreen=false;
    for (unsigned bi=0;mode7 && bi<cap->band_count;++bi) {
        const SnesPpuRasterBand *b=&cap->bands[bi];
        subscreen |= !b->forced_blank && (b->cgwsel&2u) && (b->sub_enable&16u) && (b->cgadsub&0x31u);
    }
    TILE_COST_MARK(0);
    const bool objects_context = !raw && ObjectsContextMatch(c, cap, versions);
    const bool reuse_objects = raw || (objects_context && ObjectsMatch(c, cap, versions) &&
        (!(subscreen || c->obj_overlay_pixels) || reuse));
    /* Walking animates a few sprites; the rest of the coloured plane, made
     * from identical input under the same context, is already correct. */
    uint8_t changed_object_rows[SNES_PPU_MAX_BANDS];
    const bool object_rows = !reuse_objects && objects_context && !mode7 && !subscreen &&
        !c->obj_overlay_pixels && cap->obj_pixels && cap->obj_pitch == cap->canvas_width;
    if (!reuse_objects) {
        c->obj_source_present=false;c->obj_overlay_pixels=0;
        if (object_rows) {
            for (unsigned y = 0; y < cap->visible_height; ++y) {
                const size_t at = (size_t)y * cap->canvas_width;
                changed_object_rows[y] = !snesrecomp_ppu_words_equal(c->obj_source + at,
                    cap->obj_pixels + at, cap->canvas_width);
                if (changed_object_rows[y])
                    memset(c->obj_pixels + at, 0, cap->canvas_width * sizeof(*c->obj_pixels));
            }
        } else if(cap->obj_pixels) memset(c->obj_pixels,0,
            (size_t)cap->canvas_width*cap->visible_height*sizeof(*c->obj_pixels));
    }
    /* Direct layers that only scrolled keep last frame's quads; see ShiftLayerDraw. */
    const bool shift_frame=!raw && !indexed && !mode7 && !source_count && !cap->cgram_lines;
    unsigned shift_try=0,shift_palettes[3]={0};
    uint32_t shift_changed[SNES_PPU_VRAM_WORDS/64u/32u]={0};
    const BackgroundSnapshot *snap=&c->background;
    if(!reuse && shift_frame && snap->valid && c->shift_epoch==c->epoch && snap->palette_recorded &&
        !snap->palette_lines && snap->width==cap->canvas_width && snap->height==cap->visible_height &&
        snap->extra==cap->canvas_extra)
        for(unsigned layer=0;layer<3;++layer) if(c->shift[layer].valid) shift_try|=1u<<layer;
    if(shift_try) {
        if(!(snap->vram_owner==versions->owner && snap->vram_version==versions->vram))
            for(unsigned word=0;word<SNES_PPU_VRAM_WORDS/64u/32u;++word) {
                uint32_t marks=0;
                for(unsigned layer=0;layer<3;++layer)
                    if(shift_try&(1u<<layer)) marks|=snap->layer_vram_blocks[layer][word];
                while(marks) {
                    const unsigned bit=(unsigned)__builtin_ctz(marks),block=word*32u+bit;
                    marks&=marks-1u;
                    if(!snesrecomp_ppu_words_equal(snap->vram+block*64u,cap->vram+block*64u,64u))
                        shift_changed[word]|=1u<<bit;
                }
            }
        for(unsigned entry=1;entry<128u;++entry) {
            if(snap->palettes[0][entry]==cap->cgram[entry]) continue;
            for(unsigned layer=0;layer<3;++layer) {
                const unsigned bpp=c->shift[layer].band.bg[layer].bpp;
                if(bpp==4 && (entry&15u)) shift_palettes[layer]|=1u<<(entry>>4);
                else if(bpp==2 && entry<32u && (entry&3u)) shift_palettes[layer]|=1u<<(entry>>2);
            }
        }
    }
    if (!reuse) {
        c->background.valid = false;
        if (!reuse_native) {
            for(unsigned layer=0;layer<3;++layer)if(!((retained_layers|shift_try)&(1u<<layer)))
                memset(c->background.layer_vram_blocks[layer],0,sizeof c->background.layer_vram_blocks[layer]);
            if(!(retained_layers|shift_try))memset(c->background.vram_blocks,mode7?255:0,sizeof c->background.vram_blocks);
            else for(unsigned word=0;word<sizeof c->background.vram_blocks/sizeof(uint32_t);++word)
                c->background.vram_blocks[word]=c->background.layer_vram_blocks[0][word]|
                    c->background.layer_vram_blocks[1][word]|c->background.layer_vram_blocks[2][word];
        }
        if (++c->epoch == 0) {
            snesrecomp_colour_tiles_reset(c); c->epoch = 1;reuse_native=false;retained_layers=0;
            plane_keep=false;shift_try=0;
            memset(c->background.vram_blocks,mode7?255:0,sizeof c->background.vram_blocks);
        }
        /* Pin retained native slots BEFORE any image tile can allocate or
         * evict in this epoch. No GPU slot ever keeps an uncertified index. */
        if(reuse_native)for(unsigned i=0;i<c->native_quad_count;++i)
            c->entries[c->quads[i].slot].epoch=c->epoch;
        if(retained_layers)for(unsigned i=0;i<c->native_quad_count;++i) {
            const SnesRecompTileQuad quad=c->quads[i];
            if(!(retained_planes&(1u<<(RawQuadLayer(&quad)+
                (quad.pass>=SNESRECOMP_RAW_TILE_MAIN_PASSES?3u:0u)))))continue;
            c->entries[quad.slot].epoch=c->epoch;
            c->quads[out->quad_count++]=quad;
            ++out->cache_hits;
        }
        if(plane_keep) {
            for(unsigned i=0;i<c->plane->tile_count;++i) c->entries[c->plane->tiles[i].slot].epoch=c->epoch;
            c->plane->epoch=c->epoch;
        }
    }
    out->width = cap->canvas_width; out->height = cap->visible_height;
    for (unsigned bi = 0; bi < cap->band_count; ++bi) {
        const SnesPpuRasterBand *band = &cap->bands[bi];
        SnesRecompWindowSpan *spans = c->windows[bi];
        unsigned ns;
        if (reuse_windows) ns = c->window_counts[bi];
        else if (bi && WindowsEqual(band, &cap->bands[bi - 1]) &&
            snesrecomp_ppu_resolved_windows_equal(cap,bi,bi-1u)) {
            ns = c->window_counts[bi - 1];
            memcpy(spans, c->windows[bi - 1], ns * sizeof(*spans));
        } else ns = snesrecomp_ppu_window_spans(cap, band, spans, SNESRECOMP_WINDOW_MAX_SPANS);
        c->window_counts[bi] = ns;
        if (!ns) goto invalid;
        if (!reuse_objects) DrawObjects(c, cap, band, spans, ns, coverage,
            object_rows ? changed_object_rows : NULL);
        if (reuse || raw) continue;
        /* Palette changes split the row blocks without re-evaluating windows. */
        unsigned y = band->y_begin;
        while (y < band->y_end) {
            const uint16_t *pal = cap->cgram_lines ? cap->cgram_lines + y * 256 : cap->cgram;
            unsigned end = band->y_end;
            if (cap->cgram_lines) {
                end = y + 1;
                while (end < band->y_end && pal[0] == cap->cgram_lines[end * 256]) ++end;
            }
            for (unsigned si = 0; si < ns; ++si) {
                if (!Backdrop(c, out, spans[si].begin, y, spans[si].end, end,
                    band->forced_blank ? 0xff000000u : Colour(pal[0], band, &spans[si], 5))) goto capacity;
            }
            y = end;
        }
    }
    if (!reuse_objects && subscreen && !Mode7Subscreen(c,cap,coverage)) goto capacity;
    TILE_COST_MARK(1);
    if (reuse) {
        if(plane_layer!=PLANE_NONE) PlaneRecolour(c,cap,plane_palette,out);
        out->quad_count = c->background.quad_count;
        out->backdrop_count = c->background.backdrop_count;
        out->cache_hits = out->quad_count;
        out->geometry_reused = 1;
        goto ready;
    }
    if(plane_layer!=PLANE_NONE) {
#if defined(SNESRECOMP_PPU_TILE_COSTS)
        const uint64_t plane_begin=profile_costs?snesrecomp_now_us():0;
#endif
        bool built=true;
        if(plane_keep) {PlaneRecolour(c,cap,plane_palette,out);PlaneKeepMarks(c);}
        else built=PlaneBuildTiles(c,cap,plane_first->bg+plane_layer,plane_layer,plane_rows,plane_palette,out);
        if(built && !reuse_native) built=PlaneBuildQuads(c,cap,plane_layer);
        if(!built) {
            /* Native quads take over; the layer was never retained. */
            if(reuse_native) goto capacity;
            c->plane->active=false;plane_layer=PLANE_NONE;
        } else c->plane->active=true;
#if defined(SNESRECOMP_PPU_TILE_COSTS)
        if(profile_costs && plane_layer!=PLANE_NONE) {
            profile_layers[plane_layer]+=snesrecomp_now_us()-plane_begin;
            profile_quads[plane_layer]+=c->plane->quad_count;
        }
#endif
    } else if(c->plane) c->plane->active=false;
    unsigned shift_ready=0,shift_nothing=0;
    ShiftFrame shift_frames[3];
    for(unsigned layer=0;layer<3;++layer) {
        const SnesPpuRasterBand *first=cap->bands;
        if(!(shift_try&(1u<<layer)) || c->window_counts[0]!=1 || first->forced_blank ||
            !(first->main_enable&(1u<<layer)) || !ShiftEligible(cap,&first->bg[layer],layer)) continue;
        bool single=true;
        for(unsigned bi=1;single && bi<cap->band_count;++bi)
            single=LayerEquals(first,cap->bands+bi,layer,false) && snesrecomp_ppu_resolved_windows_equal(cap,0,bi);
        if(!single || !ShiftContextMatches(&c->shift[layer],cap,first,layer,c->windows[0])) continue;
        const int state=ShiftLayerPrepare(c,cap,first,layer,c->windows[0],shift_changed,
            shift_palettes[layer],shift_frames+layer);
        if(state==SHIFT_READY) shift_ready|=1u<<layer;
        else if(state==SHIFT_NOTHING) shift_nothing|=1u<<layer;
    }
    /* Only quads that stay valid keep their atlas slots for this epoch. */
    for(unsigned layer=0;layer<3;++layer) if(shift_ready&(1u<<layer))
        for(uint32_t i=0;i<c->shift[layer].count;++i) {
            const SnesRecompTileQuad *q=c->shift_quads[layer]+i;
            if(q->x0!=q->x1) c->entries[q->slot].epoch=c->epoch;
        }
    /* Raster changes on another layer do not split this layer's geometry. */
    unsigned shift_record=0;
    SnesPpuRasterBand shift_bands[3];
    SnesRecompWindowSpan shift_spans[3];
    for (unsigned screen = 0; screen < (raw?2u:1u); ++screen)
    for (unsigned layer = 0; !mode7 && !reuse_native && layer < 3; ++layer) {
        const uint32_t layer_begin=out->quad_count;
        if(layer!=plane_layer && !(negative_layers&(1u<<layer)) && !(retained_planes&(1u<<(screen*3u+layer))))
        for (unsigned bi = 0; bi < cap->band_count;) {
            const unsigned first = bi;
            SnesPpuRasterBand merged = cap->bands[bi++];
            while (bi < cap->band_count && LayerEquals(&merged, &cap->bands[bi], layer, raw) &&
                snesrecomp_ppu_resolved_windows_equal(cap,first,bi))
                merged.y_end = cap->bands[bi++].y_end;
            if (merged.forced_blank ||
                !((screen?merged.sub_enable:merged.main_enable) & (1u << layer)) ||
                (screen && !(merged.cgwsel&2u))) continue;
            const SnesRecompWindowSpan *spans = c->windows[first];
            const unsigned ns = c->window_counts[first];
            if (!ns) goto invalid;
            if(shift_frame && first==0 && bi==cap->band_count && merged.y_begin==0 &&
                merged.y_end==cap->visible_height && ns==1 && ShiftEligible(cap,&merged.bg[layer],layer)) {
                shift_record|=1u<<layer;shift_bands[layer]=merged;shift_spans[layer]=spans[0];
                if(shift_nothing&(1u<<layer)) continue;
                if(shift_ready&(1u<<layer)) {
#if defined(SNESRECOMP_PPU_TILE_COSTS)
                    const uint64_t layer_begin_us=profile_costs?snesrecomp_now_us():0;
#endif
                    if(!ShiftLayerEmit(c,cap,&merged,layer,spans,shift_frames+layer,out)) goto capacity;
#if defined(SNESRECOMP_PPU_TILE_COSTS)
                    if(profile_costs) profile_layers[layer]+=snesrecomp_now_us()-layer_begin_us;
#endif
                    continue;
                }
            }
            const unsigned colours = merged.bg[layer].bpp == 2 ? 32u : 128u;
            for (unsigned y = merged.y_begin; y < merged.y_end;) {
                const uint16_t *pal = cap->cgram_lines ? cap->cgram_lines + y * 256 : cap->cgram;
                unsigned end = merged.y_end;
                if (cap->cgram_lines && !indexed) {
                    end = y + 1;
                    while (end < merged.y_end && snesrecomp_ppu_words_equal(pal,
                        cap->cgram_lines+end*256,colours)) ++end;
                }
#if defined(SNESRECOMP_PPU_TILE_COSTS)
                const uint64_t layer_begin=profile_costs?snesrecomp_now_us():0;
                const uint32_t quad_begin=out->quad_count;
#endif
                if (!DrawLayer(c, cap, &merged, layer, y, end, pal, spans, ns, out, raw, screen!=0,
                    INT32_MIN, INT32_MAX, true)) goto capacity;
#if defined(SNESRECOMP_PPU_TILE_COSTS)
                if(profile_costs) {
                    profile_layers[screen*3u+layer]+=snesrecomp_now_us()-layer_begin;
                    profile_quads[screen*3u+layer]+=out->quad_count-quad_begin;
                }
#endif
                y = end;
            }
        }
        ShiftLayer *const s=&c->shift[layer];
        const uint32_t count=out->quad_count-layer_begin;
        s->valid=((shift_record>>layer)&1u) && count<=SHIFT_QUADS;
        if(s->valid) {
            s->band=shift_bands[layer];s->span=shift_spans[layer];s->count=count;
            s->resolved=cap->resolved_bg[layer]!=NULL;
            memcpy(c->shift_quads[layer],c->quads+layer_begin,count*sizeof *c->quads);
            memcpy(c->shift_palettes[layer],c->quad_palettes+layer_begin,count);
        }
    }
    c->shift_epoch=c->epoch;
    if(reuse_native) {
        out->quad_count=c->native_quad_count;
        out->cache_hits+=c->native_quad_count;
    } else c->native_quad_count=out->quad_count;
    if(raw && !indexed && !mode7 && source_count && !reuse_native) {
        unsigned nonempty=0;
        for(unsigned i=0;i<c->native_quad_count;++i)nonempty|=1u<<RawQuadLayer(c->quads+i);
        if(plane_layer!=PLANE_NONE) nonempty|=1u<<plane_layer;
        NegativeRemember(c,cap,job,7u&~nonempty,negative_layers);
    }
    TILE_COST_MARK(2);
    /* Planes imply a backend that samples job art directly. */
    c->image_active=c->plane && source_count && DrawSourceImages(c,cap);
    if(!c->image_active && !DrawSources(c,cap,out)) goto capacity;
    TILE_COST_MARK(3);
    BackgroundRemember(&c->background, cap, out, versions, !indexed);
    c->source_count=(uint16_t)source_count;
    if(source_count) {
        c->source_owner=job->owner;c->source_geometry=job->geometry_version;
        for(unsigned i=0;i<source_count;++i) {
            const SnesRecompPpuSource *r=job->sources+i;
            c->source_clips[i]=(struct SourceClipRect){r->x,r->y,r->width,r->height,r->layer,r->screens};
        }
        memcpy(c->source_versions,job->image_versions,job->image_count*sizeof(uint64_t));
    }
    c->background.uniform_palette=uniform_palette;
    ++c->geometry_generation;
    TILE_COST_MARK(4);
ready:
    if(c->plane && c->plane->active) {
        const PlaneState *plane=c->plane;
        out->plane_tiles=plane->tiles;out->plane_quads=plane->quads;
        out->plane_tile_count=plane->tile_count;out->plane_quad_count=plane->quad_count;
        out->plane_width=plane->width;out->plane_height=plane->height;
        out->plane_tile_generation=plane->tile_generation;
    }
    if(c->image_active && source_count) {
        out->image_quads=c->image_quads;out->image_quad_count=c->image_quad_count;
        out->images=job->images;out->image_versions=job->image_versions;
        out->image_owner=job->owner;out->image_count=job->image_count;
    }
#if defined(SNESRECOMP_PPU_TILE_COSTS)
    if(profile_costs)fprintf(stderr,"PPU tiles seq=%llu bands=%u sources=%u reuse=%u native=%u layers=%u quads=%u decoded=%u cert_us=%llu setup_us=%llu native_us=%llu sources_us=%llu snapshot_us=%llu\n",
        (unsigned long long)snesrecomp_ppu_frame_sequence(frame),cap->band_count,source_count,
        reuse,reuse_native,retained_layers,out->quad_count,out->tiles_decoded,
        (unsigned long long)profile_times[0],(unsigned long long)profile_times[1],
        (unsigned long long)profile_times[2],(unsigned long long)profile_times[3],(unsigned long long)profile_times[4]);
    if(profile_costs)fprintf(stderr,"PPU layers us=%llu,%llu,%llu,%llu,%llu,%llu quads=%u,%u,%u,%u,%u,%u\n",
        (unsigned long long)profile_layers[0],(unsigned long long)profile_layers[1],(unsigned long long)profile_layers[2],
        (unsigned long long)profile_layers[3],(unsigned long long)profile_layers[4],(unsigned long long)profile_layers[5],
        profile_quads[0],profile_quads[1],profile_quads[2],profile_quads[3],profile_quads[4],profile_quads[5]);
#endif
    /* The cached window rows now belong to this partition, even when the
     * identical native mesh was produced by differently split raster runs. */
    if(reuse && raw) {
        c->background.band_count=cap->band_count;
        memcpy(c->background.bands,cap->bands,cap->band_count*sizeof(*cap->bands));
        if(cap->resolved_windows)memcpy(c->background.windows,cap->resolved_windows,
            cap->band_count*6u*sizeof(*cap->resolved_windows));
    }
    if(source_count) memcpy(c->source_versions,job->image_versions,job->image_count*sizeof(uint64_t));
    if (!reuse_objects) {
        ObjectsRemember(c, cap, versions, object_rows ? changed_object_rows : NULL);
        ++c->object_generation;
    }
    out->geometry_generation = c->geometry_generation;
    c->background.versions=*versions;
    if(reuse_objects) c->object_versions=*versions;
    out->object_generation = c->object_generation;
    out->quads = c->quads; out->backdrops = c->backdrops;
    out->tiles = c->tiles; out->dirty = c->dirty;
    out->generations = c->generations;
    out->obj_pixels = !raw && cap->obj_pixels ? c->obj_pixels : NULL;
    out->obj_coverage = raw?NULL:coverage;
    out->obj_overlay_pixels = raw?0u:c->obj_overlay_pixels;
    c->source_job=NULL;return SNESRECOMP_PPU_OK;
invalid:
    for(unsigned layer=0;layer<3;++layer) c->shift[layer].valid=false;
    out->quad_count = out->backdrop_count = 0;
    if(c->plane) c->plane->active=false;
    c->image_active=false;
    c->source_job=NULL;return SNESRECOMP_PPU_INVALID;
capacity:
    for(unsigned layer=0;layer<3;++layer) c->shift[layer].valid=false;
    out->quad_count = out->backdrop_count = 0;
    if(c->plane) c->plane->active=false;
    c->image_active=false;
    c->background.valid=false;c->source_job=NULL;return SNESRECOMP_PPU_RESOURCE_LIMIT;
}

SnesRecompPpuResult snesrecomp_ppu_tile_plan(SnesRecompColourTileCache *cache,
    const SnesRecompPpuFrame *frame, SnesRecompTilePlan *out) {
    return ColourPlan(cache, frame, out, false, false,NULL,false);
}
SnesRecompPpuResult snesrecomp_ppu_mode7_colour_plan(SnesRecompColourTileCache *cache,
    const SnesRecompPpuFrame *frame, SnesRecompTilePlan *out) {
    return ColourPlan(cache, frame, out, true, false,NULL,false);
}

SnesRecompPpuResult snesrecomp_ppu_mode1_raw_tiles(SnesRecompColourTileCache *cache,
    const SnesRecompPpuFrame *frame,SnesRecompTilePlan *out) {
    return ColourPlan(cache,frame,out,false,true,NULL,false);
}
SnesRecompPpuResult snesrecomp_ppu_mode1_source_tiles(SnesRecompColourTileCache *cache,
    const SnesRecompPpuJob *job,SnesRecompTilePlan *out) {
    const SnesRecompPpuJobView *view=snesrecomp_ppu_job_view(job);
    if(!view) return SNESRECOMP_PPU_INVALID;
    return ColourPlan(cache,view->frame,out,false,true,view,false);
}

SnesRecompPpuResult snesrecomp_ppu_mode1_indexed_backgrounds(
    SnesRecompColourTileCache *c,const SnesRecompPpuFrame *frame,
    SnesRecompIndexedBackgroundPlan *out) {
    if(!out) return SNESRECOMP_PPU_INVALID;
    memset(out,0,sizeof(*out));
    SnesRecompTilePlan work={0};
    const SnesRecompPpuResult result=ColourPlan(c,frame,&work,false,true,NULL,true);
    out->unsupported=(uint32_t)work.unsupported;
    if(result!=SNESRECOMP_PPU_OK) return result;
    if(!work.geometry_reused) for(unsigned i=0;i<work.quad_count;++i) {
        const SnesRecompTileQuad *q=work.quads+i;
        const bool sub=q->pass>=SNESRECOMP_RAW_TILE_MAIN_PASSES;
        const unsigned rung=sub?q->pass-SNESRECOMP_RAW_TILE_MAIN_PASSES:q->pass/2u;
        const unsigned layer=rung==0 || rung==1 || rung==6?2:rung==2 || rung==4?1:0;
        const unsigned high=rung==1 || rung==4 || rung==5 || rung==6;
        const unsigned flags=((q->reserved&128u)?0u:SNESRECOMP_INDEXED_RGB_VISIBLE)|
            (!sub && (q->pass&1u)?SNESRECOMP_INDEXED_COLOUR_MATH:0u);
        c->indexed_quads[i]=(SnesRecompIndexedTileQuad){q->x0,q->y0,q->x1,q->y1,
            q->slot,q->u0,q->v0,q->u1,q->v1,(uint8_t)(q->reserved&127u),
            (uint8_t)layer,(uint8_t)(sub?2:1),(uint8_t)high,(uint8_t)flags,0};
    }
    *out=(SnesRecompIndexedBackgroundPlan){c->indexed_quads,work.tiles,work.generations,
        work.dirty,work.geometry_generation,work.quad_count,work.tiles_decoded,
        work.cache_hits,work.geometry_reused,0,work.width,work.height,c->indexed_classes};
    return SNESRECOMP_PPU_OK;
}

SnesRecompPpuResult snesrecomp_ppu_mode1_indexed_scene(
    SnesRecompColourTileCache *c,const SnesRecompPpuJob *job,
    SnesRecompIndexedScenePlan *out) {
    if(!out) return SNESRECOMP_PPU_INVALID;
    memset(out,0,sizeof *out);
    const SnesRecompPpuJobView *v=snesrecomp_ppu_job_view(job);
    if(!c || !v || !v->indexed_layout) return SNESRECOMP_PPU_INVALID;
    const SnesRecompPpuIndexedLayout *l=v->indexed_layout;
    const SnesPpuFrameCapture *cap=snesrecomp_ppu_frame_capture(v->graphics_frame);
    if(!cap) return SNESRECOMP_PPU_INVALID;
    /* A retained BG mesh may ignore writes to OBJ character memory. Start a
     * fresh epoch even on its warm path, so OBJ Tile cannot take the same-epoch
     * hit without validating those independent source bytes. */
    if(++c->epoch==0) {snesrecomp_colour_tiles_reset(c);c->epoch=1;}
    SnesRecompIndexedScenePlan plan={0};
    const SnesRecompPpuResult result=snesrecomp_ppu_mode1_indexed_backgrounds(c,v->graphics_frame,&plan.background);
    if(result!=SNESRECOMP_PPU_OK) {out->background.unsupported=plan.background.unsupported;return result;}
    for(unsigned i=0;i<plan.background.quad_count;++i)
        c->entries[plan.background.quads[i].slot].epoch=c->epoch;
    uint32_t palette[16]={0},hash=2166136261u;
    for(unsigned i=1;i<16;++i) {palette[i]=(i<<24)|(i<<16);hash=(hash^palette[i])*16777619u;}
    SnesRecompTilePlan work={0};
    for(unsigned i=0;i<l->obj_run_count;++i) {
        const SnesRecompObjSliver *s=&l->obj_runs[i].sliver;
        const int slot=Tile(c,cap,palette,hash,(unsigned)s->tile*16u,4,4096u,true,&work);
        if(slot<0) {c->background.valid=false;return SNESRECOMP_PPU_RESOURCE_LIMIT;}
        c->indexed_slots[i]=(uint16_t)slot;
    }
    plan.obj_slots=c->indexed_slots;plan.obj_count=l->obj_run_count;
    plan.obj_tiles_decoded=work.tiles_decoded;plan.obj_cache_hits=work.cache_hits;
    *out=plan;return SNESRECOMP_PPU_OK;
}

SnesRecompPpuResult snesrecomp_ppu_mode1_indexed_frame(
    SnesRecompColourTileCache *base_cache,SnesRecompColourTileCache *source_cache,
    const SnesRecompPpuJob *job,SnesRecompIndexedFramePlan *out) {
    if(!out) return SNESRECOMP_PPU_INVALID;
    memset(out,0,sizeof *out);
    const SnesRecompPpuJobView *v=snesrecomp_ppu_job_view(job);
    if(!base_cache || !source_cache || base_cache==source_cache || !v || !v->indexed_layout)
        return SNESRECOMP_PPU_INVALID;
    SnesRecompIndexedFramePlan plan={0};
    SnesRecompPpuResult result=snesrecomp_ppu_mode1_indexed_backgrounds(base_cache,v->frame,&plan.base);
    if(result!=SNESRECOMP_PPU_OK) return result;
    result=snesrecomp_ppu_mode1_indexed_scene(source_cache,job,&plan.source);
    if(result!=SNESRECOMP_PPU_OK) return result;
    *out=plan;return SNESRECOMP_PPU_OK;
}

SnesRecompPpuResult snesrecomp_ppu_mode1_obj_scene(
    SnesRecompColourTileCache *c,const SnesRecompPpuFrame *frame,
    const SnesRecompPpuJob *sources,const SnesRecompPpuObjRun *runs,
    uint32_t count,SnesRecompObjTileScenePlan *out) {
    if(!out) return SNESRECOMP_PPU_INVALID;
    memset(out,0,sizeof *out);
    const SnesPpuFrameCapture *cap=snesrecomp_ppu_frame_capture(frame);
    const SnesRecompPpuJobView *view=sources?snesrecomp_ppu_job_view(sources):NULL;
    if(!c || !cap || (!runs && count) || (sources && (!view || view->frame!=frame)))
        return SNESRECOMP_PPU_INVALID;
    if(count>SNESRECOMP_OBJ_MAX_SLIVERS) return SNESRECOMP_PPU_RESOURCE_LIMIT;
    /* Geometry owns selection/order/visibility. Validate every field consumed
     * by this atlas stage before any cache or output mutation. */
    for(unsigned i=0;i<count;++i) {
        const SnesRecompObjSliver *s=&runs[i].sliver;
        if(s->line>=cap->visible_height || s->tile>=2048u || s->row>=8u ||
            s->palette_base<128u || (s->palette_base&15u) ||
            s->math_eligible!=(s->palette_base>=192u)) return SNESRECOMP_PPU_INVALID;
    }
    /* A warm BG mesh can ignore OBJ memory writes. Advance independently and
     * pin every retained BG slot before adding any OBJ palette/tile variant. */
    if(++c->epoch==0) {snesrecomp_colour_tiles_reset(c);c->epoch=1;}
    SnesRecompObjTileScenePlan plan={0};
    const SnesRecompPpuResult result=ColourPlan(c,frame,&plan.background,false,true,view,false);
    if(result!=SNESRECOMP_PPU_OK) {out->background.unsupported=plan.background.unsupported;return result;}
    for(unsigned i=0;i<plan.background.quad_count;++i)
        c->entries[plan.background.quads[i].slot].epoch=c->epoch;
    uint32_t palette[8][16],hashes[8],rows[8];
    uint16_t colours[8][16];
    bool valid[8]={false};
    SnesRecompTilePlan work={0};
    for(unsigned i=0;i<count;++i) {
        const SnesRecompObjSliver *s=&runs[i].sliver;
        const unsigned group=(s->palette_base-128u)/16u;
        const unsigned row=cap->cgram_lines?s->line:0u;
        if(!valid[group] || rows[group]!=row) {
            const uint16_t *source=(cap->cgram_lines?cap->cgram_lines+row*256u:cap->cgram)+s->palette_base;
            if(!valid[group] || !snesrecomp_ppu_words_equal(colours[group],source,16u)) {
                memcpy(colours[group],source,16u*sizeof *source);
                uint32_t hash=2166136261u;
                palette[group][0]=0;
                for(unsigned index=1;index<16u;++index) {
                    const unsigned value=source[index];
                    palette[group][index]=UINT32_C(0xff000000)|((value&31u)<<19)|
                        (((value>>5)&31u)<<11)|(((value>>10)&31u)<<3);
                    hash=(hash^palette[group][index])*16777619u;
                }
                hashes[group]=hash;
            }
            rows[group]=row;valid[group]=true;
        }
        /* Bit30 separates OBJ-colour families from BG and indexed variants;
         * bit31 remains clear, reserved for owned image sources. */
        const int slot=Tile(c,cap,palette[group],hashes[group],s->tile*16u,4u,
            4112u+group,true,&work);
        if(slot<0) {c->background.valid=false;return SNESRECOMP_PPU_RESOURCE_LIMIT;}
        c->indexed_slots[i]=(uint16_t)slot;
    }
    plan.obj_slots=c->indexed_slots;plan.obj_count=count;
    plan.obj_tiles_decoded=work.tiles_decoded;plan.obj_cache_hits=work.cache_hits;
    *out=plan;return SNESRECOMP_PPU_OK;
}
