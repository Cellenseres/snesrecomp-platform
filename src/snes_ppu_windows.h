#ifndef SNESRECOMP_PPU_WINDOWS_H
#define SNESRECOMP_PPU_WINDOWS_H
#include "snesrecomp_platform/snes_ppu_capture.h"
#include <string.h>
static inline bool snesrecomp_ppu_resolved_windows_equal(const SnesPpuFrameCapture *cap,unsigned a,unsigned b) {
    return !cap->resolved_windows || !memcmp(cap->resolved_windows+a*6u,
        cap->resolved_windows+b*6u,6u*sizeof(*cap->resolved_windows));
}

/* All band inputs used by the shared window-span compiler. Matrices and
 * scroll do not change these decisions. Layout/canvas identity is checked
 * separately by the owning planner. */
static inline bool snesrecomp_ppu_windows_equal(const SnesPpuRasterBand *a,
                                               const SnesPpuRasterBand *b) {
    if (a->main_enable != b->main_enable || a->sub_enable != b->sub_enable ||
        a->main_window_enable != b->main_window_enable ||
        a->sub_window_enable != b->sub_window_enable ||
        ((a->cgwsel ^ b->cgwsel) & 0xf0u) || a->window_sel != b->window_sel ||
        a->window1_left != b->window1_left || a->window1_right != b->window1_right ||
        a->window2_left != b->window2_left || a->window2_right != b->window2_right ||
        a->regs[56] != b->regs[56] || a->regs[57] != b->regs[57]) return false;
    for (unsigned bg = 0; bg < 3; ++bg)
        if (a->bg[bg].margin_left != b->bg[bg].margin_left ||
            a->bg[bg].margin_right != b->bg[bg].margin_right) return false;
    return true;
}
#endif
