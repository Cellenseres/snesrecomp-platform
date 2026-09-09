#include "snesrecomp_platform/snes_ppu_mode7.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
    RAW_INIDISP = 0,
    RAW_BGMODE = 4,
    RAW_MOSAIC = 5,
    RAW_M7SEL = 12,
    RAW_SETINI = 13,
    RAW_M7A = 30,
    RAW_M7B = 32,
    RAW_M7C = 34,
    RAW_M7D = 36,
    RAW_M7X = 38,
    RAW_M7Y = 40,
    RAW_M7H = 42,
    RAW_M7V = 44,
    RAW_SCREEN_ENABLE = 58,
    RAW_SCREEN_WINDOW = 60,
    RAW_CGADSUB = 62,
    RAW_CGWSEL = 63,
};

enum {
    MODE7_X_FLIP = 0x01u,
    MODE7_Y_FLIP = 0x02u,
    MODE7_RESERVED = 0x3cu,
    MODE7_CHAR_FILL = 0x40u,
    MODE7_LARGE_FIELD = 0x80u,
};

static uint16_t RawU16(const uint8_t *p, unsigned offset) {
    return (uint16_t)(p[offset] | ((uint16_t)p[offset + 1u] << 8));
}

static int32_t RawS16(const uint8_t *p, unsigned offset) {
    return (int32_t)(int16_t)RawU16(p, offset);
}

static int32_t Sign13(uint16_t value) {
    value &= 0x1fffu;
    return (value & 0x1000u) ? (int32_t)value - 0x2000 : (int32_t)value;
}

static int32_t ClipMode7Scroll(int32_t value) {
    return (value & 0x2000) ? (value | ~1023) : (value & 1023);
}

static int64_t MaskProduct64(int32_t a, int32_t b) {
    return ((int64_t)a * b) & ~INT64_C(63);
}

static bool HostObjPolicyInert(const SnesPpuLayoutState *l) {
    return !l->hud_split_height && !l->hud_left_end &&
           !l->hud_right_start && !l->hud_oam_first_slot &&
           !l->hud_oam_slots && !l->hud_oam_height &&
           !l->hud_oam_first_slot2 && !l->hud_oam_slots2 &&
           !l->oam_left_hint_strict && !l->oam_right_hint_strict;
}

static bool BasicBandConsistent(const SnesPpuRasterBand *b) {
    const uint8_t *r = b->regs;
    return (r[RAW_INIDISP] & 0x0fu) == b->brightness &&
           (((r[RAW_INIDISP] & 0x80u) != 0) == b->forced_blank) &&
           (r[RAW_BGMODE] & 7u) == b->bg_mode &&
           r[RAW_MOSAIC] == b->mosaic &&
           r[RAW_SCREEN_ENABLE] == b->main_enable &&
           r[RAW_SCREEN_ENABLE + 1u] == b->sub_enable &&
           r[RAW_SCREEN_WINDOW] == b->main_window_enable &&
           r[RAW_SCREEN_WINDOW + 1u] == b->sub_window_enable &&
           r[RAW_CGADSUB] == b->cgadsub && r[RAW_CGWSEL] == b->cgwsel;
}

