#ifndef SNESRECOMP_PLATFORM_SNES_BG_CONTINUITY_H
#define SNESRECOMP_PLATFORM_SNES_BG_CONTINUITY_H

#include <stdbool.h>
#include <stdint.h>

#include "snesrecomp_platform/snes_ppu_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { SNES_BG_SCROLL_MODULUS = 1024 };

typedef enum SnesBgDiscontinuity {
    SNES_BG_CONTINUOUS = 0,
    SNES_BG_DISCONTINUITY_GEOMETRY,
    SNES_BG_DISCONTINUITY_BANDS,
    SNES_BG_DISCONTINUITY_MODE,
    SNES_BG_DISCONTINUITY_BLANK,
    SNES_BG_DISCONTINUITY_ENABLE,
    SNES_BG_DISCONTINUITY_MOSAIC,
} SnesBgDiscontinuity;

typedef struct SnesBgBandDelta {
    int16_t dx[SNES_PPU_BG_COUNT];
    int16_t dy[SNES_PPU_BG_COUNT];
} SnesBgBandDelta;

typedef struct SnesBgInterpolation {
    SnesBgDiscontinuity verdict;
    /* Bit L set when BG(L+1) may move between these two frames. */
    uint8_t safe_layers;
    unsigned band_count;
    SnesBgBandDelta bands[SNES_PPU_MAX_BANDS];
} SnesBgInterpolation;

/* Shortest signed step between two scroll registers. */
int SnesBgScrollDelta(uint16_t from, uint16_t to);

/* Scroll at alpha, wrapped back into the register range. */
uint16_t SnesBgScrollAt(uint16_t from, int delta, float alpha);

/* Largest alpha that keeps the scroll inside the tile it starts in. */
float SnesBgTileBoundAlpha(uint16_t scroll, int delta);

/* Equal keys mean two frames can be compared band by band. */
uint64_t SnesBgContinuityKey(const SnesPpuFrameCapture *capture);

/* Per-layer deltas, and which layers a caller may move. */
void SnesBgCompare(const SnesPpuFrameCapture *prev,
                   const SnesPpuFrameCapture *cur,
                   unsigned max_step_px,
                   SnesBgInterpolation *out);

#ifdef __cplusplus
}
#endif

#endif
