#ifndef SNESRECOMP_PLATFORM_SNES_PPU_CAPTURE_H
#define SNESRECOMP_PLATFORM_SNES_PPU_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What a renderer needs to draw one SNES frame, in SNES terms. Nothing here
 * names a backend, a texture or a console SDK: the CPU reference renderer and
 * a native GPU backend both consume it.
 *
 * HDMA can rewrite scroll, brightness, windows and mode-7 state between any
 * two scanlines, so a frame-end snapshot would describe a frame the hardware
 * never drew. The capture is therefore a list of raster bands, each covering a
 * run of scanlines over which every field below is constant. A band is the
 * merge of per-scanline state, not an approximation: a backend may treat its
 * lines as identical without checking further.
 *
 * Fields not yet filled are present from the start so a backend can reject a
 * frame using state it does not draw.
 */

enum {
    SNES_PPU_BG_COUNT = 4,
    SNES_PPU_VRAM_WORDS = 0x8000,
    SNES_PPU_CGRAM_ENTRIES = 256,
    SNES_PPU_MAX_BANDS = 240,

    /* OAM as the hardware splits it: 128 sprites of four bytes in the low
     * table, then two bits per sprite -- size select and the ninth X bit --
     * packed into the 32-byte high table. Both halves are needed; the high
     * table alone decides whether a sprite is small or large and whether its
     * X is negative, so a capture holding only the low table would describe a
     * different frame. */
    SNES_PPU_OAM_WORDS = 256,
    SNES_PPU_HIGH_OAM_BYTES = 32,
    SNES_PPU_OAM_SLOTS = 128,

    /* The PPU register file as the emulator snapshots it, starting at INIDISP.
     * A renderer that replays a band needs these bytes verbatim: the decoded
     * fields below are what a backend and the support predicate read, but the
     * software PPU is driven by the registers themselves, and reconstructing
     * them from decoded values would be a second, divergent decoder. */
    SNES_PPU_REG_BLOCK_BYTES = 0x40,
};

typedef struct SnesPpuBgState {
    uint16_t tilemap_word_addr;   /* BGnSC base, in VRAM words */
    uint16_t char_word_addr;      /* BGnNBA base, in VRAM words */
    uint16_t h_scroll;
    uint16_t v_scroll;
    bool wide;                    /* tilemap is 64 tiles across */
    bool tall;                    /* tilemap is 64 tiles down */
    bool big_tiles;               /* 16x16 characters */
    uint8_t bpp;                  /* 2, 4 or 8 */

    /* Widescreen margins for this layer, in pixels either side of the native
     * 256-pixel window. Zero is normal: the policy grants margins per layer,
     * and a layer excluded from it simply draws 256 columns. */
    uint8_t margin_left;
    uint8_t margin_right;

    /* The margin is filled by cyclically copying the native scanline rather
     * than by widening the layer's own fetch. */
    bool margin_repeats;
} SnesPpuBgState;

typedef struct SnesPpuRasterBand {
    uint16_t y_begin;             /* first visible line, inclusive */
    uint16_t y_end;               /* last visible line, exclusive */

    uint8_t bg_mode;              /* 0..7 */
    bool bg3_priority;            /* $2105 bit 3 */
    SnesPpuBgState bg[SNES_PPU_BG_COUNT];

    uint8_t main_enable;          /* TM  $212C */
    uint8_t sub_enable;           /* TS  $212D */
    uint8_t main_window_enable;   /* TMW $212E */
    uint8_t sub_window_enable;    /* TSW $212F */

    uint8_t brightness;           /* 0..15 */
    bool forced_blank;

    /* Not consumed in phase 1, and that is exactly why they are captured: the
     * support predicate rejects a frame whose correctness would depend on
     * them. */
    uint8_t cgwsel;               /* $2130 */
    uint8_t cgadsub;              /* $2131 */
    uint16_t fixed_colour;        /* $2132, assembled BGR555 */
    uint8_t mosaic;               /* $2106 */
    uint32_t window_sel;          /* $2123..$2125 packed */
    uint8_t window1_left, window1_right;
    uint8_t window2_left, window2_right;

    /* The raw register block these fields were decoded from. Carried so the
     * software renderer can be driven from a trace exactly as it is driven
     * from a live frame. */
    uint8_t regs[SNES_PPU_REG_BLOCK_BYTES];
} SnesPpuRasterBand;