SnesPpuUnsupported snesrecomp_ppu_mode7_supports(
    const SnesPpuFrameCapture *cap) {
    unsigned next_y = 0;

    if (!cap || !cap->vram || !cap->cgram || !cap->bands ||
        !cap->band_count || cap->band_count > SNES_PPU_MAX_BANDS ||
        !cap->visible_height || cap->visible_height > SNES_PPU_MAX_BANDS ||
        cap->native_width != 256u || cap->canvas_width < cap->native_width ||
        cap->canvas_extra + cap->native_width > cap->canvas_width)
        return SNES_PPU_UNSUPPORTED_RASTER_STATE;
    if (cap->raster_memory_flags)
        return SNES_PPU_UNSUPPORTED_RASTER_MEMORY;
    if (!HostObjPolicyInert(&cap->layout))
        return SNES_PPU_UNSUPPORTED_LAYOUT_POLICY;

    for (unsigned i = 0; i < cap->band_count; i++) {
        const SnesPpuRasterBand *b = &cap->bands[i];
        const uint8_t *r = b->regs;
        const uint8_t m7sel = r[RAW_M7SEL];

        if (b->y_begin != next_y || b->y_begin >= b->y_end ||
            b->y_end > cap->visible_height || !BasicBandConsistent(b))
            return SNES_PPU_UNSUPPORTED_RASTER_STATE;
        next_y = b->y_end;

        /* A widened Mode 7 band is valid only when its non-repeating BG1
         * coverage stays inside the shared canvas. This makes the portable
         * contract explicit for both symmetric and asymmetric layouts. */
        if (b->bg[0].margin_repeats ||
            b->bg[0].margin_left > cap->canvas_extra ||
            b->bg[0].margin_right >
                cap->canvas_width - cap->canvas_extra - cap->native_width)
            return SNES_PPU_UNSUPPORTED_LAYOUT_POLICY;

        if (b->brightness > 15u)
            return SNES_PPU_UNSUPPORTED_RASTER_STATE;
        /* Forced blank produces black before any BG/OBJ semantics are read.
         * Accept arbitrary dormant mode state on those rows so transitions do
         * not fall back merely because the next scene is being prepared. */
        if (b->forced_blank)
            continue;
        if (r[RAW_SETINI] != 0u ||
            (m7sel & MODE7_RESERVED))
            return SNES_PPU_UNSUPPORTED_RASTER_STATE;
        if (b->bg_mode != 7u)
            return SNES_PPU_UNSUPPORTED_MODE;
        if (m7sel & (MODE7_CHAR_FILL | MODE7_LARGE_FIELD))
            return SNES_PPU_UNSUPPORTED_MAP_SIZE;

        /* BG1 and OBJ are the only drawable Mode 7 main-screen sources in
         * this subset. Bits for nonexistent/ExtBG layers are rejected rather
         * than silently ignored. */
        if (b->main_enable & (uint8_t)~0x11u)
            return SNES_PPU_UNSUPPORTED_MODE;
        /* An OBJ-only subscreen is inert while colour math is disabled.
         * Games may leave TS=$10 programmed in that state; it cannot affect
         * the displayed main screen and needs no OBJ replay. */
        if (b->sub_enable & (uint8_t)~0x10u)
            return SNES_PPU_UNSUPPORTED_SUBSCREEN;
        /* Window bits for a disabled main-screen source are inert. The
         * accepted subscreen subset is not composited at all because colour
         * math is off, so its window mask is inert as well. */
        if (b->main_window_enable & b->main_enable & 0x11u)
            return SNES_PPU_UNSUPPORTED_WINDOWS;
        if (b->cgwsel || b->cgadsub)
            return SNES_PPU_UNSUPPORTED_COLOUR_MATH;
        if ((b->mosaic & 0x0fu) && (b->mosaic >> 4u))
            return SNES_PPU_UNSUPPORTED_MOSAIC;
        if ((b->main_enable & 0x10u) && (!cap->oam || !cap->high_oam))
            return SNES_PPU_UNSUPPORTED_OBJ;

    }

    return next_y == cap->visible_height ? SNES_PPU_SUPPORTED
                                         : SNES_PPU_UNSUPPORTED_RASTER_STATE;
}

SnesPpuUnsupported snesrecomp_ppu_mode7_backend_supports(
    const SnesPpuFrameCapture *cap, bool backend_available) {
    const SnesPpuUnsupported semantic = snesrecomp_ppu_mode7_supports(cap);
    if (semantic != SNES_PPU_SUPPORTED)
        return semantic;
    return backend_available ? SNES_PPU_SUPPORTED
                             : SNES_PPU_UNSUPPORTED_BACKEND;
}

