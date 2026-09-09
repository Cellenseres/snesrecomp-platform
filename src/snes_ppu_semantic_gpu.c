#include "snesrecomp_platform/snes_ppu_semantic_gpu.h"

#include <string.h>

enum { RAW_WBGLOG = 56, RAW_WOBJLOG = 57 };

_Static_assert(sizeof(SnesRecompSemanticLineState) == 8u,
               "semantic line state must remain two RGBA8 texels");

typedef struct SemanticWindow {
    int w1_left, w1_right;
    int w2_left, w2_right;
    uint8_t logic;
    bool w1_enabled, w2_enabled;
    bool w1_invert, w2_invert;
} SemanticWindow;

static SemanticWindow PrepareWindow(const SnesPpuFrameCapture *cap,
                                    const SnesPpuRasterBand *band,
                                    unsigned layer) {
    const unsigned selector = (band->window_sel >> (layer * 4u)) & 0x0fu;
    SemanticWindow window;
    int screen_left, screen_right;

    window.w1_left = band->window1_left;
    window.w1_right = band->window1_right;
    window.w2_left = band->window2_left;
    window.w2_right = band->window2_right;
    /* wbgobjlog is one 16-bit field: the low byte holds two bits per BG, the
     * high byte two for OBJ and two for the colour window. */
    window.logic = (uint8_t)(layer >= 4u
        ? ((unsigned)band->regs[RAW_WOBJLOG] >> ((layer - 4u) * 2u)) & 3u
        : ((unsigned)band->regs[RAW_WBGLOG] >> (layer * 2u)) & 3u);
    window.w1_invert = (selector & 1u) != 0;
    window.w2_invert = (selector & 4u) != 0;

    if (layer < 3u) {
        screen_left = -(int)band->bg[layer].margin_left;
        screen_right = 256 + (int)band->bg[layer].margin_right;
    } else {
        screen_left = -(int)cap->layout.extra_left_cur;
        screen_right = 256 + (int)cap->layout.extra_right_cur;
    }

    /* The maintained CPU PPU treats hardware edges pinned to 0/255 as the
     * corresponding widened screen edge. */
    if (screen_left != 0 || screen_right != 256) {
        if (window.w1_left == 0) window.w1_left = screen_left;
        if (window.w1_right == 255) window.w1_right = screen_right - 1;
        if (window.w2_left == 0) window.w2_left = screen_left;
        if (window.w2_right == 255) window.w2_right = screen_right - 1;
    }
    if ((cap->layout.window_expand_layers & (1u << layer)) != 0) {
        if ((cap->layout.window_expand_windows & 1u) != 0 &&
            band->window1_left <= band->window1_right) {
            window.w1_left -= cap->layout.extra_left_cur;
            window.w1_right += cap->layout.extra_right_cur;
            if (window.w1_left < screen_left)
                window.w1_left = screen_left;
            if (window.w1_right >= screen_right)
                window.w1_right = screen_right - 1;
        }
        if ((cap->layout.window_expand_windows & 2u) != 0 &&
            band->window2_left <= band->window2_right) {
            window.w2_left -= cap->layout.extra_left_cur;
            window.w2_right += cap->layout.extra_right_cur;
            if (window.w2_left < screen_left)
                window.w2_left = screen_left;
            if (window.w2_right >= screen_right)
                window.w2_right = screen_right - 1;
        }
    }

    window.w1_enabled = (selector & 2u) != 0 &&
                        window.w1_left <= window.w1_right;
    window.w2_enabled = (selector & 8u) != 0 &&
                        window.w2_left <= window.w2_right;
    return window;
}

static bool WindowValue(const SemanticWindow *window, int screen_x) {
    bool w1 = screen_x >= window->w1_left &&
              screen_x <= window->w1_right;
    bool w2 = screen_x >= window->w2_left &&
              screen_x <= window->w2_right;
    if (window->w1_invert) w1 = !w1;
    if (window->w2_invert) w2 = !w2;
    if (!window->w1_enabled) return window->w2_enabled ? w2 : false;
    if (!window->w2_enabled) return w1;
    switch (window->logic) {
    case 0: return w1 || w2;
    case 1: return w1 && w2;
    case 2: return w1 != w2;
    default: return w1 == w2;
    }
}

static bool ModeAllows(unsigned mode, bool inside) {
    switch (mode & 3u) {
    case 0: return true;
    case 1: return inside;
    case 2: return !inside;
    default: return false;
    }
}

uint8_t snesrecomp_ppu_semantic_meta(unsigned source, bool palette_zero) {
    return (uint8_t)((source & 7u) |
                     (palette_zero ? SNESRECOMP_SEMANTIC_META_ZERO : 0u));
}

unsigned snesrecomp_ppu_semantic_source(uint8_t metadata) {
    return metadata & 7u;
}

bool snesrecomp_ppu_semantic_palette_zero(uint8_t metadata) {
    return (metadata & SNESRECOMP_SEMANTIC_META_ZERO) != 0;
}

