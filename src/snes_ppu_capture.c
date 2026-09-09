#include "snesrecomp_platform/snes_ppu_capture.h"

#include <stddef.h>

/* Stable byte offsets in the 0x40-byte PPU snapshot. The game-side capture
 * has static assertions against the emulator Ppu layout; keeping the portable
 * decoder numeric avoids making this library depend on runner internals. */
enum {
    RAW_INIDISP = 0, RAW_OBSEL = 1, RAW_OAMADDL = 2, RAW_OAMADDH = 3,
    RAW_BGMODE = 4, RAW_MOSAIC = 5, RAW_BGSC = 6,
    RAW_BG12NBA = 10, RAW_SETINI = 13, RAW_HSCROLL = 14, RAW_VSCROLL = 22,
    RAW_FIXED = 46, RAW_WINDOW_SEL = 48, RAW_WINDOW_UNUSED = 51,
    RAW_WINDOW_POS = 52, RAW_SCREEN_ENABLE = 58, RAW_SCREEN_WINDOW = 60,
    RAW_CGADSUB = 62, RAW_CGWSEL = 63,
};

/* SETINI bit 1 interlaces sprites: a sprite covers half as many lines and its
 * source row depends on which field is being drawn. The frame parity that
 * decides it is emulator state rather than captured state, so an interlaced
 * OBJ frame is refused rather than drawn from a guessed field. */
enum { SETINI_OBJ_INTERLACE = 0x02u };

/* Every host OBJ policy byte must be zero for the exact subset. See the
 * SnesPpuLayoutState comment: these arm behaviour whose full state a trace
 * does not carry. */
static bool ObjHostPolicyInert(const SnesPpuLayoutState *l) {
    return !l->hud_split_height && !l->hud_left_end && !l->hud_right_start &&
           !l->hud_oam_first_slot && !l->hud_oam_slots &&
           !l->hud_oam_height && !l->hud_oam_first_slot2 &&
           !l->hud_oam_slots2 && !l->oam_left_hint_strict &&
           !l->oam_right_hint_strict;
}

static uint16_t RawU16(const uint8_t *p, unsigned offset) {
    return (uint16_t)(p[offset] | ((uint16_t)p[offset + 1u] << 8));
}

static uint32_t RawU32(const uint8_t *p, unsigned offset) {
    return (uint32_t)RawU16(p, offset) |
           ((uint32_t)RawU16(p, offset + 2u) << 16);
}

static bool BandRawConsistent(const SnesPpuRasterBand *b) {
    const uint8_t *r = b->regs;
    const uint16_t nba = RawU16(r, RAW_BG12NBA);

    if ((r[RAW_INIDISP] & 0x0fu) != b->brightness ||
        ((r[RAW_INIDISP] & 0x80u) != 0) != b->forced_blank ||
        (r[RAW_BGMODE] & 7u) != b->bg_mode ||
        ((r[RAW_BGMODE] & 8u) != 0) != b->bg3_priority ||
        r[RAW_MOSAIC] != b->mosaic ||
        r[RAW_SCREEN_ENABLE] != b->main_enable ||
        r[RAW_SCREEN_ENABLE + 1u] != b->sub_enable ||
        r[RAW_SCREEN_WINDOW] != b->main_window_enable ||
        r[RAW_SCREEN_WINDOW + 1u] != b->sub_window_enable ||
        r[RAW_CGADSUB] != b->cgadsub || r[RAW_CGWSEL] != b->cgwsel ||
        RawU16(r, RAW_FIXED) != b->fixed_colour ||
        RawU32(r, RAW_WINDOW_SEL) != b->window_sel ||
        r[RAW_WINDOW_POS] != b->window1_left ||
        r[RAW_WINDOW_POS + 1u] != b->window1_right ||
        r[RAW_WINDOW_POS + 2u] != b->window2_left ||
        r[RAW_WINDOW_POS + 3u] != b->window2_right)
        return false;

    for (unsigned bg = 0; bg < SNES_PPU_BG_COUNT; bg++) {
        const uint8_t sc = r[RAW_BGSC + bg];
        const SnesPpuBgState *s = &b->bg[bg];
        if (s->tilemap_word_addr != (uint16_t)((sc & 0xfcu) << 8) ||
            s->wide != ((sc & 1u) != 0) ||
            s->tall != ((sc & 2u) != 0) ||
            s->char_word_addr !=
                (uint16_t)(((nba >> (bg * 4u)) & 0x0fu) << 12) ||
            s->big_tiles != ((r[RAW_BGMODE] & (0x10u << bg)) != 0) ||
            s->h_scroll != RawU16(r, RAW_HSCROLL + bg * 2u) ||
            s->v_scroll != RawU16(r, RAW_VSCROLL + bg * 2u))
            return false;
    }
    return true;
}