/* Widescreen policy, which is renderer state rather than SNES state: it is
 * published once a frame by the host and does not appear in the register file.
 * Without it a replay would silently fall back to whatever the running process
 * happened to have set, which is the one thing a self-contained trace must not
 * do. */
typedef struct SnesPpuLayoutState {
    uint8_t extra_left_right;     /* centering budget */
    uint8_t extra_left_cur;       /* margin columns actually drawn */
    uint8_t extra_right_cur;
    uint8_t layer_widen_mask;
    uint8_t layer_clamp;
    uint8_t layer_mirror;
    uint8_t layer_repeat;
    uint8_t bg3_widen_y;

    /* Which layers receive expanded window coordinates and which hardware
     * windows expand with them. These live in host PPU state, not $21xx. */
    uint8_t window_expand_layers;
    uint8_t window_expand_windows;

    /* Which software renderer the host selected. It is a host choice rather
     * than SNES state, but it decides which of two implementations draws the
     * frame, so a replay that guessed it could differ from the live frame for
     * a reason nothing in the SNES state explains. */
    uint32_t render_flags;

    /* Host OBJ policy.
     *
     * Sprite evaluation is the one place where the widescreen host reaches
     * into hardware semantics: a HUD OAM range can be shifted outward, the
     * ambiguous 9-bit X margin bands can be decoded strictly per slot, and a
     * split HUD can claim the full centering budget. None of it lives in
     * $21xx, all of it changes which pixels a sprite covers, and the hint
     * tables and the temporal classifier behind them are far larger than a
     * frame trace should carry.
     *
     * They are captured as the bytes that *arm* each policy rather than as
     * the policies themselves. The exact portable subset requires them to be
     * zero; a game that arms one falls back rather than replaying against host
     * state a trace never held. */
    uint8_t hud_split_height;     /* wsHudSplitHeight, incl. full-budget bit */
    uint8_t hud_left_end;
    uint8_t hud_right_start;
    uint8_t hud_oam_first_slot;
    uint8_t hud_oam_slots;
    uint8_t hud_oam_height;
    uint8_t hud_oam_first_slot2;
    uint8_t hud_oam_slots2;
    uint8_t oam_left_hint_strict;
    uint8_t oam_right_hint_strict;
} SnesPpuLayoutState;

typedef struct SnesPpuFrameCapture {
    /* Borrowed, not owned. Valid until the next capture. */
    const uint16_t *vram;         /* SNES_PPU_VRAM_WORDS */
    const uint16_t *cgram;        /* SNES_PPU_CGRAM_ENTRIES */

    /* OAM, or NULL when this capture carries no sprite authority at all.
     * NULL is not "no sprites": it is "this capture cannot say", and a frame
     * with OBJ enabled is refused rather than drawn from whatever OAM the
     * running process happens to hold. Traces written before OAM was captured
     * read back as NULL for exactly that reason. */
    const uint16_t *oam;          /* SNES_PPU_OAM_WORDS */
    const uint8_t *high_oam;      /* SNES_PPU_HIGH_OAM_BYTES */

    const SnesPpuRasterBand *bands;
    unsigned band_count;

    /* Geometry. `canvas_width` is the framebuffer the frame is composed into,
     * which stays constant for the session; `canvas_extra` is where the native
     * window starts inside it, so canvas column c is SNES screen x
     * `c - canvas_extra`. A 4:3 frame is a canvas whose margins nobody
     * writes. */
    uint16_t native_width;        /* 256 */
    uint16_t canvas_width;
    uint16_t canvas_extra;
    uint16_t visible_height;      /* 224 */

    /* Non-zero when VRAM or CGRAM changed since the previous capture, so a
     * backend can skip re-decoding what it already holds. Zero means "nothing
     * moved"; it never means "unknown". */
    uint32_t vram_dirty_words;
    uint32_t cgram_dirty_entries;

    /* Raster-time writes through PPU memory ports cannot be reconstructed
     * from the one frame-global VRAM/CGRAM snapshot carried here. These bits
     * describe that history independently of ordinary between-frame dirtiness.
     * Trace v2's band payload predates the field; the trace header's reserved
     * flag bits carry it without changing band bytes or frozen hashes. */
    uint32_t raster_memory_flags;

    SnesPpuLayoutState layout;
} SnesPpuFrameCapture;