bool snesrecomp_ppu_mode7_compile_lines(
    const SnesPpuFrameCapture *cap, SnesRecompMode7Line *lines,
    unsigned capacity) {
    if (!lines || snesrecomp_ppu_mode7_supports(cap) != SNES_PPU_SUPPORTED ||
        capacity < cap->visible_height)
        return false;

    for (unsigned bi = 0; bi < cap->band_count; bi++) {
        const SnesPpuRasterBand *b = &cap->bands[bi];
        const uint8_t *r = b->regs;
        const int32_t a = RawS16(r, RAW_M7A);
        const int32_t mb = RawS16(r, RAW_M7B);
        const int32_t c = RawS16(r, RAW_M7C);
        const int32_t d = RawS16(r, RAW_M7D);
        const int32_t x_center = Sign13(RawU16(r, RAW_M7X));
        const int32_t y_center = Sign13(RawU16(r, RAW_M7Y));
        const int32_t h_scroll = Sign13(RawU16(r, RAW_M7H));
        const int32_t v_scroll = Sign13(RawU16(r, RAW_M7V));
        const int32_t clipped_h = ClipMode7Scroll(h_scroll - x_center);
        const int32_t clipped_v = ClipMode7Scroll(v_scroll - y_center);
        const bool x_flip = (r[RAW_M7SEL] & MODE7_X_FLIP) != 0;
        const bool y_flip = (r[RAW_M7SEL] & MODE7_Y_FLIP) != 0;
        for (unsigned y = b->y_begin; y < b->y_end; y++) {
            /* Existing capture row zero maps to PPU scanline one. */
            const int32_t ppu_y = (int32_t)y + 1;
            const int32_t ry = y_flip ? 255 - ppu_y : ppu_y;
            const int32_t rx0 = x_flip ? 255 : 0;
            const int64_t base_x =
                MaskProduct64(a, clipped_h) + MaskProduct64(mb, ry) +
                MaskProduct64(mb, clipped_v) + (int64_t)x_center * 256;
            const int64_t base_y =
                MaskProduct64(c, clipped_h) + MaskProduct64(d, ry) +
                MaskProduct64(d, clipped_v) + (int64_t)y_center * 256;
            SnesRecompMode7Line *out = &lines[y];

            out->start_x = (uint32_t)(base_x + (int64_t)a * rx0) &
                           SNESRECOMP_MODE7_COORD_MASK;
            out->start_y = (uint32_t)(base_y + (int64_t)c * rx0) &
                           SNESRECOMP_MODE7_COORD_MASK;
            out->step_x = x_flip ? -a : a;
            out->step_y = x_flip ? -c : c;
        }
    }
    return true;
}

