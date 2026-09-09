#include "snesrecomp_platform/snes_ppu_obj.h"

#include <string.h>

/* Offsets into the 0x40-byte PPU register snapshot. Kept numeric here for the
 * same reason the predicate does: this library must not depend on the
 * emulator's struct layout, and the game-side capture static-asserts them. */
enum { RAW_OBSEL = 1, RAW_OAMADDL = 2, RAW_OAMADDH = 3 };

/* OBSEL. The two sizes a sprite chooses between are a pair selected by bits
 * 7:5; the per-sprite high-OAM bit picks which of the pair applies. */
static const uint8_t k_sprite_sizes[8][2] = {
    { 8, 16 }, { 8, 32 }, { 8, 64 }, { 16, 32 },
    { 16, 64 }, { 32, 64 }, { 16, 32 }, { 16, 32 },
};

static unsigned ObjTileAdr1(uint8_t obsel) {
    return (unsigned)(obsel & 7u) << 13;
}

static unsigned ObjTileAdr2(uint8_t obsel) {
    return ObjTileAdr1(obsel) + ((((unsigned)obsel & 0x18u) + 8u) << 9);
}

/* The ninth X bit lives in the high table, and a sprite whose decoded X sits
 * past the widened right edge is the hardware's negative X instead. The strict
 * per-slot hint decode is host policy the predicate has already required to be
 * off, so this is the whole rule. */
static int DecodeOamX(const uint16_t *oam, const uint8_t *high_oam,
                      uint8_t index, int extra_right) {
    int x = oam[index] & 0xffu;
    x |= (int)((high_oam[index >> 3] >> (index & 7u)) & 1u) << 8;
    if (x >= 256 + extra_right)
        x -= 512;
    return x;
}

static const SnesPpuRasterBand *BandForLine(const SnesPpuFrameCapture *cap,
                                            unsigned y) {
    for (unsigned i = 0; i < cap->band_count; i++)
        if (y >= cap->bands[i].y_begin && y < cap->bands[i].y_end)
            return &cap->bands[i];
    return NULL;
}

bool snesrecomp_ppu_obj_evaluate(const SnesPpuFrameCapture *cap,
                                 SnesRecompObjFrame *frame) {
    /* kPpuRenderFlags_NoSpriteLimits. Named here rather than included from
     * the emulator header, which this library may not see. */
    enum { RENDER_FLAG_NO_SPRITE_LIMITS = 8u };
    const bool limits =
        (cap && (cap->layout.render_flags & RENDER_FLAG_NO_SPRITE_LIMITS)) == 0;
    int extra_left, extra_right;

    if (!cap || !cap->bands || !cap->band_count || !cap->oam ||
        !cap->high_oam || !frame || !frame->slivers || !frame->line_priority ||
        frame->line_capacity < cap->visible_height)
        return false;

    frame->count = 0;
    frame->range_over = false;
    frame->time_over = false;
    memset(frame->line_priority, 0, cap->visible_height);

    extra_left = (int)cap->layout.extra_left_cur;
    extra_right = (int)cap->layout.extra_right_cur;

    for (unsigned y = 0; y < cap->visible_height; y++) {
        const SnesPpuRasterBand *band = BandForLine(cap, y);
        uint8_t found[SNES_PPU_OAM_SLOTS];
        unsigned found_count = 0;
        int tiles_found = 0;
        uint8_t obsel, index;
        unsigned obj_adr1, obj_adr2;

        if (!band)
            return false;
        /* Forced blank evaluates no sprites at all, and OBJ disabled on both
         * screens leaves the line buffer unread. Skipping both here keeps the
         * sliver bound meaningful on frames that only blank. */
        if (band->forced_blank ||
            !((band->main_enable | band->sub_enable) & 0x10u))
            continue;

        obsel = band->regs[RAW_OBSEL];
        obj_adr1 = ObjTileAdr1(obsel);
        obj_adr2 = ObjTileAdr2(obsel);

        /* OAM priority rotation: $2103 bit 7 starts the range walk at the
         * current OAM address instead of slot 0, which changes both which
         * sprites survive the 32-sprite cap and who wins an overlap. */
        index = (band->regs[RAW_OAMADDH] & 0x80u)
            ? (uint8_t)(band->regs[RAW_OAMADDL] & 0xfeu) : 0u;

        /* Range evaluation walks OAM forward from that start. */
        for (unsigned i = 0; i < SNES_PPU_OAM_SLOTS; i++) {
            const uint8_t sprite_y = (uint8_t)(cap->oam[index] >> 8);
            const uint8_t row = (uint8_t)(y - sprite_y);
            const int size = k_sprite_sizes[obsel >> 5]
                [(cap->high_oam[index >> 3] >> ((index & 7u) + 1u)) & 1u];
            if (row < size) {
                const int x = DecodeOamX(cap->oam, cap->high_oam, index,
                                         extra_right);
                if (x + size > -extra_left) {
                    found_count++;
                    if (found_count > SNESRECOMP_OBJ_LINE_SPRITES && limits) {
                        frame->range_over = true;
                        found_count = SNESRECOMP_OBJ_LINE_SPRITES;
                        break;
                    }
                    if (found_count > SNES_PPU_OAM_SLOTS)
                        return false;
                    found[found_count - 1u] = index;
                }
            }
            index = (uint8_t)(index + 2u);
        }

        /* Tile fetch walks the accepted sprites backward, and each sliver
         * overwrites what is already there. The last write therefore comes
         * from the lowest OAM index, which is the hardware's tie-break. */
        for (unsigned i = found_count; i > 0u; i--) {
            const uint8_t slot_index = found[i - 1u];
            const uint8_t sprite_y = (uint8_t)(cap->oam[slot_index] >> 8);
            const int size = k_sprite_sizes[obsel >> 5]
                [(cap->high_oam[slot_index >> 3] >>
                  ((slot_index & 7u) + 1u)) & 1u];
            const int x = DecodeOamX(cap->oam, cap->high_oam, slot_index,
                                     extra_right);
            const unsigned oam1 = cap->oam[slot_index + 1u];
            const unsigned obj_adr = (oam1 & 0x100u) ? obj_adr2 : obj_adr1;
            const uint8_t palette_base =
                (uint8_t)(0x80u + 16u * ((oam1 & 0xe00u) >> 9));
            const uint8_t priority = (uint8_t)((oam1 & 0x3000u) >> 12);
            const uint8_t math_eligible = (oam1 & 0x800u) ? 1u : 0u;
            int row = (uint8_t)(y - sprite_y);
            bool stop = false;

            if (oam1 & 0x8000u)                       /* vertical flip */
                row = size - 1 - row;

            for (int col = 0; col < size; col += 8) {
                unsigned used_col, used_tile, word;
                SnesRecompObjSliver *sliver;
                if (col + x <= -8 - extra_left ||
                    col + x >= 256 + extra_right)
                    continue;
                tiles_found++;
                if (tiles_found > SNESRECOMP_OBJ_LINE_SLIVERS && limits) {
                    frame->time_over = true;
                    stop = true;
                    break;
                }
                used_col = (oam1 & 0x4000u)
                    ? (unsigned)(size - 1 - col) : (unsigned)col;
                used_tile = ((((oam1 & 0xffu) >> 4) + ((unsigned)row >> 3))
                             << 4) |
                            ((((oam1 & 0xfu) + (used_col >> 3))) & 0xfu);
                word = (obj_adr + used_tile * 16u) & 0x7fffu;

                if (frame->count >= frame->capacity)
                    return false;
                sliver = &frame->slivers[frame->count++];
                sliver->screen_x = (int16_t)(col + x);
                sliver->line = (uint16_t)y;
                sliver->tile = (uint16_t)(word >> 4);
                sliver->row = (uint8_t)(row & 7);
                sliver->palette_base = palette_base;
                sliver->priority = priority;
                sliver->flip_h = (oam1 & 0x4000u) ? 1u : 0u;
                sliver->oam_slot = (uint8_t)(slot_index >> 1);
                sliver->math_eligible = math_eligible;
                frame->line_priority[y] |= (uint8_t)(1u << priority);
            }
            if (stop)
                break;
        }
    }
    return true;
}

