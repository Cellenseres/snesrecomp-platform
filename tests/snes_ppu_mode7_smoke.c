#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "snesrecomp_platform/snes_ppu_mode7.h"

static uint16_t s_vram[SNES_PPU_VRAM_WORDS];
static uint16_t s_cgram[SNES_PPU_CGRAM_ENTRIES];
static uint16_t s_oam[SNES_PPU_OAM_WORDS];
static uint8_t s_high_oam[SNES_PPU_HIGH_OAM_BYTES];
static uint8_t s_reference_a[342 * 224 * 4 * 4];
static uint8_t s_reference_b[342 * 224 * 4 * 4];

static void Put16(uint8_t *p, unsigned offset, int value) {
    const uint16_t v = (uint16_t)value;
    p[offset] = (uint8_t)v;
    p[offset + 1u] = (uint8_t)(v >> 8);
}

static SnesPpuFrameCapture MakeCapture(SnesPpuRasterBand *band) {
    SnesPpuFrameCapture cap;
    memset(&cap, 0, sizeof cap);
    memset(band, 0, sizeof *band);
    band->y_end = 224;
    band->bg_mode = 7;
    band->main_enable = 0x11;
    band->brightness = 15;
    band->regs[0] = 15;
    band->regs[4] = 7;
    band->regs[58] = 0x11;
    Put16(band->regs, 30, 256); /* A */
    Put16(band->regs, 36, 256); /* D */

    cap.vram = s_vram;
    cap.cgram = s_cgram;
    cap.oam = s_oam;
    cap.high_oam = s_high_oam;
    cap.bands = band;
    cap.band_count = 1;
    cap.native_width = 256;
    cap.canvas_width = 342;
    cap.canvas_extra = 43;
    cap.visible_height = 224;
    return cap;
}

static int Expect(int condition, const char *name) {
    if (condition)
        return 1;
    fprintf(stderr, "Mode 7 case failed: %s\n", name);
    return 0;
}

/* Mirrors the fragment shader after interpolation has produced its signed
 * 8.8 coordinate. This test verifies the two-texture representation and both
 * dependent lookups against the core's direct VRAM interpretation. */
static uint8_t SampleTextureViews(const uint8_t *map_tex,
                                  const uint8_t *char_tex,
                                  int32_t world_x, int32_t world_y) {
    const uint32_t x = (uint32_t)world_x & SNESRECOMP_MODE7_COORD_MASK;
    const uint32_t y = (uint32_t)world_y & SNESRECOMP_MODE7_COORD_MASK;
    const unsigned px = x >> 8;
    const unsigned py = y >> 8;
    const unsigned tile = map_tex[((py >> 3) * 128u) + (px >> 3)];
    return char_tex[tile * 64u + (py & 7u) * 8u + (px & 7u)];
}

static uint8_t SampleCoreVram(const uint16_t *vram,
                              int32_t world_x, int32_t world_y) {
    const uint32_t x = (uint32_t)world_x & SNESRECOMP_MODE7_COORD_MASK;
    const uint32_t y = (uint32_t)world_y & SNESRECOMP_MODE7_COORD_MASK;
    const unsigned tile = vram[((y >> 11) & 127u) * 128u +
                               ((x >> 11) & 127u)] & 0xffu;
    return (uint8_t)(vram[tile * 64u + ((y >> 8) & 7u) * 8u +
                               ((x >> 8) & 7u)] >> 8);
}