static bool BgResourcesEqual(const SnesPpuBgState *a,
                             const SnesPpuBgState *b) {
    return a->tilemap_word_addr == b->tilemap_word_addr &&
           a->char_word_addr == b->char_word_addr &&
           a->wide == b->wide && a->tall == b->tall &&
           a->big_tiles == b->big_tiles && a->bpp == b->bpp &&
           a->margin_left == b->margin_left &&
           a->margin_right == b->margin_right &&
           a->margin_repeats == b->margin_repeats;
}

const char *snes_ppu_unsupported_text(SnesPpuUnsupported reason) {
    switch (reason) {
    case SNES_PPU_SUPPORTED:                    return "supported";
    case SNES_PPU_UNSUPPORTED_MODE:             return "mode";
    case SNES_PPU_UNSUPPORTED_BG3:              return "bg3";
    case SNES_PPU_UNSUPPORTED_BG4:              return "bg4";
    case SNES_PPU_UNSUPPORTED_OBJ:              return "obj";
    case SNES_PPU_UNSUPPORTED_SUBSCREEN:        return "subscreen";
    case SNES_PPU_UNSUPPORTED_COLOUR_MATH:      return "colour-math";
    case SNES_PPU_UNSUPPORTED_WINDOWS:          return "windows";
    case SNES_PPU_UNSUPPORTED_MOSAIC:           return "mosaic";
    case SNES_PPU_UNSUPPORTED_OFFSET_PER_TILE:  return "offset-per-tile";
    case SNES_PPU_UNSUPPORTED_BIG_TILES:        return "big-tiles";
    case SNES_PPU_UNSUPPORTED_MAP_SIZE:         return "map-size";
    case SNES_PPU_UNSUPPORTED_RASTER_STATE:     return "raster-state";
    case SNES_PPU_UNSUPPORTED_LAYOUT_POLICY:    return "layout-policy";
    case SNES_PPU_UNSUPPORTED_RASTER_MEMORY:    return "raster-memory";
    case SNES_PPU_UNSUPPORTED_BACKEND:          return "backend-unavailable";
    }
    return "unknown";
}

