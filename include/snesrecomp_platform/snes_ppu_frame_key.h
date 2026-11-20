#ifndef SNESRECOMP_PLATFORM_SNES_PPU_FRAME_KEY_H
#define SNESRECOMP_PLATFORM_SNES_PPU_FRAME_KEY_H
#include "snes_ppu_frame.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Exact CPU-side preparation key for owned qualified-pixel frames. No frame
 * pointer is retained. Memory versions are owner-scoped; raster/layout/OAM/
 * window/map inputs are compared separately. Optional sliver lists are not
 * admitted by this key. A hit never certifies GPU uploads or guest work. */
typedef struct SnesRecompPpuFrameKey SnesRecompPpuFrameKey;
SnesRecompPpuFrameKey *snesrecomp_ppu_frame_key_create(void);
void snesrecomp_ppu_frame_key_destroy(SnesRecompPpuFrameKey *key);
void snesrecomp_ppu_frame_key_clear(SnesRecompPpuFrameKey *key);
bool snesrecomp_ppu_frame_key_matches(const SnesRecompPpuFrameKey *key,
    const SnesRecompPpuFrame *frame);
/* Background-only certificate: excludes OAM contents and qualified OBJ pixels.
 * A consumer must certify every OBJ row its cached result actually consumes;
 * this weaker check never authorizes complete-image reuse by itself. */
bool snesrecomp_ppu_frame_key_matches_background(const SnesRecompPpuFrameKey *key,
    const SnesRecompPpuFrame *frame);
/* Diagnostics: bit 0 versions, 1 shape, 2 bands, 3 windows, 4 OAM, 8+n map n.
 * Zero means the whole key matches. */
unsigned snesrecomp_ppu_frame_key_mismatch(const SnesRecompPpuFrameKey *key,
    const SnesRecompPpuFrame *frame);
/* Call only after successful preparation. Invalid input clears the key. */
bool snesrecomp_ppu_frame_key_commit(SnesRecompPpuFrameKey *key,
    const SnesRecompPpuFrame *frame);
#ifdef __cplusplus
}
#endif
#endif