unsigned snesrecomp_ppu_obj_build_geometry(
    const SnesRecompObjFrame *frame,
    unsigned canvas_width, unsigned canvas_extra, unsigned lines,
    unsigned atlas_tiles_x, float scale_x, float scale_y,
    SnesRecompObjVertex *verts, uint16_t *indices, unsigned vert_capacity) {
    unsigned nv = 0, ni = 0;

    if (!frame || !frame->slivers || !verts || !indices || !canvas_width ||
        !lines || !atlas_tiles_x ||
        frame->count * SNESRECOMP_OBJ_SLIVER_VERTS > vert_capacity ||
        frame->count * SNESRECOMP_OBJ_SLIVER_VERTS > 0xffffu)
        return 0;

    for (unsigned i = 0; i < frame->count; i++) {
        const SnesRecompObjSliver *s = &frame->slivers[i];
        const float c0 = (float)((int)s->screen_x + (int)canvas_extra);
        const float x0 = scale_x * (2.0f * (c0 / (float)canvas_width) - 1.0f);
        const float x1 =
            scale_x * (2.0f * ((c0 + 8.0f) / (float)canvas_width) - 1.0f);
        /* Same expression as the background strips, so an OBJ quad and a BG
         * strip for the same line land on identical target rows. */
        const float y0 =
            scale_y * (1.0f - 2.0f * ((float)s->line / (float)lines));
        const float y1 =
            scale_y * (1.0f - 2.0f * ((float)(s->line + 1u) / (float)lines));
        const float tile_row = (float)(s->tile / atlas_tiles_x);
        const float tile_col = (float)(s->tile % atlas_tiles_x);
        const float ua = tile_col * 8.0f;
        const float ub = ua + 8.0f;
        const float u0 = s->flip_h ? ub : ua;
        const float u1 = s->flip_h ? ua : ub;
        const float v = tile_row * 8.0f + (float)s->row + 0.5f;
        const float palette = (float)s->palette_base;
        const float flags = (float)s->priority + 4.0f * (float)s->math_eligible;
        SnesRecompObjVertex *out = &verts[nv];

        out[0].x = x0; out[0].y = y0; out[0].u = u0; out[0].v = v;
        out[1].x = x1; out[1].y = y0; out[1].u = u1; out[1].v = v;
        out[2].x = x0; out[2].y = y1; out[2].u = u0; out[2].v = v;
        out[3].x = x1; out[3].y = y1; out[3].u = u1; out[3].v = v;
        for (unsigned k = 0; k < SNESRECOMP_OBJ_SLIVER_VERTS; k++) {
            out[k].palette = palette;
            out[k].flags = flags;
        }

        indices[ni + 0] = (uint16_t)(nv + 0u);
        indices[ni + 1] = (uint16_t)(nv + 1u);
        indices[ni + 2] = (uint16_t)(nv + 2u);
        indices[ni + 3] = (uint16_t)(nv + 2u);
        indices[ni + 4] = (uint16_t)(nv + 1u);
        indices[ni + 5] = (uint16_t)(nv + 3u);

        nv += SNESRECOMP_OBJ_SLIVER_VERTS;
        ni += SNESRECOMP_OBJ_SLIVER_INDICES;
    }
    return nv;
}
