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
struct SnesRecompColourTileCache {
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
    uint8_t source_row_counts[2][3][SNES_PPU_MAX_BANDS];
    uint16_t source_rows[2][3][SNES_PPU_MAX_BANDS][SNESRECOMP_PPU_JOB_SOURCES_PER_ROW];
    const SnesRecompPpuJobView *source_job; /* Only during synchronous preparation. */
};

SnesRecompColourTileCache *snesrecomp_colour_tiles_create(void) {
    return calloc(1, sizeof(SnesRecompColourTileCache));
}
void snesrecomp_colour_tiles_destroy(SnesRecompColourTileCache *c) {
    if (c) for (unsigned i = 0; i < 12; ++i) free(c->tile_memos[i]);
    free(c);
}
void snesrecomp_colour_tiles_reset(SnesRecompColourTileCache *c) {
    if (c) {
        memset(c->entries, 0, sizeof(c->entries));
        memset(c->dirty, 0, sizeof(c->dirty));
        c->epoch = 0; c->background.valid = false;
        for (unsigned i = 0; i < 12; ++i)
            if (c->tile_memos[i]) c->tile_memos[i]->valid = false;
    }
}
void snesrecomp_colour_tiles_uploaded(SnesRecompColourTileCache *c) {
    if (c) memset(c->dirty, 0, sizeof(c->dirty));
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
static bool BackgroundMatches(const BackgroundSnapshot *old, const SnesPpuFrameCapture *cap,
    const SnesRecompPpuFrameVersions *versions, bool raw, bool uniform_palette) {
    if (!old->valid || old->width != cap->canvas_width || old->height != cap->visible_height ||
        old->extra != cap->canvas_extra || old->band_count != cap->band_count ||
        memcmp(&old->layout, &cap->layout, sizeof(old->layout))) return false;
    if(old->resolved_windows!=(cap->resolved_windows!=NULL) || (cap->resolved_windows &&
        memcmp(old->windows,cap->resolved_windows,cap->band_count*6u*sizeof(*cap->resolved_windows)))) return false;
    if(!raw && memcmp(old->bands,cap->bands,cap->band_count*sizeof(*cap->bands))) return false;
    for(unsigned bi=0;raw && bi<cap->band_count;++bi) {
        const SnesPpuRasterBand *previous=old->bands+bi;
        const SnesPpuRasterBand *next=cap->bands+bi;
        /* Match the actual mesh inputs, not a dump of unrelated SNES ports.
         * OBJ state, master brightness, fixed colour, addition/subtraction and
         * half colour belong to other prepared resources/materials. Their
         * changes must not rebuild background tilemaps and window geometry. */
        if(previous->y_begin!=next->y_begin || previous->y_end!=next->y_end ||
            !LayerEquals(previous,next,0,true) || !LayerEquals(previous,next,1,true) ||
            !LayerEquals(previous,next,2,true)) return false;
    }
    if (!(old->versions.owner==versions->owner && old->versions.vram==versions->vram))
        for (unsigned block=0; block<SNES_PPU_VRAM_WORDS/64u; ++block)
            if ((old->vram_blocks[block/32u] & (1u<<(block%32u))) &&
                !snesrecomp_ppu_words_equal(old->vram+block*64u,cap->vram+block*64u,64u)) return false;
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
        if (!!(old->map_mask & (1u << layer)) != !!cap->resolved_bg[layer]) return false;
        if (cap->resolved_bg[layer] && memcmp(old->maps[layer], cap->resolved_bg[layer],
                                             sizeof(old->maps[layer]))) return false;
    }
    return true;
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
    if(!old->valid || !(same_owner && old->versions.vram==versions->vram))
        for (unsigned block=0; block<SNES_PPU_VRAM_WORDS/64u; ++block)
            if (old->vram_blocks[block/32u] & (1u<<(block%32u)))
                memcpy(old->vram+block*64u,cap->vram+block*64u,64u*sizeof(uint16_t));
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

static void DrawObjects(SnesRecompColourTileCache *c, const SnesPpuFrameCapture *cap,
    const SnesPpuRasterBand *band, const SnesRecompWindowSpan *spans, unsigned ns,
    const uint64_t *coverage) {
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
            for (unsigned py = y; py < end; ++py)
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
            !memcmp(e->palette + 1, palette + 1, (colours - 1) * sizeof(uint32_t))) {
            if (e->epoch == c->epoch || !memcmp(e->source, cap->vram + word, words * sizeof(uint16_t))) {
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

static void VramUsed(BackgroundSnapshot *background, unsigned word) {
    const unsigned block=word/64u;
    background->vram_blocks[block/32u] |= 1u<<(block%32u);
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
static void RefreshRawPalette(SnesRecompColourTileCache *c,
    const SnesPpuFrameCapture *cap,SnesRecompTilePlan *plan) {
    const uint16_t *colours_source=cap->cgram_lines?cap->cgram_lines:cap->cgram;
    uint32_t visited[SNESRECOMP_COLOUR_TILE_SLOTS/32u]={0};
    for(unsigned qi=0;qi<c->background.quad_count;++qi) {
        const unsigned slot=c->quads[qi].slot;
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
SnesRecompPpuResult snesrecomp_ppu_mode1_layer_sample_windows(const SnesPpuFrameCapture *cap,
    const SnesPpuRasterBand *band,const SnesRecompWindowSpan *spans,unsigned count,
    uint8_t layer,bool sub,uint16_t x,uint16_t y,bool *opaque,uint8_t *priority) {
    if(!cap || !band || !opaque || !priority || !cap->vram || layer>=3 ||
        x>=cap->canvas_width || y>=cap->visible_height || y<band->y_begin || y>=band->y_end)
        return SNESRECOMP_PPU_INVALID;
    if(band->bg_mode!=1 || band->bg[layer].elastic_count || band->bg[layer].margin_repeats ||
        (cap->layout.layer_mirror&(1u<<layer))) return SNESRECOMP_PPU_UNSUPPORTED;
    *opaque=false;*priority=0;
    if(band->forced_blank || !((sub?band->sub_enable:band->main_enable)&(1u<<layer))) return SNESRECOMP_PPU_OK;
    const SnesPpuBgState *bg=band->bg+layer;
    const int screen=(int)x-cap->canvas_extra;
    if(screen<-(int)bg->margin_left || screen>=256+bg->margin_right) return SNESRECOMP_PPU_OK;
    if(!spans || !count || count>SNESRECOMP_WINDOW_MAX_SPANS) return SNESRECOMP_PPU_INVALID;
    const SnesRecompWindowSpan *span=SampleSpan(spans,count,x);
    if(!span) return SNESRECOMP_PPU_INVALID;
    if(!(span->mask&(1u<<(layer+(sub?3u:0u))))) return SNESRECOMP_PPU_OK;
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
    *opaque=index!=0;*priority=(uint8_t)((entry>>13)&1u);return SNESRECOMP_PPU_OK;
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
    const uint32_t selectors = (15u << (layer * 4)) | (15u << 20);
    const unsigned logic = 3u << (layer * 2);
    return !memcmp(&a->bg[layer], &b->bg[layer], sizeof(a->bg[layer])) &&
        a->forced_blank == b->forced_blank && (raw || a->brightness == b->brightness) &&
        (layer != 2 || a->bg3_priority == b->bg3_priority) &&
        !((a->main_enable ^ b->main_enable) & bit) &&
        !((a->main_window_enable ^ b->main_window_enable) & bit) &&
        (!raw || (!((a->sub_enable ^ b->sub_enable) & bit) &&
                  !((a->sub_window_enable ^ b->sub_window_enable) & bit))) &&
        a->cgwsel == b->cgwsel && !((a->cgadsub ^ b->cgadsub) & (bit | (raw?0u:192u))) &&
        (raw || a->fixed_colour == b->fixed_colour) && !((a->window_sel ^ b->window_sel) & selectors) &&
        a->window1_left == b->window1_left && a->window1_right == b->window1_right &&
        a->window2_left == b->window2_left && a->window2_right == b->window2_right &&
        !((a->regs[56] ^ b->regs[56]) & logic) && !((a->regs[57] ^ b->regs[57]) & 12u);
}

static bool WindowsEqual(const SnesPpuRasterBand *a, const SnesPpuRasterBand *b) {
    return snesrecomp_ppu_windows_equal(a,b);
}

static bool ObjectsMatch(const SnesRecompColourTileCache *c, const SnesPpuFrameCapture *cap,
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
    return (c->object_versions.owner==versions->owner && c->object_versions.objects==versions->objects) ||
        snesrecomp_ppu_words_equal(c->obj_source,cap->obj_pixels,
                   (size_t)cap->canvas_width*cap->visible_height);
}

static void ObjectsRemember(SnesRecompColourTileCache *c, const SnesPpuFrameCapture *cap,
    const SnesRecompPpuFrameVersions *versions) {
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
    if(!(same_owner && c->object_versions.objects==versions->objects))
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

static bool DrawLayer(SnesRecompColourTileCache *c, const SnesPpuFrameCapture *cap,
    const SnesPpuRasterBand *band, unsigned layer, unsigned y, unsigned end,
    const uint16_t *pal, const SnesRecompWindowSpan *spans, unsigned ns,
    SnesRecompTilePlan *out, bool raw, bool sub) {
    const SnesPpuBgState *bg = &band->bg[layer];
    const bool elastic = bg->elastic_count != 0;
    const bool repeat = !elastic && bg->margin_repeats;
    const bool mirror = !elastic && !repeat && (cap->layout.layer_mirror & (1u << layer));
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
        uint32_t palettes[8][16] = {{0}};
        uint32_t palette_hashes[8];
        unsigned prepared_palettes = 0;
        int left = (int)cap->canvas_extra - bg->margin_left;
        int right = (int)cap->canvas_extra + 256 + bg->margin_right;
        if(elastic) {left=(int)cap->canvas_extra-cap->layout.extra_left_cur;
            right=(int)cap->canvas_extra+256+cap->layout.extra_right_cur;}
        if (left < span->begin) left = span->begin;
        if (right > span->end) right = span->end;
        for (unsigned py = y; py < end;) {
            const unsigned tile_shift = bg->big_tiles ? 4u : 3u;
            const unsigned coordinate_mask = bg->big_tiles ? 1023u : 511u;
            unsigned sy = (bg->v_scroll + py + 1u) & coordinate_mask;
            unsigned row = sy & 7u, height = 8u - row;
            if (height > end - py) height = end - py;
            for (int px = left; px < right;) {
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
                    VramUsed(&c->background,(bg->tilemap_word_addr+screen*1024u+(ty&31u)*32u+(tx&31u))&32767u);
                }
                unsigned character = entry & 1023u;
                if (bg->big_tiles) {
                    /* Flip the entire 16x16 character before selecting a quadrant. */
                    const unsigned cell_x = ((sx >> 3) & 1u) ^ !!(entry & 0x4000u);
                    const unsigned cell_y = ((sy >> 3) & 1u) ^ !!(entry & 0x8000u);
                    character = (character + cell_x + (cell_y << 4)) & 1023u;
                }
                unsigned word = (bg->char_word_addr + character * bg->bpp * 4u) & 32767u;
                VramUsed(&c->background,word);
                VramUsed(&c->background,(word+bg->bpp*4u-1u)&32767u);
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
                if (!c->opaque[slot]) { px += (int)width; continue; }
                if (out->quad_count >= SNESRECOMP_TILE_PLAN_QUADS) return false;
                unsigned priority = (entry >> 13) & 1u;
                unsigned pass = layer == 2 ? (priority ? (band->bg3_priority ? 6u : 1u) : 0u) :
                                layer == 1 ? (priority ? 4u : 2u) : (priority ? 5u : 3u);
                if(raw) pass=sub?SNESRECOMP_RAW_TILE_MAIN_PASSES+pass:
                    pass*2u+!!((span->mask&SNESRECOMP_SEMANTIC_MATH) && (band->cgadsub&(1u<<layer)));
                SnesRecompTileQuad quad;
                SnesRecompTileQuad *q = &quad;
                const unsigned first_u = (entry & 0x4000u) ? 7u - col : col;
                const bool flip_x = !!(entry & 0x4000u) ^ reverse;
                const unsigned texture_width=repeated_pixel?1u:width;
                *q = (SnesRecompTileQuad){(int16_t)px, (int16_t)py, (int16_t)(px + width),
                    (int16_t)(py + height), (uint16_t)slot,
                    (uint8_t)(flip_x ? first_u + 1u : first_u),
                    (uint8_t)((entry & 0x8000) ? 8u - row : row),
                    (uint8_t)(flip_x ? first_u + 1u - texture_width : first_u + texture_width),
                    (uint8_t)((entry & 0x8000) ? 8u - row - height : row + height),
                    (uint8_t)pass, c->indexed_tiles?
                        (uint8_t)((palette_number<<bg->bpp)|(rgb?0u:128u)):0};
                if(c->source_job) {
                    if(!SourceClipNative(c,out,q,layer,sub)) return false;
                } else c->quads[out->quad_count++]=quad;
                px += (int)width;
            }
            py += height;
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

static SnesRecompPpuResult ColourPlan(SnesRecompColourTileCache *c,
    const SnesRecompPpuFrame *frame, SnesRecompTilePlan *out, bool mode7, bool raw,
    const SnesRecompPpuJobView *job, bool indexed) {
    if (!c || !out) return SNESRECOMP_PPU_INVALID;
    memset(out, 0, sizeof(*out));
    const SnesPpuFrameCapture *cap = snesrecomp_ppu_frame_capture(frame);
    const uint64_t *coverage=snesrecomp_ppu_frame_obj_coverage(frame);
    const SnesRecompPpuFrameVersions *versions=snesrecomp_ppu_frame_versions(frame);
    if (!cap) return SNESRECOMP_PPU_INVALID;
    out->unsupported = indexed?snesrecomp_ppu_mode1_indexed_supports(cap):
        raw?snesrecomp_ppu_mode1_raw_supports(cap):Supports(cap, mode7, false,false);
    if (out->unsupported) return SNESRECOMP_PPU_UNSUPPORTED;
    if(c->raw_tiles!=raw || c->indexed_tiles!=indexed) {
        c->background.valid=false;c->obj_source_present=false;
        c->obj_overlay_pixels=0;c->raw_tiles=raw;
        c->indexed_tiles=indexed;
        for(unsigned i=0;i<12;++i) if(c->tile_memos[i]) c->tile_memos[i]->valid=false;
    }
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
    const bool same_sources=c->source_count==source_count && (!source_count ||
        (c->source_owner==job->owner && c->source_geometry==job->geometry_version));
    bool reuse = same_sources && BackgroundMatches(&c->background, cap, versions, raw, uniform_palette);
    if(reuse && source_count && memcmp(c->source_versions,job->image_versions,job->image_count*sizeof(uint64_t)))
        reuse=SourceRefresh(c,job,out);
    c->source_job=source_count?job:NULL;
    if(!reuse && source_count) SourceRows(c,job);
    bool subscreen=false;
    for (unsigned bi=0;mode7 && bi<cap->band_count;++bi) {
        const SnesPpuRasterBand *b=&cap->bands[bi];
        subscreen |= !b->forced_blank && (b->cgwsel&2u) && (b->sub_enable&16u) && (b->cgadsub&0x31u);
    }
    const bool reuse_objects = raw || (ObjectsMatch(c, cap, versions) &&
        (!(subscreen || c->obj_overlay_pixels) || reuse));
    if (!reuse_objects) {
        c->obj_source_present=false;c->obj_overlay_pixels=0;
        if(cap->obj_pixels) memset(c->obj_pixels,0,
            (size_t)cap->canvas_width*cap->visible_height*sizeof(*c->obj_pixels));
    }
    if (!reuse) {
        c->background.valid = false;
        memset(c->background.vram_blocks,mode7?255:0,sizeof c->background.vram_blocks);
        if (++c->epoch == 0) { snesrecomp_colour_tiles_reset(c); c->epoch = 1; }
    }
    out->width = cap->canvas_width; out->height = cap->visible_height;
    for (unsigned bi = 0; bi < cap->band_count; ++bi) {
        const SnesPpuRasterBand *band = &cap->bands[bi];
        SnesRecompWindowSpan *spans = c->windows[bi];
        unsigned ns;
        if (reuse) ns = c->window_counts[bi];
        else if (bi && WindowsEqual(band, &cap->bands[bi - 1]) &&
            snesrecomp_ppu_resolved_windows_equal(cap,bi,bi-1u)) {
            ns = c->window_counts[bi - 1];
            memcpy(spans, c->windows[bi - 1], ns * sizeof(*spans));
        } else ns = snesrecomp_ppu_window_spans(cap, band, spans, SNESRECOMP_WINDOW_MAX_SPANS);
        c->window_counts[bi] = ns;
        if (!ns) goto invalid;
        if (!reuse_objects) DrawObjects(c, cap, band, spans, ns, coverage);
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
    if (reuse) {
        if(raw && !indexed && uniform_palette && c->background.uniform_palette &&
            !(c->background.versions.owner==versions->owner &&
              c->background.versions.palette==versions->palette)) {
            RefreshRawPalette(c,cap,out);
            memcpy(c->background.palettes[0],cap->cgram_lines?cap->cgram_lines:cap->cgram,
                sizeof c->background.palettes[0]);
            c->background.palette_lines=false; /* Equivalent uniform snapshot. */
        }
        out->quad_count = c->background.quad_count;
        out->backdrop_count = c->background.backdrop_count;
        out->cache_hits = out->quad_count;
        out->geometry_reused = 1;
        goto ready;
    }
    /* Raster changes on another layer do not split this layer's geometry. */
    for (unsigned screen = 0; screen < (raw?2u:1u); ++screen)
    for (unsigned layer = 0; !mode7 && layer < 3; ++layer)
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
            const unsigned colours = merged.bg[layer].bpp == 2 ? 32u : 128u;
            for (unsigned y = merged.y_begin; y < merged.y_end;) {
                const uint16_t *pal = cap->cgram_lines ? cap->cgram_lines + y * 256 : cap->cgram;
                unsigned end = merged.y_end;
                if (cap->cgram_lines && !indexed) {
                    end = y + 1;
                    while (end < merged.y_end && snesrecomp_ppu_words_equal(pal,
                        cap->cgram_lines+end*256,colours)) ++end;
                }
                if (!DrawLayer(c, cap, &merged, layer, y, end, pal, spans, ns, out, raw, screen!=0)) goto capacity;
                y = end;
            }
        }
    if(!DrawSources(c,cap,out)) goto capacity;
    BackgroundRemember(&c->background, cap, out, versions, !indexed);
    c->source_count=(uint16_t)source_count;
    if(source_count) {
        c->source_owner=job->owner;c->source_geometry=job->geometry_version;
        memcpy(c->source_versions,job->image_versions,job->image_count*sizeof(uint64_t));
    }
    c->background.uniform_palette=uniform_palette;
    ++c->geometry_generation;
ready:
    if(source_count) memcpy(c->source_versions,job->image_versions,job->image_count*sizeof(uint64_t));
    if (!reuse_objects) {
        ObjectsRemember(c, cap, versions);
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
    out->quad_count = out->backdrop_count = 0;
    c->source_job=NULL;return SNESRECOMP_PPU_INVALID;
capacity:
    out->quad_count = out->backdrop_count = 0;
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