int main(void) {
    SnesPpuRasterBand band;
    SnesPpuFrameCapture cap = MakeCapture(&band);
    SnesRecompMode7Line lines[224];
    SnesRecompBgStripVertex verts[224 * SNESRECOMP_BG_STRIP_VERTS];
    uint16_t indices[224 * SNESRECOMP_BG_STRIP_INDICES];
    uint8_t map_tex[SNESRECOMP_MODE7_TEXTURE_TEXELS];
    uint8_t char_tex[SNESRECOMP_MODE7_TEXTURE_TEXELS];
    int passed = 1;

    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_SUPPORTED,
                     "basic eligibility");
    band.bg[0].margin_left = 43;
    band.bg[0].margin_right = 43;
    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_SUPPORTED,
                     "symmetric canvas margins eligible");
    cap.canvas_width = 298;
    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_UNSUPPORTED_RASTER_STATE,
                     "native viewport outside canvas fails closed");
    cap.canvas_width = 342;
    band.bg[0].margin_left = 0;
    band.bg[0].margin_right = 0;
    passed &= Expect(snesrecomp_ppu_mode7_compile_lines(
                         &cap, lines, sizeof lines / sizeof lines[0]),
                     "identity compile");
    passed &= Expect(lines[0].start_x == 0 && lines[0].start_y == 256 &&
                         lines[0].step_x == 256 && lines[0].step_y == 0,
                     "identity first visible line");
    passed &= Expect(lines[223].start_y == 224u * 256u,
                     "identity last visible line");
    passed &= Expect(snesrecomp_ppu_mode7_build_strips(
                         verts, indices, lines, 224, 342, 43, 1.0f,
                         224.0f / 225.0f) == 224u * 4u,
                     "strip geometry");
    passed &= Expect(verts[0].wx == -128.0f && verts[1].wx == 65408.0f &&
                         verts[0].wy == 256.0f && verts[1].wy == 256.0f,
                     "fragment-centre affine bias");

    s_vram[0] = 0x3412u;
    s_vram[127u * 128u + 127u] = 0xabcd;
    passed &= Expect(snesrecomp_ppu_mode7_unpack_vram(
                         s_vram, map_tex, char_tex),
                     "VRAM texture split");
    passed &= Expect(map_tex[0] == 0x12u && char_tex[0] == 0x34u &&
                         map_tex[16383] == 0xcdu &&
                         char_tex[16383] == 0xabu,
                     "VRAM byte views");

    {
        uint32_t random = 0x6d2b79f5u;
        for (unsigned i = 0; i < SNESRECOMP_MODE7_TEXTURE_TEXELS; i++) {
            random = random * 1664525u + 1013904223u;
            s_vram[i] = (uint16_t)(random >> 8);
        }
        passed &= Expect(snesrecomp_ppu_mode7_unpack_vram(
                             s_vram, map_tex, char_tex),
                         "shader-view corpus split");
        for (unsigned i = 0; i < 200000u && passed; i++) {
            int32_t x, y;
            random = random * 1664525u + 1013904223u;
            x = (int32_t)(random & 0x00ffffffu) - 0x00800000;
            random = random * 1664525u + 1013904223u;
            y = (int32_t)(random & 0x00ffffffu) - 0x00800000;
            passed &= Expect(SampleTextureViews(map_tex, char_tex, x, y) ==
                                 SampleCoreVram(s_vram, x, y),
                             "shader texture addressing");
        }
    }

    band.regs[12] = 1; /* X flip */
    passed &= Expect(snesrecomp_ppu_mode7_compile_lines(
                         &cap, lines, sizeof lines / sizeof lines[0]),
                     "x flip compile");
    passed &= Expect(lines[0].start_x == 255u * 256u &&
                         lines[0].step_x == -256,
                     "x flip affine state");

    band.regs[12] = 2; /* Y flip */
    passed &= Expect(snesrecomp_ppu_mode7_compile_lines(
                         &cap, lines, sizeof lines / sizeof lines[0]),
                     "y flip compile");
    passed &= Expect(lines[0].start_y == 254u * 256u,
                     "y flip first visible line");

    band.regs[12] = 0;
    Put16(band.regs, 38, 0x1fff); /* X center = -1 */
    Put16(band.regs, 42, 0x1fff); /* H scroll = -1 */
    passed &= Expect(snesrecomp_ppu_mode7_compile_lines(
                         &cap, lines, sizeof lines / sizeof lines[0]),
                     "signed 13-bit compile");
    passed &= Expect(lines[0].start_x == SNESRECOMP_MODE7_COORD_MASK - 255u,
                     "signed 13-bit center");

    cap = MakeCapture(&band);
    Put16(band.regs, 30, 0);    /* A */
    Put16(band.regs, 32, 256);  /* B */
    Put16(band.regs, 34, -256); /* C */
    Put16(band.regs, 36, 0);    /* D */
    passed &= Expect(snesrecomp_ppu_mode7_compile_lines(
                         &cap, lines, sizeof lines / sizeof lines[0]),
                     "rotation compile");
    passed &= Expect(lines[0].start_x == 256u &&
                         lines[0].start_y == 0u &&
                         lines[0].step_x == 0 &&
                         lines[0].step_y == -256,
                     "quarter-turn affine state");

    cap = MakeCapture(&band);
    Put16(band.regs, 30, 128); /* A: half-speed X */
    Put16(band.regs, 36, 512); /* D: double-speed Y */
    passed &= Expect(snesrecomp_ppu_mode7_compile_lines(
                         &cap, lines, sizeof lines / sizeof lines[0]),
                     "nonuniform scale compile");
    passed &= Expect(lines[0].start_y == 512u &&
                         lines[0].step_x == 128,
                     "nonuniform scale affine state");

    /* Deterministic 2x reference: a 512-unit X step samples texels 0 and 1
     * on the two HD subpixels, while both HD subrows use capture line zero. */
    memset(s_vram, 0, sizeof s_vram);
    cap = MakeCapture(&band);
    cap.canvas_width = 256;
    cap.canvas_extra = 0;
    band.main_enable = band.regs[58] = 1;
    Put16(band.regs, 30, 512);
    Put16(band.regs, 36, 256);
    s_vram[0] = 1u; /* map (0,0) -> character 1 */
    for (unsigned row = 0; row < 8u; row++)
        for (unsigned x = 0; x < 8u; x++)
            s_vram[64u + row * 8u + x] =
                (uint16_t)((x + 1u) << 8u);
    passed &= Expect(snesrecomp_ppu_mode7_compile_lines(
                         &cap, lines, sizeof lines / sizeof lines[0]),
                     "reference affine compile");
    passed &= Expect(snesrecomp_ppu_mode7_render_reference(
                         &cap, lines, 224, NULL, 2,
                         s_reference_a, 512),
                     "reference render");
    passed &= Expect(s_reference_a[0] == 1u &&
                         s_reference_a[1] == 2u &&
                         s_reference_a[512] == 1u &&
                         s_reference_a[513] == 2u,
                     "subpixel and native-line mapping");
    passed &= Expect(snesrecomp_ppu_mode7_render_reference(
                         &cap, lines, 224, NULL, 2,
                         s_reference_b, 512) &&
                         memcmp(s_reference_a, s_reference_b,
                                512u * 448u) == 0,
                     "deterministic reference output");

    /* Every HD scale uses real affine samples across screen X -43..298.
     * Filling both Mode 7 byte views with one non-zero tile makes a missed
     * margin immediately visible as palette index zero. */
    cap = MakeCapture(&band);
    band.main_enable = band.regs[58] = 1;
    band.bg[0].margin_left = 43;
    band.bg[0].margin_right = 43;
    for (unsigned i = 0; i < SNESRECOMP_MODE7_TEXTURE_TEXELS; i++)
        s_vram[i] = 0x0701u;
    passed &= Expect(snesrecomp_ppu_mode7_compile_lines(
                         &cap, lines, sizeof lines / sizeof lines[0]),
                     "widescreen affine compile");
    for (unsigned scale = 1; scale <= 4; scale *= 2u) {
        const size_t width = (size_t)cap.canvas_width * scale;
        passed &= Expect(snesrecomp_ppu_mode7_render_reference(
                             &cap, lines, 224, NULL, scale,
                             s_reference_a, width),
                         scale == 1u ? "widescreen reference 1x" :
                         scale == 2u ? "widescreen reference 2x" :
                                       "widescreen reference 4x");
        passed &= Expect(s_reference_a[0] == 7u &&
                             s_reference_a[width - 1u] == 7u,
                         scale == 1u ? "X -43 and 298 sampled at 1x" :
                         scale == 2u ? "X -43 and 298 sampled at 2x" :
                                       "X -43 and 298 sampled at 4x");
    }

    band.forced_blank = true;
    band.regs[0] = 0x8fu;
    passed &= Expect(snesrecomp_ppu_mode7_render_reference(
                         &cap, lines, 224, NULL, 2,
                         s_reference_a, 684) &&
                         s_reference_a[0] == 0u &&
                         s_reference_a[683] == 0u,
                     "forced blank blacks widened HD row");
    band.forced_blank = false;
    band.regs[0] = 15u;

    cap.canvas_extra = 50;
    band.bg[0].margin_left = 43;
    band.bg[0].margin_right = 20;
    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_SUPPORTED,
                     "asymmetric bounded margins eligible");
    passed &= Expect(snesrecomp_ppu_mode7_render_reference(
                         &cap, lines, 224, NULL, 1,
                         s_reference_a, 342),
                     "asymmetric margin render");
    passed &= Expect(s_reference_a[6] == 0u &&
                         s_reference_a[7] == 7u &&
                         s_reference_a[325] == 7u &&
                         s_reference_a[326] == 0u,
                     "asymmetric margins respect exact bounds");
    band.bg[0].margin_left = 51;
    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_UNSUPPORTED_LAYOUT_POLICY,
                     "left margin outside canvas fails closed");
    band.bg[0].margin_left = 43;
    band.bg[0].margin_right = 37;
    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_UNSUPPORTED_LAYOUT_POLICY,
                     "right margin outside canvas fails closed");
    band.bg[0].margin_right = 20;
    band.bg[0].margin_repeats = true;
    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_UNSUPPORTED_LAYOUT_POLICY,
                     "repeating Mode 7 margin fails closed");

    /* Exercise the margin sampling with rotation, non-uniform scale and both
     * flips. Expected texels use the compiled affine line, so negative and
     * greater-than-255 screen positions must wrap through the same 18 bits. */
    {
        uint32_t random = 0x91e10da5u;
        int64_t left_x, left_y, right_x, right_y;
        cap = MakeCapture(&band);
        band.main_enable = band.regs[58] = 1;
        band.bg[0].margin_left = 43;
        band.bg[0].margin_right = 43;
        band.regs[12] = 3u;
        Put16(band.regs, 30, 128);
        Put16(band.regs, 32, 64);
        Put16(band.regs, 34, -256);
        Put16(band.regs, 36, 512);
        for (unsigned i = 0; i < SNESRECOMP_MODE7_TEXTURE_TEXELS; i++) {
            random = random * 1664525u + 1013904223u;
            s_vram[i] = (uint16_t)(random >> 8u);
        }
        passed &= Expect(snesrecomp_ppu_mode7_compile_lines(
                             &cap, lines,
                             sizeof lines / sizeof lines[0]) &&
                             snesrecomp_ppu_mode7_render_reference(
                                 &cap, lines, 224, NULL, 1,
                                 s_reference_a, 342),
                         "transformed widescreen margin render");
        left_x = (int64_t)lines[0].start_x -
                 (int64_t)lines[0].step_x * 43;
        left_y = (int64_t)lines[0].start_y -
                 (int64_t)lines[0].step_y * 43;
        right_x = (int64_t)lines[0].start_x +
                  (int64_t)lines[0].step_x * 298;
        right_y = (int64_t)lines[0].start_y +
                  (int64_t)lines[0].step_y * 298;
        passed &= Expect(
            s_reference_a[0] == SampleCoreVram(
                s_vram, (int32_t)left_x, (int32_t)left_y) &&
                s_reference_a[341] ==
                    SampleCoreVram(
                        s_vram, (int32_t)right_x, (int32_t)right_y),
            "transformed margins share 18-bit affine wrap");
    }

    /* Restore the deterministic native corpus used by the raster-band test
     * below; the margin cases deliberately replaced both VRAM byte views. */
    memset(s_vram, 0, sizeof s_vram);
    cap = MakeCapture(&band);
    cap.canvas_width = 256;
    cap.canvas_extra = 0;
    band.main_enable = band.regs[58] = 1;
    Put16(band.regs, 30, 512);
    Put16(band.regs, 36, 256);
    s_vram[0] = 1u;
    for (unsigned row = 0; row < 8u; row++)
        for (unsigned x = 0; x < 8u; x++)
            s_vram[64u + row * 8u + x] =
                (uint16_t)((x + 1u) << 8u);

    {
        SnesPpuRasterBand raster_bands[2];
        raster_bands[0] = band;
        raster_bands[0].y_begin = 0;
        raster_bands[0].y_end = 1;
        raster_bands[1] = band;
        raster_bands[1].y_begin = 1;
        raster_bands[1].y_end = 224;
        raster_bands[1].forced_blank = true;
        raster_bands[1].regs[0] = 0x8fu;
        cap.bands = raster_bands;
        cap.band_count = 2;
        passed &= Expect(snesrecomp_ppu_mode7_compile_lines(
                             &cap, lines,
                             sizeof lines / sizeof lines[0]) &&
                             snesrecomp_ppu_mode7_render_reference(
                                 &cap, lines, 224, NULL, 2,
                                 s_reference_a, 512),
                         "raster-band boundary render");
        passed &= Expect(s_reference_a[0] == 1u &&
                             s_reference_a[512] == 1u &&
                             s_reference_a[1024] == 0u,
                         "both subrows select their native raster line");
    }

    /* Wrapped negative coordinates still address the 1024x1024 plane. */
    cap = MakeCapture(&band);
    cap.canvas_width = 256;
    cap.canvas_extra = 0;
    band.main_enable = band.regs[58] = 1;
    s_vram[127u * 128u + 127u] =
        (uint16_t)((s_vram[127u * 128u + 127u] & 0xff00u) | 2u);
    s_vram[2u * 64u + 63u] = 9u << 8u;
    lines[0].start_x = SNESRECOMP_MODE7_COORD_MASK - 255u;
    lines[0].start_y = SNESRECOMP_MODE7_COORD_MASK - 255u;
    lines[0].step_x = -256;
    lines[0].step_y = 0;
    passed &= Expect(snesrecomp_ppu_mode7_render_reference(
                         &cap, lines, 224, NULL, 1,
                         s_reference_a, 256),
                     "negative wrap render");
    passed &= Expect(s_reference_a[0] == 9u,
                     "negative coordinate wrap");

    /* OBJ priority is applied only after OAM-order resolution. Priority zero
     * remains behind an opaque Mode 7 BG1 pixel; priority one wins. */
    {
        SnesRecompObjSliver sliver;
        SnesRecompObjFrame obj;
        memset(&sliver, 0, sizeof sliver);
        memset(&obj, 0, sizeof obj);
        cap = MakeCapture(&band);
        cap.canvas_width = 256;
        cap.canvas_extra = 0;
        band.main_enable = band.regs[58] = 0x11;
        Put16(band.regs, 30, 256);
        Put16(band.regs, 36, 256);
        memset(s_vram, 0, sizeof s_vram);
        s_vram[0] = 1u;
        for (unsigned row = 0; row < 8u; row++)
            s_vram[64u + row * 8u] = 3u << 8u;
        s_vram[2u * 16u] = 0x0080u;
        sliver.tile = 2;
        sliver.palette_base = 128;
        obj.slivers = &sliver;
        obj.count = 1;
        passed &= Expect(snesrecomp_ppu_mode7_compile_lines(
                             &cap, lines,
                             sizeof lines / sizeof lines[0]),
                         "OBJ reference compile");
        passed &= Expect(snesrecomp_ppu_mode7_render_reference(
                             &cap, lines, 224, &obj, 1,
                             s_reference_a, 256) &&
                             s_reference_a[0] == 3u,
                         "OBJ priority zero behind BG1");
        sliver.priority = 1;
        passed &= Expect(snesrecomp_ppu_mode7_render_reference(
                             &cap, lines, 224, &obj, 1,
                             s_reference_a, 256) &&
                             s_reference_a[0] == 129u,
                         "OBJ priority one above BG1");
        s_vram[2u * 16u] = 0u;
        passed &= Expect(snesrecomp_ppu_mode7_render_reference(
                             &cap, lines, 224, &obj, 1,
                             s_reference_a, 256) &&
                             s_reference_a[0] == 3u,
                         "transparent OBJ index preserves BG1");
        s_vram[64u + 8u] = 0u;
        passed &= Expect(snesrecomp_ppu_mode7_render_reference(
                             &cap, lines, 224, &obj, 1,
                             s_reference_a, 256) &&
                             s_reference_a[0] == 0u,
                         "palette index zero selects backdrop");
    }

    /* OBJ uses the same native 342-pixel plane, including the first widened
     * column, and preserves its priority decision there. */
    {
        SnesRecompObjSliver sliver;
        SnesRecompObjFrame obj;
        memset(&sliver, 0, sizeof sliver);
        memset(&obj, 0, sizeof obj);
        cap = MakeCapture(&band);
        band.main_enable = band.regs[58] = 0x10;
        band.bg[0].margin_left = 43;
        band.bg[0].margin_right = 43;
        memset(s_vram, 0, sizeof s_vram);
        s_vram[2u * 16u] = 0x0080u;
        sliver.screen_x = -43;
        sliver.tile = 2;
        sliver.palette_base = 128;
        sliver.priority = 1;
        obj.slivers = &sliver;
        obj.count = 1;
        passed &= Expect(snesrecomp_ppu_mode7_compile_lines(
                             &cap, lines,
                             sizeof lines / sizeof lines[0]) &&
                             snesrecomp_ppu_mode7_render_reference(
                                 &cap, lines, 224, &obj, 1,
                                 s_reference_a, 342) &&
                             s_reference_a[0] == 129u,
                         "OBJ renders in widened left margin");
    }

    /* Widened pixels use the same authentic Mode 7 wrap as the native view. */
    cap = MakeCapture(&band);
    band.main_enable = band.regs[58] = 1;
    band.bg[0].margin_left = 43;
    band.bg[0].margin_right = 43;
    for (unsigned i = 0; i < SNESRECOMP_MODE7_TEXTURE_TEXELS; i++)
        s_vram[i] = 1u;
    for (unsigned i = 0; i < 64u; i++)
        s_vram[64u + i] = (uint16_t)(1u | (9u << 8u));
    band.regs[12] = 0;
    passed &= Expect(snesrecomp_ppu_mode7_compile_lines(
                         &cap, lines, sizeof lines / sizeof lines[0]) &&
                         snesrecomp_ppu_mode7_render_reference(
                             &cap, lines, 224, NULL, 1,
                             s_reference_a, 342) &&
                         s_reference_a[0] == 9u,
                     "repeat mode wraps the widened left margin");
    band.regs[12] = 0x80;
    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_UNSUPPORTED_MAP_SIZE,
                     "large field fails closed");
    band.regs[12] = 0;
    band.main_enable = band.regs[58] = 0x11;
    band.mosaic = band.regs[5] = 0x11;
    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_UNSUPPORTED_MOSAIC,
                     "effective mosaic fails closed");
    band.mosaic = band.regs[5] = 0;
    band.sub_enable = band.regs[59] = 0x10;
    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_SUPPORTED,
                     "dormant OBJ-only subscreen is accepted");
    band.main_enable = band.regs[58] = 1;
    band.main_window_enable = band.regs[60] = 0x10;
    band.sub_window_enable = band.regs[61] = 0x10;
    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_SUPPORTED,
                     "dormant OBJ windows are accepted");
    band.main_window_enable = band.regs[60] = 1;
    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_UNSUPPORTED_WINDOWS,
                     "drawable BG1 window fails closed");
    band.main_window_enable = band.regs[60] = 0;
    band.sub_window_enable = band.regs[61] = 0;
    band.main_enable = band.regs[58] = 0x11;
    band.cgwsel = band.regs[63] = 2;
    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_UNSUPPORTED_COLOUR_MATH,
                     "drawable OBJ subscreen colour math fails closed");
    band.cgwsel = band.regs[63] = 0;
    band.sub_enable = band.regs[59] = 1;
    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_UNSUPPORTED_SUBSCREEN,
                     "drawable BG1 subscreen fails closed");
    band.sub_enable = band.regs[59] = 0;
    cap.oam = NULL;
    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_UNSUPPORTED_OBJ,
                     "missing OBJ authority fails closed");

    return passed ? 0 : 1;
}
