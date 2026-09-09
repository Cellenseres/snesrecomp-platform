#ifndef SNESRECOMP_PLATFORM_SNES_PPU_SEMANTIC_GPU_H
#define SNESRECOMP_PLATFORM_SNES_PPU_SEMANTIC_GPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "snesrecomp_platform/snes_ppu_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SNESRECOMP_SEMANTIC_BG1_MAIN = 1u << 0,
    SNESRECOMP_SEMANTIC_BG2_MAIN = 1u << 1,
    SNESRECOMP_SEMANTIC_BG3_MAIN = 1u << 2,
    SNESRECOMP_SEMANTIC_BG1_SUB = 1u << 3,
    SNESRECOMP_SEMANTIC_BG2_SUB = 1u << 4,
    SNESRECOMP_SEMANTIC_BG3_SUB = 1u << 5,
    SNESRECOMP_SEMANTIC_MAIN_RGB = 1u << 6,
    SNESRECOMP_SEMANTIC_MATH = 1u << 7,

    SNESRECOMP_SEMANTIC_SOURCE_BG1 = 0,
    SNESRECOMP_SEMANTIC_SOURCE_BG2 = 1,
    SNESRECOMP_SEMANTIC_SOURCE_BG3 = 2,
    /* The CPU renderer splits OBJ into two layer ids rather than carrying a
     * flag: 4 for palettes 4..7, which CGADSUB bit 4 can reach, and 6 for
     * palettes 0..3, which no CGADSUB bit can, because the enable mask is only
     * six bits wide. Keeping both ids preserves that with no extra state. */
    SNESRECOMP_SEMANTIC_SOURCE_OBJ = 4,
    SNESRECOMP_SEMANTIC_SOURCE_BACKDROP = 5,
    SNESRECOMP_SEMANTIC_SOURCE_OBJ_NO_MATH = 6,
    SNESRECOMP_SEMANTIC_META_ZERO = 8,

    /* The TASK-04 mask byte is full, so OBJ permissions travel in a second
     * byte of their own rather than displacing an established bit. */
    SNESRECOMP_SEMANTIC_OBJ_MAIN = 1u << 0,
    SNESRECOMP_SEMANTIC_OBJ_SUB = 1u << 1,
};

/* Eight bytes become two adjacent RGBA8 texels in the GXM state texture. */
typedef struct SnesRecompSemanticLineState {
    uint8_t cgadsub;
    uint8_t cgwsel;
    uint8_t fixed_r5;
    uint8_t fixed_g5;
    uint8_t fixed_b5;
    uint8_t brightness;
    uint8_t forced_blank;
    uint8_t bg3_priority;
} SnesRecompSemanticLineState;

uint8_t snesrecomp_ppu_semantic_meta(unsigned source, bool palette_zero);
unsigned snesrecomp_ppu_semantic_source(uint8_t metadata);
bool snesrecomp_ppu_semantic_palette_zero(uint8_t metadata);

/* Compiles the exact per-pixel layer/color-window decisions and per-line
 * compositor state from the authoritative capture. `mask_pitch` is bytes.
 * `obj_mask` may be NULL for a frame with no sprites; when given it receives
 * the OBJ Main/Sub permissions at the same pitch. */
bool snesrecomp_ppu_compile_semantic_input(
    const SnesPpuFrameCapture *cap, uint8_t *mask, uint8_t *obj_mask,
    size_t mask_pitch, SnesRecompSemanticLineState *lines,
    unsigned line_capacity);

bool snesrecomp_ppu_expand_semantic_scroll(
    const SnesPpuFrameCapture *cap,
    uint16_t *bg1_x, uint16_t *bg1_y,
    uint16_t *bg2_x, uint16_t *bg2_y,
    uint16_t *bg3_x, uint16_t *bg3_y, unsigned capacity);

/* CGRAM BGR555 -> byte-exact R5,G5,B5,A. No brightness is applied. */
void snesrecomp_ppu_build_rgb5_palette(uint8_t *dst_rgba,
                                       const uint16_t *cgram);

#ifdef __cplusplus
}
#endif

#endif