bool snesrecomp_ppu_compile_semantic_input(
    const SnesPpuFrameCapture *cap, uint8_t *mask, uint8_t *obj_mask,
    size_t mask_pitch, SnesRecompSemanticLineState *lines,
    unsigned line_capacity) {
    unsigned next_y = 0;
    if (!cap || !cap->bands || !mask || !lines ||
        mask_pitch < cap->canvas_width || line_capacity < cap->visible_height)
        return false;
    for (unsigned bi = 0; bi < cap->band_count; bi++) {
        const SnesPpuRasterBand *band = &cap->bands[bi];
        SemanticWindow windows[5];
        SnesRecompSemanticLineState line;
        uint8_t *band_mask;
        uint8_t *band_obj_mask = NULL;
        if (band->y_begin != next_y || band->y_begin >= band->y_end ||
            band->y_end > cap->visible_height)
            return false;
        next_y = band->y_end;
        for (unsigned bg = 0; bg < 3u; bg++)
            windows[bg] = PrepareWindow(cap, band, bg);
        windows[3] = PrepareWindow(cap, band, 5u);
        windows[4] = PrepareWindow(cap, band, 4u);

        line.cgadsub = band->cgadsub;
        line.cgwsel = band->cgwsel;
        line.fixed_r5 = (uint8_t)(band->fixed_colour & 31u);
        line.fixed_g5 = (uint8_t)((band->fixed_colour >> 5) & 31u);
        line.fixed_b5 = (uint8_t)((band->fixed_colour >> 10) & 31u);
        line.brightness = band->brightness;
        line.forced_blank = band->forced_blank ? 1u : 0u;
        line.bg3_priority = band->bg3_priority ? 1u : 0u;

        band_mask = mask + (size_t)band->y_begin * mask_pitch;
        if (obj_mask)
            band_obj_mask = obj_mask + (size_t)band->y_begin * mask_pitch;
        for (unsigned x = 0; x < cap->canvas_width; x++) {
            const int sx = (int)x - (int)cap->canvas_extra;
            uint8_t bits = 0;
            for (unsigned bg = 0; bg < 3u; bg++) {
                const bool masked = WindowValue(&windows[bg], sx);
                if ((band->main_enable & (1u << bg)) &&
                    (!(band->main_window_enable & (1u << bg)) || !masked))
                    bits |= (uint8_t)(1u << bg);
                if ((band->sub_enable & (1u << bg)) &&
                    (!(band->sub_window_enable & (1u << bg)) || !masked))
                    bits |= (uint8_t)(1u << (bg + 3u));
            }
            {
                const bool inside = WindowValue(&windows[3], sx);
                const bool composed =
                    sx >= -(int)cap->layout.extra_left_cur &&
                    sx < 256 + (int)cap->layout.extra_right_cur;
                if (composed) {
                    if (ModeAllows(band->cgwsel >> 6, inside))
                        bits |= SNESRECOMP_SEMANTIC_MAIN_RGB;
                    if (ModeAllows(band->cgwsel >> 4, inside))
                        bits |= SNESRECOMP_SEMANTIC_MATH;
                }
            }
            band_mask[x] = bits;
            if (band_obj_mask) {
                const bool masked = WindowValue(&windows[4], sx);
                uint8_t obj_bits = 0;
                if ((band->main_enable & 0x10u) &&
                    (!(band->main_window_enable & 0x10u) || !masked))
                    obj_bits |= SNESRECOMP_SEMANTIC_OBJ_MAIN;
                if ((band->sub_enable & 0x10u) &&
                    (!(band->sub_window_enable & 0x10u) || !masked))
                    obj_bits |= SNESRECOMP_SEMANTIC_OBJ_SUB;
                band_obj_mask[x] = obj_bits;
            }
        }

        for (unsigned y = band->y_begin; y < band->y_end; y++) {
            lines[y] = line;
            if (y != band->y_begin) {
                memcpy(mask + (size_t)y * mask_pitch, band_mask,
                       cap->canvas_width);
                if (band_obj_mask)
                    memcpy(obj_mask + (size_t)y * mask_pitch, band_obj_mask,
                           cap->canvas_width);
            }
        }
    }
    return next_y == cap->visible_height;
}

bool snesrecomp_ppu_expand_semantic_scroll(
    const SnesPpuFrameCapture *cap,
    uint16_t *bg1_x, uint16_t *bg1_y,
    uint16_t *bg2_x, uint16_t *bg2_y,
    uint16_t *bg3_x, uint16_t *bg3_y, unsigned capacity) {
    uint16_t *xs[3] = { bg1_x, bg2_x, bg3_x };
    uint16_t *ys[3] = { bg1_y, bg2_y, bg3_y };
    unsigned next_y = 0;
    if (!cap || !cap->bands || !bg1_x || !bg1_y || !bg2_x || !bg2_y ||
        !bg3_x || !bg3_y || capacity < cap->visible_height)
        return false;
    for (unsigned bi = 0; bi < cap->band_count; bi++) {
        const SnesPpuRasterBand *band = &cap->bands[bi];
        if (band->y_begin != next_y || band->y_begin >= band->y_end ||
            band->y_end > cap->visible_height)
            return false;
        next_y = band->y_end;
        for (unsigned y = band->y_begin; y < band->y_end; y++) {
            for (unsigned bg = 0; bg < 3u; bg++) {
                xs[bg][y] = band->bg[bg].h_scroll;
                /* Visible row y is PPU line y+1; the cast preserves wrapping. */
                ys[bg][y] =
                    (uint16_t)(band->bg[bg].v_scroll + y + 1u);
            }
        }
    }
    return next_y == cap->visible_height;
}

void snesrecomp_ppu_build_rgb5_palette(uint8_t *dst_rgba,
                                       const uint16_t *cgram) {
    if (!dst_rgba || !cgram)
        return;
    for (unsigned i = 0; i < SNES_PPU_CGRAM_ENTRIES; i++) {
        const uint16_t c = cgram[i];
        dst_rgba[i * 4u + 0u] = (uint8_t)(c & 31u);
        dst_rgba[i * 4u + 1u] = (uint8_t)((c >> 5) & 31u);
        dst_rgba[i * 4u + 2u] = (uint8_t)((c >> 10) & 31u);
        dst_rgba[i * 4u + 3u] = 255u;
    }
}
