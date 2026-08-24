#ifndef SNESRECOMP_PLATFORM_PRESENTER_INTERNAL_H
#define SNESRECOMP_PLATFORM_PRESENTER_INTERNAL_H

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
} SnesRecompPresenterOps;

struct SnesRecompPresenter {
    const SnesRecompPresenterOps *ops;
    void *context;
    SnesRecompPresentBackend backend;
    uint32_t capabilities;
    int frame_width;
    int frame_height;
    char backend_name[64];
    char last_error[256];
};

void snesrecomp_presenter_set_error(
    SnesRecompPresenter *presenter,
    const char *format,
    ...);

bool snesrecomp_presenter_sdl_create(
    SnesRecompPresenter *presenter,
    const SnesRecompPresentConfig *config);

#if SNESRECOMP_PLATFORM_HAS_OPENGL
bool snesrecomp_presenter_opengl_create(
    SnesRecompPresenter *presenter,
    const SnesRecompPresentConfig *config);
#endif

#endif
