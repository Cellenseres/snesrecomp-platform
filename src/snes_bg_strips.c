#include "snesrecomp_platform/snes_bg_strips.h"

unsigned snesrecomp_bg_build_strips(SnesRecompBgStripVertex *verts,
                                    uint16_t *indices,
                                    const uint16_t *scroll_x,
                                    const uint16_t *scroll_y,
                                    unsigned lines,
                                    unsigned canvas_width,
                                    unsigned canvas_extra,
                                    unsigned margin_left,
                                    unsigned margin_right,
                                    float scale_x,
                                    float scale_y) {
    unsigned nv = 0;
    unsigned ni = 0;
    float x0, x1;

    if (!verts || !indices || !scroll_x || !scroll_y || !lines || !canvas_width)
        return 0;

    /* Map this layer's native window and margins into clip space. */
    {
        const float c0 = (float)canvas_extra - (float)margin_left;
        const float c1 = (float)canvas_extra + 256.0f + (float)margin_right;
        x0 = scale_x * (2.0f * (c0 / (float)canvas_width) - 1.0f);
        x1 = scale_x * (2.0f * (c1 / (float)canvas_width) - 1.0f);
    }

    for (unsigned line = 0; line < lines; line++) {
        /* Place strip edges on the scanline boundaries. */
        const float y0 =
            scale_y * (1.0f - 2.0f * ((float)line / (float)lines));
        const float y1 =
            scale_y * (1.0f - 2.0f * ((float)(line + 1u) / (float)lines));

        /* Negative world X is valid in the left margin. */
        const float wx0 = (float)scroll_x[line] - (float)margin_left;
        const float wx1 = (float)scroll_x[line] + 256.0f + (float)margin_right;
        /* Sample the source row at its centre. */
        const float wy = (float)scroll_y[line] + 0.5f;

        SnesRecompBgStripVertex *v = &verts[nv];
        v[0].x = x0; v[0].y = y0; v[0].wx = wx0; v[0].wy = wy;
        v[1].x = x1; v[1].y = y0; v[1].wx = wx1; v[1].wy = wy;
        v[2].x = x0; v[2].y = y1; v[2].wx = wx0; v[2].wy = wy;
        v[3].x = x1; v[3].y = y1; v[3].wx = wx1; v[3].wy = wy;

        indices[ni + 0] = (uint16_t)(nv + 0);
        indices[ni + 1] = (uint16_t)(nv + 1);
        indices[ni + 2] = (uint16_t)(nv + 2);
        indices[ni + 3] = (uint16_t)(nv + 2);
        indices[ni + 4] = (uint16_t)(nv + 1);
        indices[ni + 5] = (uint16_t)(nv + 3);

        nv += SNESRECOMP_BG_STRIP_VERTS;
        ni += SNESRECOMP_BG_STRIP_INDICES;
    }

    return nv;
}
