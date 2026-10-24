#ifndef SNESRECOMP_PLATFORM_PRESENTER_BACKEND_H
#define SNESRECOMP_PLATFORM_PRESENTER_BACKEND_H

/* Backend service-provider interface. Applications use presenter.h; target
 * adapters implement one native presenter against this header. */

#include "snesrecomp_platform/presenter.h"

typedef struct SnesRecompPresenterOps {
    void (*destroy)(SnesRecompPresenter *presenter);
    bool (*present)(
        SnesRecompPresenter *presenter,
        const SnesRecompVideoFrame *frame);
    bool (*set_fullscreen)(
        SnesRecompPresenter *presenter,
        bool fullscreen);
    bool (*set_window_scale)(
        SnesRecompPresenter *presenter,
        int scale);
    bool (*set_window_title)(
        SnesRecompPresenter *presenter,
        const char *title);
    bool (*get_drawable_size)(
        SnesRecompPresenter *presenter,
        int *width,
        int *height);
    bool (*present_mode7_hd)(
        SnesRecompPresenter *presenter,
        const SnesRecompMode7HdFrame *frame);
    float (*get_display_scale)(SnesRecompPresenter *presenter);
    unsigned (*get_display_millihertz)(SnesRecompPresenter *presenter);
    /* Optional; appended to preserve existing backend initializers. */
    bool (*set_vsync)(SnesRecompPresenter *presenter, bool enabled);
    uint32_t (*ppu_capabilities)(const SnesRecompPresenter *presenter);
    SnesRecompPpuResult (*ppu_prepare)(SnesRecompPresenter *presenter,
                                      const SnesRecompPpuFrame *frame);
    SnesRecompPpuResult (*ppu_submit)(SnesRecompPresenter *presenter);
    void (*ppu_cancel)(SnesRecompPresenter *presenter);
    SnesRecompPpuResult (*ppu_prepare_job)(SnesRecompPresenter *presenter,
                                         const SnesRecompPpuJob *job);
    bool (*present_blits)(SnesRecompPresenter *presenter,
        const SnesRecompVideoFrame *base, const SnesRecompBlitPlan *plan);
} SnesRecompPresenterOps;

struct SnesRecompPresenter {
    const SnesRecompPresenterOps *ops;
    void *context;
    SnesRecompPresentBackend backend;
    uint32_t capabilities;
    int frame_width;
    int frame_height;
    int pixel_aspect_numerator;
    int pixel_aspect_denominator;
    SnesRecompVSyncState vsync_state;
    char backend_name[64];
    char last_error[256];
    /* Mask of 1u << scale. Appended so existing offsets do not move; zero
     * keeps the earlier 2x-only contract. */
    uint32_t mode7_scales;
    const SnesRecompPpuFrame *ppu_pending;
    SnesRecompPpuStats ppu_stats;
    const SnesRecompPpuJob *ppu_pending_job;
};

int snesrecomp_presenter_display_width(
    const SnesRecompPresenter *presenter,
    int frame_width);

void snesrecomp_presenter_set_error(
    SnesRecompPresenter *presenter,
    const char *format,
    ...);

bool snesrecomp_presenter_sdl_create(
    SnesRecompPresenter *presenter,
    const SnesRecompPresentConfig *config);

#if SNESRECOMP_PLATFORM_HAS_NATIVE_PRESENTER
bool snesrecomp_presenter_native_create(
    SnesRecompPresenter *presenter,
    const SnesRecompPresentConfig *config);
#endif

#if SNESRECOMP_PLATFORM_HAS_OPENGL
bool snesrecomp_presenter_opengl_create(
    SnesRecompPresenter *presenter,
    const SnesRecompPresentConfig *config);
#endif

#if SNESRECOMP_PLATFORM_HAS_VULKAN
bool snesrecomp_presenter_vulkan_create(
    SnesRecompPresenter *presenter,
    const SnesRecompPresentConfig *config);
#endif

#endif