unsigned snesrecomp_ppu_mode7_build_strips(
    SnesRecompBgStripVertex *verts, uint16_t *indices,
    const SnesRecompMode7Line *lines, unsigned line_count,
    unsigned canvas_width, unsigned canvas_extra,
    float scale_x, float scale_y) {
    unsigned nv = 0;
    unsigned ni = 0;
    float x0, x1;

    if (!verts || !indices || !lines || !line_count || !canvas_width ||
        canvas_extra + 256u > canvas_width)
        return 0;

    x0 = scale_x *
         (2.0f * ((float)canvas_extra / (float)canvas_width) - 1.0f);
    x1 = scale_x *
         (2.0f * ((float)(canvas_extra + 256u) /
                   (float)canvas_width) - 1.0f);

    for (unsigned line = 0; line < line_count; line++) {
        const SnesRecompMode7Line *m = &lines[line];
        const float y0 = scale_y *
            (1.0f - 2.0f * ((float)line / (float)line_count));
        const float y1 = scale_y *
            (1.0f - 2.0f * ((float)(line + 1u) / (float)line_count));
        const double sx = (double)(int32_t)m->start_x;
        const double sy = (double)(int32_t)m->start_y;
        const double dx = (double)m->step_x;
        const double dy = (double)m->step_y;
        const float wx0 = (float)(sx - dx * 0.5);
        const float wy0 = (float)(sy - dy * 0.5);
        const float wx1 = (float)(sx + dx * 255.5);
        const float wy1 = (float)(sy + dy * 255.5);
        SnesRecompBgStripVertex *v = &verts[nv];

        v[0].x = x0; v[0].y = y0; v[0].wx = wx0; v[0].wy = wy0;
        v[1].x = x1; v[1].y = y0; v[1].wx = wx1; v[1].wy = wy1;
        v[2].x = x0; v[2].y = y1; v[2].wx = wx0; v[2].wy = wy0;
        v[3].x = x1; v[3].y = y1; v[3].wx = wx1; v[3].wy = wy1;

        indices[ni + 0u] = (uint16_t)(nv + 0u);
        indices[ni + 1u] = (uint16_t)(nv + 1u);
        indices[ni + 2u] = (uint16_t)(nv + 2u);
        indices[ni + 3u] = (uint16_t)(nv + 2u);
        indices[ni + 4u] = (uint16_t)(nv + 1u);
        indices[ni + 5u] = (uint16_t)(nv + 3u);
        nv += SNESRECOMP_BG_STRIP_VERTS;
        ni += SNESRECOMP_BG_STRIP_INDICES;
    }
    return nv;
}

bool snesrecomp_ppu_mode7_unpack_vram(const uint16_t *vram,
                                      uint8_t *map_tex,
                                      uint8_t *char_tex) {
    if (!vram || (!map_tex && !char_tex))
        return false;
    for (unsigned i = 0; i < SNESRECOMP_MODE7_TEXTURE_TEXELS; i++) {
        const uint16_t word = vram[i];
        if (map_tex)
            map_tex[i] = (uint8_t)word;
        if (char_tex)
            char_tex[i] = (uint8_t)(word >> 8);
    }
    return true;
}

static int64_t FloorDiv64(int64_t numerator, int64_t denominator) {
    int64_t quotient = numerator / denominator;
    const int64_t remainder = numerator % denominator;
    if (remainder < 0)
        quotient--;
    return quotient;
}

static uint8_t Mode7Texel(const uint16_t *vram,
                          int64_t world_x, int64_t world_y) {
    const uint32_t x = (uint32_t)world_x & SNESRECOMP_MODE7_COORD_MASK;
    const uint32_t y = (uint32_t)world_y & SNESRECOMP_MODE7_COORD_MASK;
    const unsigned tile = vram[((y >> 11u) & 127u) * 128u +
                               ((x >> 11u) & 127u)] & 0xffu;
    return (uint8_t)(vram[tile * 64u + ((y >> 8u) & 7u) * 8u +
                               ((x >> 8u) & 7u)] >> 8u);
}

static uint8_t ObjTexel(const uint16_t *vram,
                        const SnesRecompObjSliver *sliver,
                        unsigned screen_x) {
    const unsigned column = sliver->flip_h ? 7u - screen_x : screen_x;
    const unsigned bit = 7u - column;
    const unsigned address = ((unsigned)sliver->tile * 16u +
                              (unsigned)sliver->row) & 0x7fffu;
    const uint16_t low = vram[address];
    const uint16_t high = vram[(address + 8u) & 0x7fffu];
    return (uint8_t)(((low >> bit) & 1u) |
                     (((low >> (bit + 8u)) & 1u) << 1u) |
                     (((high >> bit) & 1u) << 2u) |
                     (((high >> (bit + 8u)) & 1u) << 3u));
}

