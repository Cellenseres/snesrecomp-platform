#include "snesrecomp_platform/presenter.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "presenter_internal.h"

static void write_error(
    char *error,
    size_t error_size,
    const char *message) {
    if (!error || error_size == 0)
        return;
    snprintf(error, error_size, "%s", message ? message : "unknown error");
}

void snesrecomp_presenter_set_error(
    SnesRecompPresenter *presenter,
    const char *format,
    ...) {
    if (!presenter)
        return;

    va_list args;
    va_start(args, format);
    vsnprintf(
        presenter->last_error,
        sizeof(presenter->last_error),
        format,
        args);
    va_end(args);
}

static bool validate_config(
    const SnesRecompPresentConfig *config,
    char *error,
    size_t error_size) {
    if (!config) {
        write_error(error, error_size, "presenter config is null");
        return false;
    }
    if (!config->window_title || !config->window_title[0]) {
        write_error(error, error_size, "window title is empty");
        return false;
    }
    if (config->frame_width <= 0 || config->frame_height <= 0) {
        write_error(error, error_size, "frame dimensions must be positive");
        return false;
    }
    if (config->window_scale <= 0) {
        write_error(error, error_size, "window scale must be positive");
        return false;
    }
    if (config->pixel_format != SNESRECOMP_PIXEL_FORMAT_ARGB8888) {
        write_error(error, error_size, "unsupported initial pixel format");
        return false;
    }
    return true;
}

bool snesrecomp_presenter_create(
    const SnesRecompPresentConfig *config,
    SnesRecompPresenter **out_presenter,
    char *error,
    size_t error_size) {
    if (out_presenter)
        *out_presenter = NULL;
    if (!out_presenter) {
        write_error(error, error_size, "presenter output pointer is null");
        return false;
    }
    if (!validate_config(config, error, error_size))
        return false;

    SnesRecompPresenter *presenter =
        (SnesRecompPresenter *)calloc(1, sizeof(*presenter));
    if (!presenter) {
        write_error(error, error_size, "out of memory creating presenter");
        return false;
    }

    presenter->frame_width = config->frame_width;
    presenter->frame_height = config->frame_height;
    if (config->preserve_aspect &&
        config->pixel_aspect_numerator > 0 &&
        config->pixel_aspect_denominator > 0) {
        presenter->pixel_aspect_numerator =
            config->pixel_aspect_numerator;
        presenter->pixel_aspect_denominator =
            config->pixel_aspect_denominator;
    } else {
        presenter->pixel_aspect_numerator = 1;
        presenter->pixel_aspect_denominator = 1;
    }
    presenter->vsync_state = SNESRECOMP_VSYNC_UNKNOWN;

    bool created = false;
    switch (config->backend) {
    case SNESRECOMP_PRESENT_BACKEND_AUTO:
    case SNESRECOMP_PRESENT_BACKEND_SDL:
    case SNESRECOMP_PRESENT_BACKEND_SDL_SOFTWARE:
        created = snesrecomp_presenter_sdl_create(presenter, config);
        break;
    case SNESRECOMP_PRESENT_BACKEND_OPENGL:
#if SNESRECOMP_PLATFORM_HAS_OPENGL
        created = snesrecomp_presenter_opengl_create(presenter, config);
#else
        snesrecomp_presenter_set_error(
            presenter, "OpenGL presenter is not compiled in this build");
#endif
        break;
    case SNESRECOMP_PRESENT_BACKEND_SDL_GPU:
        snesrecomp_presenter_set_error(
            presenter, "SDL GPU presenter is not compiled yet");
        break;
    default:
        snesrecomp_presenter_set_error(
            presenter, "unknown presentation backend");
        break;
    }

    if (!created) {
        write_error(error, error_size, presenter->last_error);
        if (presenter->ops && presenter->ops->destroy)
            presenter->ops->destroy(presenter);
        free(presenter);
        return false;
    }

    write_error(error, error_size, "");
    *out_presenter = presenter;
    return true;
}

int snesrecomp_presenter_display_width(
    const SnesRecompPresenter *presenter,
    int frame_width) {
    if (!presenter || frame_width <= 0)
        return frame_width;
    const int64_t scaled =
        (int64_t)frame_width * presenter->pixel_aspect_numerator;
    const int64_t width =
        (scaled + presenter->pixel_aspect_denominator / 2) /
        presenter->pixel_aspect_denominator;
    if (width <= 0)
        return 1;
    return width > INT_MAX ? INT_MAX : (int)width;
}

void snesrecomp_presenter_destroy(SnesRecompPresenter *presenter) {
    if (!presenter)
        return;
    if (presenter->ops && presenter->ops->destroy)
        presenter->ops->destroy(presenter);
    free(presenter);
}

bool snesrecomp_presenter_present(
    SnesRecompPresenter *presenter,
    const SnesRecompVideoFrame *frame) {
    if (!presenter || !presenter->ops || !presenter->ops->present)
        return false;
    return presenter->ops->present(presenter, frame);
}

bool snesrecomp_presenter_set_fullscreen(
    SnesRecompPresenter *presenter,
    bool fullscreen) {
    if (!presenter || !presenter->ops || !presenter->ops->set_fullscreen)
        return false;
    return presenter->ops->set_fullscreen(presenter, fullscreen);
}

bool snesrecomp_presenter_set_window_scale(
    SnesRecompPresenter *presenter,
    int scale) {
    if (!presenter || !presenter->ops || !presenter->ops->set_window_scale)
        return false;
    return presenter->ops->set_window_scale(presenter, scale);
}

bool snesrecomp_presenter_set_window_title(
    SnesRecompPresenter *presenter,
    const char *title) {
    if (!presenter || !presenter->ops || !presenter->ops->set_window_title)
        return false;
    return presenter->ops->set_window_title(presenter, title);
}

bool snesrecomp_presenter_get_drawable_size(
    SnesRecompPresenter *presenter,
    int *width,
    int *height) {
    if (!presenter || !presenter->ops || !presenter->ops->get_drawable_size)
        return false;
    return presenter->ops->get_drawable_size(presenter, width, height);
}

uint32_t snesrecomp_presenter_capabilities(
    const SnesRecompPresenter *presenter) {
    return presenter ? presenter->capabilities : 0;
}

SnesRecompPresentBackend snesrecomp_presenter_backend(
    const SnesRecompPresenter *presenter) {
    return presenter ? presenter->backend : SNESRECOMP_PRESENT_BACKEND_AUTO;
}

const char *snesrecomp_presenter_backend_name(
    const SnesRecompPresenter *presenter) {
    return presenter ? presenter->backend_name : "unavailable";
}

SnesRecompVSyncState snesrecomp_presenter_vsync_state(
    const SnesRecompPresenter *presenter) {
    return presenter ? presenter->vsync_state : SNESRECOMP_VSYNC_UNKNOWN;
}

const char *snesrecomp_vsync_state_name(SnesRecompVSyncState state) {
    switch (state) {
    case SNESRECOMP_VSYNC_DISABLED:
        return "disabled";
    case SNESRECOMP_VSYNC_ENABLED:
        return "enabled";
    case SNESRECOMP_VSYNC_UNSUPPORTED:
        return "unsupported";
    case SNESRECOMP_VSYNC_UNKNOWN:
    default:
        return "unknown";
    }
}

const char *snesrecomp_presenter_last_error(
    const SnesRecompPresenter *presenter) {
    return presenter ? presenter->last_error : "presenter is null";
}