enum {
    SNES_PPU_RASTER_MEMORY_VRAM = 1u << 0,
    SNES_PPU_RASTER_MEMORY_CGRAM = 1u << 1,
    SNES_PPU_RASTER_MEMORY_OAM = 1u << 2,
    SNES_PPU_RASTER_MEMORY_UNKNOWN = 1u << 3,
};

/* Why a backend declined a frame. A backend must name one of these rather than
 * draw a frame with an effect left out -- a missing effect is a wrong frame,
 * and a wrong frame that raises the GPU counter is worse than a fallback. */
typedef enum SnesPpuUnsupported {
    SNES_PPU_SUPPORTED = 0,
    SNES_PPU_UNSUPPORTED_MODE,            /* not a BG mode this backend draws */
    SNES_PPU_UNSUPPORTED_BG3,             /* BG3 enabled */
    SNES_PPU_UNSUPPORTED_BG4,
    SNES_PPU_UNSUPPORTED_OBJ,             /* sprites enabled */
    SNES_PPU_UNSUPPORTED_SUBSCREEN,       /* subscreen in use */
    SNES_PPU_UNSUPPORTED_COLOUR_MATH,
    SNES_PPU_UNSUPPORTED_WINDOWS,
    SNES_PPU_UNSUPPORTED_MOSAIC,
    SNES_PPU_UNSUPPORTED_OFFSET_PER_TILE,
    SNES_PPU_UNSUPPORTED_BIG_TILES,
    SNES_PPU_UNSUPPORTED_MAP_SIZE,
    SNES_PPU_UNSUPPORTED_RASTER_STATE,    /* state varies in a way it cannot follow */
    SNES_PPU_UNSUPPORTED_LAYOUT_POLICY,   /* widescreen semantics it cannot honour */
    SNES_PPU_UNSUPPORTED_RASTER_MEMORY,   /* missing raster-time memory history */
    SNES_PPU_UNSUPPORTED_BACKEND,         /* backend not available at all */
} SnesPpuUnsupported;

const char *snes_ppu_unsupported_text(SnesPpuUnsupported reason);

/* Can a backend that draws Mode 1 BG1 and BG2, and nothing else, produce this
 * entire frame exactly? Returns the first effect it would otherwise have had
 * to leave out.
 *
 * Portable on purpose: accepting a frame the renderer cannot draw is the most
 * expensive mistake here, so the decision is unit-testable without hardware. */
SnesPpuUnsupported snesrecomp_ppu_phase1_supports(
    const SnesPpuFrameCapture *cap);

/* Semantic eligibility and backend presence are deliberately separate. A
 * desktop build can validate that a trace is in the supported subset while
 * still falling back because it has no native semantic PPU renderer. */
SnesPpuUnsupported snesrecomp_ppu_phase1_backend_supports(
    const SnesPpuFrameCapture *cap, bool backend_available);

/* Expands authoritative raw bands into renderer-friendly line state. Vertical
 * scroll includes the output scanline, matching the software PPU's source-Y
 * convention. No live PPU state is consulted. */
bool snesrecomp_ppu_phase1_expand_lines(
    const SnesPpuFrameCapture *cap,
    uint16_t *bg1_scroll_x, uint16_t *bg1_scroll_y,
    uint16_t *bg2_scroll_x, uint16_t *bg2_scroll_y,
    uint8_t *brightness, uint8_t *forced_blank, unsigned capacity);

#ifdef __cplusplus
}
#endif

#endif