bool snesrecomp_ppu_mode7_render_reference(
    const SnesPpuFrameCapture *cap,
    const SnesRecompMode7Line *lines,
    unsigned line_count,
    const SnesRecompObjFrame *obj,
    unsigned scale,
    uint8_t *pixels,
    size_t pitch) {
    uint8_t *obj_plane = NULL;
    size_t native_pixels, output_width;
    unsigned band_index = 0;
    bool wants_obj = false;

    if (!pixels || !lines || scale < 1u || scale > 4u ||
        snesrecomp_ppu_mode7_supports(cap) != SNES_PPU_SUPPORTED ||
        line_count < cap->visible_height)
        return false;
    output_width = (size_t)cap->canvas_width * scale;
    if (pitch < output_width ||
        cap->visible_height > SIZE_MAX / cap->canvas_width)
        return false;
    for (unsigned i = 0; i < cap->band_count; i++)
        if (!cap->bands[i].forced_blank &&
            (cap->bands[i].main_enable & 0x10u))
            wants_obj = true;
    if (wants_obj && !obj)
        return false;
    native_pixels = (size_t)cap->canvas_width * cap->visible_height;
    obj_plane = (uint8_t *)calloc(native_pixels, 2u);
    if (!obj_plane)
        return false;

    if (obj) {
        for (unsigned i = 0; i < obj->count; i++) {
            const SnesRecompObjSliver *sliver = &obj->slivers[i];
            if (sliver->line >= cap->visible_height)
                continue;
            for (unsigned x = 0; x < 8u; x++) {
                const int canvas_x = sliver->screen_x + (int)x +
                                     (int)cap->canvas_extra;
                uint8_t index, *dst;
                if (canvas_x < 0 || canvas_x >= cap->canvas_width)
                    continue;
                index = ObjTexel(cap->vram, sliver, x);
                if (!index)
                    continue;
                dst = obj_plane +
                    ((size_t)sliver->line * cap->canvas_width +
                     (unsigned)canvas_x) * 2u;
                dst[0] = (uint8_t)(sliver->palette_base + index);
                dst[1] = sliver->priority;
            }
        }
    }

    for (unsigned y = 0; y < cap->visible_height; y++) {
        const SnesPpuRasterBand *band;
        while (band_index + 1u < cap->band_count &&
               y >= cap->bands[band_index].y_end)
            band_index++;
        band = &cap->bands[band_index];
        for (unsigned sub_y = 0; sub_y < scale; sub_y++) {
            uint8_t *row = pixels + (size_t)(y * scale + sub_y) * pitch;
            for (unsigned hx = 0; hx < output_width; hx++) {
                const unsigned native_x = hx / scale;
                const int local_hx = (int)hx -
                                     (int)cap->canvas_extra * (int)scale;
                uint8_t bg = 0, obj_index, obj_priority;
                if (!band->forced_blank && (band->main_enable & 1u) &&
                    local_hx >=
                        -(int)band->bg[0].margin_left * (int)scale &&
                    local_hx <
                        (256 + (int)band->bg[0].margin_right) * (int)scale) {
                    const int64_t numerator =
                        (int64_t)2 * (local_hx + 1) - (int64_t)scale;
                    const int64_t denominator = (int64_t)2 * scale;
                    const int64_t wx = (int64_t)lines[y].start_x +
                        FloorDiv64((int64_t)lines[y].step_x * numerator,
                                   denominator);
                    const int64_t wy = (int64_t)lines[y].start_y +
                        FloorDiv64((int64_t)lines[y].step_y * numerator,
                                   denominator);
                    bg = Mode7Texel(cap->vram, wx, wy);
                }
                obj_index = obj_plane[
                    ((size_t)y * cap->canvas_width + native_x) * 2u];
                obj_priority = obj_plane[
                    ((size_t)y * cap->canvas_width + native_x) * 2u + 1u];
                if (!band->forced_blank && (band->main_enable & 0x10u) &&
                    obj_index && (!bg || obj_priority > 0u))
                    row[hx] = obj_index;
                else
                    row[hx] = band->forced_blank ? 0u : bg;
            }
        }
    }
    free(obj_plane);
    return true;
}
