#include "snesrecomp_platform/snes_bg_continuity.h"

#include <string.h>

int SnesBgScrollDelta(uint16_t from, uint16_t to) {
    int delta = ((int)to - (int)from) & (SNES_BG_SCROLL_MODULUS - 1);
    if (delta > SNES_BG_SCROLL_MODULUS / 2)
        delta -= SNES_BG_SCROLL_MODULUS;
    return delta;
}

uint16_t SnesBgScrollAt(uint16_t from, int delta, float alpha) {
    if (alpha <= 0.0f)
        return (uint16_t)(from & (SNES_BG_SCROLL_MODULUS - 1));
    if (alpha >= 1.0f)
        alpha = 1.0f;

    const float step = (float)delta * alpha;
    const int rounded = (int)(step >= 0.0f ? step + 0.5f : step - 0.5f);
    return (uint16_t)(((int)from + rounded) & (SNES_BG_SCROLL_MODULUS - 1));
}

float SnesBgTileBoundAlpha(uint16_t scroll, int delta) {
    if (!delta)
        return 1.0f;

    const int phase = (int)(scroll & 7u);
    const int room = delta > 0 ? 7 - phase : phase;
    const int reach = delta > 0 ? delta : -delta;
    const float bound = (float)room / (float)reach;
    return bound < 1.0f ? bound : 1.0f;
}

static uint64_t Mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    return hash * 1099511628211ull;
}

static uint64_t MixBand(uint64_t hash, const SnesPpuRasterBand *band) {
    hash = Mix(hash, band->y_begin);
    hash = Mix(hash, band->y_end);
    hash = Mix(hash, band->bg_mode);
    hash = Mix(hash, band->bg3_priority ? 1u : 0u);
    hash = Mix(hash, band->forced_blank ? 1u : 0u);
    hash = Mix(hash, band->main_enable);
    hash = Mix(hash, band->sub_enable);
    hash = Mix(hash, band->main_window_enable);
    hash = Mix(hash, band->sub_window_enable);
    hash = Mix(hash, band->mosaic);

    for (unsigned layer = 0; layer < SNES_PPU_BG_COUNT; layer++) {
        const SnesPpuBgState *bg = &band->bg[layer];
        hash = Mix(hash, bg->tilemap_word_addr);
        hash = Mix(hash, bg->char_word_addr);
        hash = Mix(hash, bg->bpp);
        hash = Mix(hash, (bg->wide ? 1u : 0u) | (bg->tall ? 2u : 0u) |
                             (bg->big_tiles ? 4u : 0u));
    }
    return hash;
}

uint64_t SnesBgContinuityKey(const SnesPpuFrameCapture *capture) {
    if (!capture)
        return 0;

    uint64_t hash = 14695981039346656037ull;
    hash = Mix(hash, capture->canvas_width);
    hash = Mix(hash, capture->canvas_extra);
    hash = Mix(hash, capture->visible_height);
    hash = Mix(hash, capture->band_count);

    for (unsigned i = 0; i < capture->band_count; i++)
        hash = MixBand(hash, &capture->bands[i]);
    return hash;
}

static bool SameSurface(const SnesPpuBgState *a, const SnesPpuBgState *b) {
    return a->tilemap_word_addr == b->tilemap_word_addr &&
           a->char_word_addr == b->char_word_addr &&
           a->bpp == b->bpp && a->wide == b->wide && a->tall == b->tall &&
           a->big_tiles == b->big_tiles;
}

static SnesBgDiscontinuity BandVerdict(const SnesPpuRasterBand *a,
                                       const SnesPpuRasterBand *b) {
    if (a->y_begin != b->y_begin || a->y_end != b->y_end)
        return SNES_BG_DISCONTINUITY_BANDS;
    if (a->forced_blank || b->forced_blank)
        return SNES_BG_DISCONTINUITY_BLANK;
    if (a->bg_mode != b->bg_mode || a->bg3_priority != b->bg3_priority)
        return SNES_BG_DISCONTINUITY_MODE;
    if (a->main_enable != b->main_enable || a->sub_enable != b->sub_enable ||
        a->main_window_enable != b->main_window_enable ||
        a->sub_window_enable != b->sub_window_enable)
        return SNES_BG_DISCONTINUITY_ENABLE;
    if (a->mosaic != b->mosaic)
        return SNES_BG_DISCONTINUITY_MOSAIC;
    return SNES_BG_CONTINUOUS;
}

void SnesBgCompare(const SnesPpuFrameCapture *prev,
                   const SnesPpuFrameCapture *cur,
                   unsigned max_step_px,
                   SnesBgInterpolation *out) {
    if (!out)
        return;
    memset(out, 0, sizeof *out);

    if (!prev || !cur || !prev->bands || !cur->bands) {
        out->verdict = SNES_BG_DISCONTINUITY_GEOMETRY;
        return;
    }
    if (prev->canvas_width != cur->canvas_width ||
        prev->canvas_extra != cur->canvas_extra ||
        prev->visible_height != cur->visible_height) {
        out->verdict = SNES_BG_DISCONTINUITY_GEOMETRY;
        return;
    }
    if (prev->band_count != cur->band_count || !cur->band_count ||
        cur->band_count > SNES_PPU_MAX_BANDS) {
        out->verdict = SNES_BG_DISCONTINUITY_BANDS;
        return;
    }

    uint8_t safe = 0;
    for (unsigned i = 0; i < cur->band_count; i++) {
        const SnesBgDiscontinuity verdict =
            BandVerdict(&prev->bands[i], &cur->bands[i]);
        if (verdict != SNES_BG_CONTINUOUS) {
            out->verdict = verdict;
            return;
        }
        safe |= (uint8_t)(cur->bands[i].main_enable & 0x0f);
    }

    out->band_count = cur->band_count;
    for (unsigned i = 0; i < cur->band_count; i++) {
        const SnesPpuRasterBand *a = &prev->bands[i];
        const SnesPpuRasterBand *b = &cur->bands[i];

        for (unsigned layer = 0; layer < SNES_PPU_BG_COUNT; layer++) {
            const int dx = SnesBgScrollDelta(a->bg[layer].h_scroll,
                                             b->bg[layer].h_scroll);
            const int dy = SnesBgScrollDelta(a->bg[layer].v_scroll,
                                             b->bg[layer].v_scroll);
            out->bands[i].dx[layer] = (int16_t)dx;
            out->bands[i].dy[layer] = (int16_t)dy;

            const unsigned reach = (unsigned)(dx < 0 ? -dx : dx) >
                                           (unsigned)(dy < 0 ? -dy : dy)
                                       ? (unsigned)(dx < 0 ? -dx : dx)
                                       : (unsigned)(dy < 0 ? -dy : dy);
            if (reach > max_step_px ||
                !SameSurface(&a->bg[layer], &b->bg[layer]))
                safe &= (uint8_t)~(1u << layer);
        }
    }
    out->safe_layers = safe;
}
