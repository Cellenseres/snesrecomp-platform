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
static uint32_t s_argb_reference[342 * 224];
static uint8_t s_full_world_map[512 * 512];

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

static uint8_t Expand5(unsigned component) {
    component = component > 31u ? 31u : component;
    return (uint8_t)((component << 3u) | (component >> 2u));
}

static uint32_t Argb5(unsigned r, unsigned g, unsigned b) {
    return UINT32_C(0xff000000) | ((uint32_t)Expand5(r) << 16u) |
           ((uint32_t)Expand5(g) << 8u) | Expand5(b);
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

/* Sub-register refinement. A band names its own step, a zero refinement
 * changes nothing, and origin and angle each carry a whole value: pinning a
 * remainder to a foreign whole number costs a full pixel or step at every
 * wrap. */

static SnesRecompMode7Line s_refined_a[224];
static SnesRecompMode7Line s_refined_b[224];

enum {
    REFINE_CENTER_X = 0x0642,
    REFINE_CENTER_Y = 0x0317
};

static void UnitCosSin(unsigned index, unsigned steps, int *cos_out,
                       int *sin_out) {
    const double turn = 6.283185307179586476925286766559;
    const double a = (double)(index % steps) * (turn / (double)steps);
    double c = 1.0, s = a, tc = 1.0, ts = a;
    unsigned n;

    for (n = 1; n <= 12u; n++) {
        tc *= -a * a / (double)((2u * n - 1u) * (2u * n));
        c += tc;
        ts *= -a * a / (double)((2u * n) * (2u * n + 1u));
        s += ts;
    }
    *cos_out = (int)(c * 256.0 + (c < 0.0 ? -0.5 : 0.5));
    *sin_out = (int)(s * 256.0 + (s < 0.0 ? -0.5 : 0.5));
}

/* A uniform scale times a rotation, scroll trailing the centre. */
static SnesPpuFrameCapture MakeRotationCapture(SnesPpuRasterBand *band,
                                               unsigned index, int scale) {
    SnesPpuFrameCapture cap = MakeCapture(band);
    int table_cos, table_sin;

    UnitCosSin(index, 256u, &table_cos, &table_sin);
    band->main_enable = band->regs[58] = 1;
    Put16(band->regs, 30, table_cos * scale / 256);
    Put16(band->regs, 32, table_sin * scale / 256);
    Put16(band->regs, 34, -table_sin * scale / 256);
    Put16(band->regs, 36, table_cos * scale / 256);
    Put16(band->regs, 38, REFINE_CENTER_X);
    Put16(band->regs, 40, REFINE_CENTER_Y);
    Put16(band->regs, 42, REFINE_CENTER_X - 0x80);
    Put16(band->regs, 44, REFINE_CENTER_Y - 0x70);
    return cap;
}

/* Signed difference inside the 18-bit Mode 7 plane. */
static int32_t PlaneDelta(uint32_t a, uint32_t b) {
    int32_t d = (int32_t)((a - b) & SNESRECOMP_MODE7_COORD_MASK);
    return (d & 0x20000) ? d - 0x40000 : d;
}

static SnesRecompMode7Refinement Refinement(uint32_t x, uint32_t y,
                                            int32_t delta) {
    SnesRecompMode7Refinement r;
    r.origin_x = x;
    r.origin_y = y;
    r.angle_delta = delta;
    return r;
}

static int RefinementCases(void) {
    static const int kScales[] = {64, 128, 256, 320, 512, 1024};
    const unsigned height = 224u;
    SnesPpuRasterBand band;
    SnesPpuFrameCapture cap;
    SnesRecompMode7Refinement refinement;
    unsigned index, scale, recovered, y, tick;
    int32_t cos_q30, sin_q30, previous;
    int passed = 1, wrong = 0, refused = 0, worst = 0;
    int64_t one = INT64_C(1) << 30;

    for (scale = 0; scale < sizeof kScales / sizeof kScales[0]; scale++) {
        for (index = 0; index < 256u; index++) {
            cap = MakeRotationCapture(&band, index, kScales[scale]);
            if (!snesrecomp_ppu_mode7_band_rotation_step(&band, 256u,
                                                         &recovered))
                refused++;
            else if (recovered != index)
                wrong++;
        }
    }
    passed &= Expect(wrong == 0, "every rotation step names itself");
    passed &= Expect(refused == 0, "no usable matrix refuses a step");

    cap = MakeRotationCapture(&band, 0u, 0);
    passed &= Expect(!snesrecomp_ppu_mode7_band_rotation_step(&band, 256u,
                                                              &recovered),
                     "a zero matrix names no step");

    snesrecomp_ppu_mode7_substep_rotation(0, &cos_q30, &sin_q30);
    passed &= Expect(cos_q30 == (int32_t)one && sin_q30 == 0,
                     "a zero delta is the identity rotation");
    previous = 0;
    wrong = 0;
    for (tick = 1u; tick < 512u; tick++) {
        int64_t norm;
        snesrecomp_ppu_mode7_substep_rotation((int32_t)tick, &cos_q30,
                                              &sin_q30);
        norm = (int64_t)cos_q30 * cos_q30 + (int64_t)sin_q30 * sin_q30;
        if (norm < one * one - (INT64_C(1) << 40) ||
            norm > one * one + (INT64_C(1) << 40) || sin_q30 <= previous)
            wrong++;
        previous = sin_q30;
    }
    passed &= Expect(wrong == 0,
                     "sub-step rotation keeps unit length and advances");

    /* Up to the multiplier truncation this path omits. */
    cap = MakeRotationCapture(&band, 37u, 320);
    refinement = Refinement((uint32_t)REFINE_CENTER_X * 256u,
                            (uint32_t)REFINE_CENTER_Y * 256u, 0);
    passed &= Expect(snesrecomp_ppu_mode7_compile_lines(&cap, s_refined_a,
                                                        height) &&
                         snesrecomp_ppu_mode7_compile_lines_refined(
                             &cap, s_refined_b, height, &refinement),
                     "both compilers accept the band");
    wrong = 0;
    for (y = 0; y < height; y++) {
        const int32_t dx = PlaneDelta(s_refined_b[y].start_x,
                                      s_refined_a[y].start_x);
        const int32_t dy = PlaneDelta(s_refined_b[y].start_y,
                                      s_refined_a[y].start_y);
        if (dx < 0 || dx > 189 || dy < 0 || dy > 189 ||
            s_refined_b[y].step_x != s_refined_a[y].step_x ||
            s_refined_b[y].step_y != s_refined_a[y].step_y)
            wrong++;
    }
    passed &= Expect(wrong == 0, "a zero refinement reproduces the compile");

    /* Walking the origin moves the plane by exactly that, wraps included. */
    cap = MakeRotationCapture(&band, 91u, 448);
    worst = 0;
    for (tick = 0; tick <= 1024u; tick++) {
        refinement = Refinement((uint32_t)REFINE_CENTER_X * 256u + tick,
                                (uint32_t)REFINE_CENTER_Y * 256u - tick, 0);
        if (!snesrecomp_ppu_mode7_compile_lines_refined(&cap, s_refined_b,
                                                        height, &refinement))
            break;
        if (tick) {
            const int32_t dx = PlaneDelta(s_refined_b[0].start_x,
                                          (uint32_t)previous);
            if (dx != 1)
                worst = 256;
        }
        previous = (int32_t)s_refined_b[0].start_x;
    }
    passed &= Expect(tick > 1024u && worst == 0,
                     "an origin walk moves the plane by what it walked");

    /* The delta may run past a step without snapping back. */
    worst = 0;
    for (tick = 0; tick <= 600u; tick++) {
        refinement = Refinement((uint32_t)REFINE_CENTER_X * 256u,
                                (uint32_t)REFINE_CENTER_Y * 256u,
                                (int32_t)tick);
        if (!snesrecomp_ppu_mode7_compile_lines_refined(&cap, s_refined_b,
                                                        height, &refinement))
            break;
        if (tick) {
            const int32_t d = PlaneDelta(s_refined_b[0].start_x,
                                         (uint32_t)previous);
            const int32_t m = d < 0 ? -d : d;
            if (m > worst)
                worst = m;
        }
        previous = (int32_t)s_refined_b[0].start_x;
    }
    passed &= Expect(tick > 600u && worst <= 16,
                     "an angle delta past a step does not snap back");

    passed &= Expect(!snesrecomp_ppu_mode7_compile_lines_refined(
                         &cap, s_refined_b, height, NULL),
                     "a missing refinement fails closed");
    return passed;
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

    passed &= RefinementCases();

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

    /* An external logical plane must not inherit the SNES ring's 1024-pixel
     * wrap. At source X=1024 the native view wraps to tile-map entry zero,
     * while the 4096-pixel plane reaches tile column 128. */
    {
        SnesRecompMode7MapSource full_source = {
            s_full_world_map, 512u, 512u,
        };
        memset(s_vram, 0, sizeof s_vram);
        memset(s_full_world_map, 1, sizeof s_full_world_map);
        s_full_world_map[128] = 2u;
        s_vram[0] = 1u;
        for (unsigned i = 0; i < 64u; i++) {
            s_vram[64u + i] = 11u << 8u;
            s_vram[128u + i] = 22u << 8u;
        }
        cap = MakeCapture(&band);
        cap.canvas_width = 256;
        cap.canvas_extra = 0;
        band.main_enable = band.regs[58] = 1;
        for (unsigned y = 0; y < 224u; y++) {
            lines[y].start_x = 1024u * 256u;
            lines[y].start_y = 0;
            lines[y].step_x = 0;
            lines[y].step_y = 0;
        }
        passed &= Expect(snesrecomp_ppu_mode7_map_source_valid(&full_source),
                         "512x512 logical Mode 7 source eligible");
        passed &= Expect(snesrecomp_ppu_mode7_render_reference(
                             &cap, lines, 224, NULL, 1,
                             s_reference_a, 256) &&
                         s_reference_a[0] == 11u,
                         "native source retains 1024-pixel wrap");
        for (unsigned scale = 1; scale <= 4; scale *= 2u) {
            passed &= Expect(snesrecomp_ppu_mode7_render_reference_with_map(
                                 &cap, lines, 224, NULL, &full_source, scale,
                                 s_reference_a, 256u * scale) &&
                             s_reference_a[0] == 22u,
                             scale == 1u
                                 ? "logical source beyond ring at 1x"
                                 : scale == 2u
                                       ? "logical source beyond ring at 2x"
                                       : "logical source beyond ring at 4x");
        }
        full_source.width_tiles = 4097u;
        passed &= Expect(!snesrecomp_ppu_mode7_map_source_valid(&full_source),
                         "oversized logical source fails closed");

        cap = MakeCapture(&band);
        band.main_enable = band.regs[58] = 1;
        band.bg[0].margin_left = 43;
        band.bg[0].margin_right = 43;
        for (unsigned i = 0; i < SNESRECOMP_MODE7_TEXTURE_TEXELS; i++)
            s_vram[i] = 0x0701u;
        passed &= Expect(snesrecomp_ppu_mode7_compile_lines(
                             &cap, lines, sizeof lines / sizeof lines[0]),
                         "restore widescreen state after logical source");
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
    cap = MakeCapture(&band);
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
                         SNES_PPU_SUPPORTED,
                     "drawable BG1 window is supported");
    band.window_sel = band.regs[48] = 0x02;
    band.window1_left = band.regs[52] = 40;
    band.window1_right = band.regs[53] = 80;
    passed &= Expect(snesrecomp_ppu_mode7_compile_lines(
                         &cap, lines, sizeof lines / sizeof lines[0]) &&
                         snesrecomp_ppu_mode7_render_reference(
                             &cap, lines, 224, NULL, 1,
                             s_reference_a, cap.canvas_width) &&
                         s_reference_a[39] == 9u &&
                         s_reference_a[40] == 0u &&
                         s_reference_a[80] == 0u &&
                         s_reference_a[81] == 9u,
                     "BG1 window masks its inclusive interval");
    band.window_sel = band.regs[48] = 0x03;
    passed &= Expect(snesrecomp_ppu_mode7_render_reference(
                         &cap, lines, 224, NULL, 1,
                         s_reference_a, cap.canvas_width) &&
                         s_reference_a[39] == 0u &&
                         s_reference_a[40] == 9u &&
                         s_reference_a[80] == 9u &&
                         s_reference_a[81] == 0u,
                     "inverted BG1 window preserves its interval");
    cap.canvas_width = 342;
    cap.canvas_extra = 43;
    cap.layout.extra_left_cur = 43;
    cap.layout.extra_right_cur = 43;
    cap.layout.window_expand_layers = 1;
    cap.layout.window_expand_windows = 1;
    band.bg[0].margin_left = 43;
    band.bg[0].margin_right = 43;
    band.window_sel = band.regs[48] = 0x02;
    band.window1_left = band.regs[52] = 0;
    band.window1_right = band.regs[53] = 255;
    passed &= Expect(snesrecomp_ppu_mode7_render_reference(
                         &cap, lines, 224, NULL, 1,
                         s_reference_a, cap.canvas_width) &&
                         s_reference_a[0] == 0u &&
                         s_reference_a[341] == 0u,
                     "pinned full-screen window expands through margins");
    /* Lufia II initializes W12SEL=$33, WH0=$08, WH1=$F7 and TMW=$1F.
     * With only BG1 in TM, its inverted window is the sole drawable mask.
     * Expanding by 43 preserves the authentic eight-pixel inset on both
     * sides while allowing the full-world source everywhere in between. */
    band.window_sel = 0x00333333u;
    band.regs[48] = band.regs[49] = band.regs[50] = 0x33;
    band.window1_left = band.regs[52] = 8;
    band.window1_right = band.regs[53] = 247;
    band.main_window_enable = band.regs[60] = 0x1f;
    cap.layout.window_expand_layers = 0x31;
    passed &= Expect(snesrecomp_ppu_mode7_render_reference(
                         &cap, lines, 224, NULL, 1,
                         s_reference_a, cap.canvas_width) &&
                         s_reference_a[7] == 0u &&
                         s_reference_a[8] == 9u &&
                         s_reference_a[333] == 9u &&
                         s_reference_a[334] == 0u,
                     "Lufia II intro BG1 window expands exactly");
    band.main_enable = band.regs[58] = 0x11;
    band.main_window_enable = band.regs[60] = 0x10;
    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_SUPPORTED,
                     "drawable OBJ window is supported");
    {
        SnesRecompObjSliver slivers[2];
        SnesRecompObjFrame obj;
        memset(slivers, 0, sizeof slivers);
        memset(&obj, 0, sizeof obj);
        s_vram[2u * 16u] = 0x0080u;
        slivers[0].screen_x = -43;
        slivers[1].screen_x = -35;
        for (unsigned i = 0; i < 2u; i++) {
            slivers[i].tile = 2;
            slivers[i].palette_base = 128;
            slivers[i].priority = 1;
        }
        obj.slivers = slivers;
        obj.count = 2;
        obj.capacity = 2;
        passed &= Expect(snesrecomp_ppu_mode7_render_reference(
                             &cap, lines, 224, &obj, 1,
                             s_reference_a, cap.canvas_width) &&
                             s_reference_a[0] == 9u &&
                             s_reference_a[8] == 129u,
                         "Lufia II OBJ window clips after OAM ordering");
    }
    band.main_window_enable = band.regs[60] = 0;
    band.window_sel = band.regs[48] = 0;
    band.regs[49] = band.regs[50] = 0;
    band.window1_left = band.regs[52] = 0;
    band.window1_right = band.regs[53] = 0;
    cap.canvas_width = 256;
    cap.canvas_extra = 0;
    memset(&cap.layout, 0, sizeof cap.layout);
    band.sub_window_enable = band.regs[61] = 0;
    /* Standard CGRAM colour math is part of the portable Mode 7 contract.
     * Verify fixed add, half subtract and the intro's BG1-main/OBJ-sub form
     * against the CPU renderer's byte-exact brightness map. */
    cap = MakeCapture(&band);
    cap.canvas_width = 256;
    cap.canvas_extra = 0;
    band.main_enable = band.regs[58] = 1;
    memset(s_vram, 0, sizeof s_vram);
    memset(s_cgram, 0, sizeof s_cgram);
    s_vram[0] = 1u;
    for (unsigned i = 0; i < 64u; i++)
        s_vram[64u + i] = 9u << 8u;
    s_cgram[9] = (uint16_t)(10u | (20u << 5u) | (30u << 10u));
    band.fixed_colour = (uint16_t)(4u | (8u << 5u) | (4u << 10u));
    band.cgadsub = band.regs[62] = 0x01;
    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_SUPPORTED &&
                     snesrecomp_ppu_mode7_compile_lines(
                         &cap, lines, sizeof lines / sizeof lines[0]) &&
                     snesrecomp_ppu_mode7_render_reference_argb8888_with_map(
                         &cap, lines, 224, NULL, NULL, 1,
                         (uint8_t *)s_argb_reference,
                         256u * sizeof(uint32_t)) &&
                     s_argb_reference[0] == Argb5(14u, 28u, 31u),
                     "fixed-colour addition");
    band.cgadsub = band.regs[62] = 0xc1;
    passed &= Expect(
        snesrecomp_ppu_mode7_render_reference_argb8888_with_map(
            &cap, lines, 224, NULL, NULL, 1,
            (uint8_t *)s_argb_reference,
            256u * sizeof(uint32_t)) &&
            s_argb_reference[0] == Argb5(3u, 6u, 13u),
        "half fixed-colour subtraction");
    {
        SnesRecompObjSliver sliver;
        SnesRecompObjFrame obj;
        memset(&sliver, 0, sizeof sliver);
        memset(&obj, 0, sizeof obj);
        s_vram[2u * 16u] = 0x0080u;
        s_cgram[129] = (uint16_t)(2u | (4u << 5u) | (6u << 10u));
        sliver.line = 0;
        sliver.screen_x = 0;
        sliver.tile = 2;
        sliver.palette_base = 128;
        sliver.priority = 1;
        obj.slivers = &sliver;
        obj.count = obj.capacity = 1;
        band.sub_enable = band.regs[59] = 0x10;
        band.cgwsel = band.regs[63] = 0x02;
        band.cgadsub = band.regs[62] = 0x41;
        passed &= Expect(
            snesrecomp_ppu_mode7_supports(&cap) == SNES_PPU_SUPPORTED &&
                snesrecomp_ppu_mode7_render_reference_argb8888_with_map(
                    &cap, lines, 224, &obj, NULL, 1,
                    (uint8_t *)s_argb_reference,
                    256u * sizeof(uint32_t)) &&
                s_argb_reference[0] == Argb5(6u, 12u, 18u),
            "BG1 main plus half OBJ subscreen");
    }
    band.sub_enable = band.regs[59] = 0;
    band.cgwsel = band.regs[63] = 1;
    band.cgadsub = band.regs[62] = 0;
    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_UNSUPPORTED_COLOUR_MATH,
                     "direct-colour Mode 7 still fails closed");
    band.cgwsel = band.regs[63] = 0;
    band.sub_enable = band.regs[59] = 1;
    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_UNSUPPORTED_SUBSCREEN,
                     "drawable BG1 subscreen fails closed");
    band.sub_enable = band.regs[59] = 0;
    band.main_enable = band.regs[58] = 0x11;
    cap.oam = NULL;
    passed &= Expect(snesrecomp_ppu_mode7_supports(&cap) ==
                         SNES_PPU_UNSUPPORTED_OBJ,
                     "missing OBJ authority fails closed");

    return passed ? 0 : 1;
}