SnesPpuUnsupported snesrecomp_ppu_phase1_supports(
    const SnesPpuFrameCapture *cap) {
    const SnesPpuBgState *resource_bg[3] = { NULL, NULL, NULL };
    unsigned next_y = 0;

    if (!cap || !cap->bands || !cap->vram || !cap->cgram ||
        !cap->band_count || cap->band_count > SNES_PPU_MAX_BANDS ||
        !cap->visible_height || cap->visible_height > SNES_PPU_MAX_BANDS)
        return SNES_PPU_UNSUPPORTED_RASTER_STATE;
    if (cap->raster_memory_flags)
        return SNES_PPU_UNSUPPORTED_RASTER_MEMORY;
    /* The HUD split and OAM hint policies also reshape composition and window
     * extents for frames with no sprites at all, so they are checked once for
     * the whole capture rather than only where OBJ is enabled. */
    if (!ObjHostPolicyInert(&cap->layout))
        return SNES_PPU_UNSUPPORTED_LAYOUT_POLICY;

    for (unsigned i = 0; i < cap->band_count; i++) {
        const SnesPpuRasterBand *b = &cap->bands[i];
        const unsigned on = (unsigned)(b->main_enable | b->sub_enable);
        const uint8_t setini = b->regs[RAW_SETINI];

        if (b->y_begin != next_y || b->y_begin >= b->y_end ||
            b->y_end > cap->visible_height || !BandRawConsistent(b))
            return SNES_PPU_UNSUPPORTED_RASTER_STATE;
        next_y = b->y_end;

        if ((setini & 0x0du) || (setini & (uint8_t)~0x4fu) ||
            b->regs[RAW_WINDOW_UNUSED] != 0)
            return SNES_PPU_UNSUPPORTED_RASTER_STATE;

        if (b->forced_blank)
            continue;
        if (b->brightness > 15u)
            return SNES_PPU_UNSUPPORTED_RASTER_STATE;
        if (b->bg_mode != 1)
            return SNES_PPU_UNSUPPORTED_MODE;
        if (on & 0x10u) {
            /* Sprites are drawn from OAM, and only from OAM the capture
             * itself carries. One reason covers every OBJ rejection so the
             * frozen expected-reject corpus keeps naming it. */
            if (!cap->oam || !cap->high_oam)
                return SNES_PPU_UNSUPPORTED_OBJ;
            if (setini & SETINI_OBJ_INTERLACE)
                return SNES_PPU_UNSUPPORTED_OBJ;
        }
        /* TM/TS bit 3 is deliberately NOT a rejection here.
         *
         * Mode 1 has three backgrounds. Bit 3 names a BG4 that does not exist
         * in this mode, and the CPU renderer's Mode-1 path never reads it: not
         * in the priority ladder, not in the window evaluation, and not in the
         * compositor, whose layer ids come from pixels that were actually
         * drawn. The one place the bit is visible at all is the "is any
         * subscreen layer enabled" gate, and with no drawable layer behind it
         * the subscreen stays at the backdrop either way -- which the
         * compositor resolves to the empty-sub case identically.
         *
         * Some games leave it set through most gameplay. Refusing those frames
         * would hand them back to the software renderer for a layer nobody
         * draws. Bit 3 stays closed wherever it means something, because every
         * mode that has a BG4 is already refused above.
         *
         * Direct colour is an 8bpp feature. TASK-04 deliberately does not
         * infer broader mode support merely because Mode 1 ignores it. */
        if (b->cgwsel & 1u)
            return SNES_PPU_UNSUPPORTED_COLOUR_MATH;
        if (b->mosaic & on & 0x07u)
            return SNES_PPU_UNSUPPORTED_MOSAIC;

        for (unsigned bg = 0; bg < 3; bg++) {
            if (!(on & (1u << bg)))
                continue;
            if (b->bg[bg].big_tiles)
                return SNES_PPU_UNSUPPORTED_BIG_TILES;
            if (b->bg[bg].bpp != (bg == 2u ? 2u : 4u))
                return SNES_PPU_UNSUPPORTED_MAP_SIZE;
            if (!resource_bg[bg])
                resource_bg[bg] = &b->bg[bg];
            else if (!BgResourcesEqual(resource_bg[bg], &b->bg[bg]))
                return SNES_PPU_UNSUPPORTED_RASTER_STATE;
        }
    }

    if (next_y != cap->visible_height)
        return SNES_PPU_UNSUPPORTED_RASTER_STATE;

    if (cap->band_count > 1u) {
        for (unsigned bg = 0; bg < 3u; bg++) {
            const SnesPpuBgState *base = resource_bg[bg];
            if (!base)
                continue;
            if ((!base->margin_left && !base->margin_right) ||
                base->margin_repeats)
                continue;
            for (unsigned i = 0; i < cap->band_count; i++) {
                const SnesPpuRasterBand *b = &cap->bands[i];
                if (b->forced_blank ||
                    !((b->main_enable | b->sub_enable) & (1u << bg)))
                    continue;
                if (b->bg[bg].h_scroll != base->h_scroll ||
                    b->bg[bg].v_scroll != base->v_scroll)
                    return SNES_PPU_UNSUPPORTED_LAYOUT_POLICY;
            }
        }
    }

    return SNES_PPU_SUPPORTED;
}

SnesPpuUnsupported snesrecomp_ppu_phase1_backend_supports(
    const SnesPpuFrameCapture *cap, bool backend_available) {
    const SnesPpuUnsupported semantic =
        snesrecomp_ppu_phase1_supports(cap);
    if (semantic != SNES_PPU_SUPPORTED)
        return semantic;
    return backend_available ? SNES_PPU_SUPPORTED
                             : SNES_PPU_UNSUPPORTED_BACKEND;
}

bool snesrecomp_ppu_phase1_expand_lines(
    const SnesPpuFrameCapture *cap,
    uint16_t *bg1_scroll_x, uint16_t *bg1_scroll_y,
    uint16_t *bg2_scroll_x, uint16_t *bg2_scroll_y,
    uint8_t *brightness, uint8_t *forced_blank, unsigned capacity) {
    if (!cap || !cap->bands || !bg1_scroll_x || !bg1_scroll_y ||
        !bg2_scroll_x || !bg2_scroll_y || !brightness || !forced_blank ||
        capacity < cap->visible_height)
        return false;
    for (unsigned i = 0; i < cap->band_count; i++) {
        const SnesPpuRasterBand *b = &cap->bands[i];
        if (b->y_begin >= b->y_end || b->y_end > cap->visible_height)
            return false;
        for (unsigned y = b->y_begin; y < b->y_end; y++) {
            bg1_scroll_x[y] = b->bg[0].h_scroll;
            bg1_scroll_y[y] = (uint16_t)(b->bg[0].v_scroll + y);
            bg2_scroll_x[y] = b->bg[1].h_scroll;
            bg2_scroll_y[y] = (uint16_t)(b->bg[1].v_scroll + y);
            brightness[y] = b->brightness;
            forced_blank[y] = b->forced_blank ? 1u : 0u;
        }
    }
    return true;
}
