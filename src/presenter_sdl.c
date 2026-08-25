#include "presenter_internal.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SdlPresenterContext {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SnesRecompPixelFormat texture_format;
    int texture_width;
    int texture_height;
    bool preserve_aspect;
    bool linear_filtering;
} SdlPresenterContext;

static SDL_PixelFormat to_sdl_pixel_format(SnesRecompPixelFormat format) {
    switch (format) {
    case SNESRECOMP_PIXEL_FORMAT_ARGB8888:
        return SDL_PIXELFORMAT_ARGB8888;
    default:
        return SDL_PIXELFORMAT_UNKNOWN;
    }
}

static bool set_sdl_error(
    SnesRecompPresenter *presenter,
    const char *operation) {
    const char *detail = SDL_GetError();
    snesrecomp_presenter_set_error(
        presenter,
        "%s failed: %s",
        operation,
        detail && detail[0] ? detail : "unknown SDL error");
    return false;
}

static bool create_texture(
    SnesRecompPresenter *presenter,
    SnesRecompPixelFormat format,
    int width,
    int height) {
    SdlPresenterContext *context =
        (SdlPresenterContext *)presenter->context;
    const SDL_PixelFormat sdl_format = to_sdl_pixel_format(format);
    if (sdl_format == SDL_PIXELFORMAT_UNKNOWN) {
        snesrecomp_presenter_set_error(
            presenter, "unsupported SDL pixel format");
        return false;
    }

    SDL_Texture *texture = SDL_CreateTexture(
        context->renderer,
        sdl_format,
        SDL_TEXTUREACCESS_STREAMING,
        width,
        height);
    if (!texture)
        return set_sdl_error(presenter, "SDL_CreateTexture");

    if (!SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE)) {
        SDL_DestroyTexture(texture);
        return set_sdl_error(presenter, "SDL_SetTextureBlendMode");
    }
    if (!SDL_SetTextureScaleMode(
            texture,
            context->linear_filtering
                ? SDL_SCALEMODE_LINEAR
                : SDL_SCALEMODE_NEAREST)) {
        SDL_DestroyTexture(texture);
        return set_sdl_error(presenter, "SDL_SetTextureScaleMode");
    }

    if (context->preserve_aspect &&
        !SDL_SetRenderLogicalPresentation(
            context->renderer,
            snesrecomp_presenter_display_width(presenter, width),
            height,
            SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        SDL_DestroyTexture(texture);
        return set_sdl_error(
            presenter, "SDL_SetRenderLogicalPresentation");
    }

    if (context->texture)
        SDL_DestroyTexture(context->texture);
    context->texture = texture;
    context->texture_format = format;
    context->texture_width = width;
    context->texture_height = height;
    presenter->frame_width = width;
    presenter->frame_height = height;
    return true;
}

static void sdl_destroy(SnesRecompPresenter *presenter) {
    SdlPresenterContext *context =
        (SdlPresenterContext *)presenter->context;
    if (!context)
        return;

    if (context->texture)
        SDL_DestroyTexture(context->texture);
    if (context->renderer)
        SDL_DestroyRenderer(context->renderer);
    if (context->window)
        SDL_DestroyWindow(context->window);
    free(context);
    presenter->context = NULL;
}

static bool sdl_present(
    SnesRecompPresenter *presenter,
    const SnesRecompVideoFrame *frame) {
    SdlPresenterContext *context =
        (SdlPresenterContext *)presenter->context;
    if (!frame || !frame->pixels || frame->width <= 0 ||
        frame->height <= 0 || frame->pitch < frame->width * 4) {
        snesrecomp_presenter_set_error(
            presenter, "invalid video frame");
        return false;
    }

    if (!context->texture ||
        context->texture_format != frame->pixel_format ||
        context->texture_width != frame->width ||
        context->texture_height != frame->height) {
        if (!create_texture(
                presenter,
                frame->pixel_format,
                frame->width,
                frame->height)) {
            return false;
        }
    }

    if (!SDL_UpdateTexture(
            context->texture, NULL, frame->pixels, frame->pitch)) {
        return set_sdl_error(presenter, "SDL_UpdateTexture");
    }
    if (!SDL_SetRenderDrawColor(context->renderer, 0, 0, 0, 255))
        return set_sdl_error(presenter, "SDL_SetRenderDrawColor");
    if (!SDL_RenderClear(context->renderer))
        return set_sdl_error(presenter, "SDL_RenderClear");
    if (!SDL_RenderTexture(context->renderer, context->texture, NULL, NULL))
        return set_sdl_error(presenter, "SDL_RenderTexture");
    if (!SDL_RenderPresent(context->renderer))
        return set_sdl_error(presenter, "SDL_RenderPresent");
    return true;
}

static bool sdl_set_fullscreen(
    SnesRecompPresenter *presenter,
    bool fullscreen) {
    SdlPresenterContext *context =
        (SdlPresenterContext *)presenter->context;
    if (!SDL_SetWindowFullscreen(context->window, fullscreen))
        return set_sdl_error(presenter, "SDL_SetWindowFullscreen");
    return true;
}

static bool sdl_set_window_scale(
    SnesRecompPresenter *presenter,
    int scale) {
    SdlPresenterContext *context =
        (SdlPresenterContext *)presenter->context;
    if (scale <= 0) {
        snesrecomp_presenter_set_error(
            presenter, "window scale must be positive");
        return false;
    }
    if (!SDL_SetWindowSize(
            context->window,
            snesrecomp_presenter_display_width(
                presenter, presenter->frame_width) * scale,
            presenter->frame_height * scale)) {
        return set_sdl_error(presenter, "SDL_SetWindowSize");
    }
    return true;
}

static bool sdl_set_window_title(
    SnesRecompPresenter *presenter,
    const char *title) {
    SdlPresenterContext *context =
        (SdlPresenterContext *)presenter->context;
    if (!title || !title[0]) {
        snesrecomp_presenter_set_error(
            presenter, "window title is empty");
        return false;
    }
    if (!SDL_SetWindowTitle(context->window, title))
        return set_sdl_error(presenter, "SDL_SetWindowTitle");
    return true;
}

static bool sdl_get_drawable_size(
    SnesRecompPresenter *presenter,
    int *width,
    int *height) {
    SdlPresenterContext *context =
        (SdlPresenterContext *)presenter->context;
    if (!width || !height) {
        snesrecomp_presenter_set_error(
            presenter, "drawable-size output is null");
        return false;
    }
    if (!SDL_GetWindowSizeInPixels(context->window, width, height))
        return set_sdl_error(presenter, "SDL_GetWindowSizeInPixels");
    return true;
}

static const SnesRecompPresenterOps kSdlPresenterOps = {
    sdl_destroy,
    sdl_present,
    sdl_set_fullscreen,
    sdl_set_window_scale,
    sdl_set_window_title,
    sdl_get_drawable_size,
};

bool snesrecomp_presenter_sdl_create(
    SnesRecompPresenter *presenter,
    const SnesRecompPresentConfig *config) {
    SdlPresenterContext *context =
        (SdlPresenterContext *)calloc(1, sizeof(*context));
    if (!context) {
        snesrecomp_presenter_set_error(
            presenter, "out of memory creating SDL presenter");
        return false;
    }

    presenter->ops = &kSdlPresenterOps;
    presenter->context = context;
    context->preserve_aspect = config->preserve_aspect;
    context->linear_filtering = config->linear_filtering;

    context->window = SDL_CreateWindow(
        config->window_title,
        snesrecomp_presenter_display_width(
            presenter, config->frame_width) * config->window_scale,
        config->frame_height * config->window_scale,
        SDL_WINDOW_RESIZABLE);
    if (!context->window)
        return set_sdl_error(presenter, "SDL_CreateWindow");

    const bool software =
        config->backend == SNESRECOMP_PRESENT_BACKEND_SDL_SOFTWARE;
    context->renderer = SDL_CreateRenderer(
        context->window,
        software ? "software" : NULL);
    if (!context->renderer)
        return set_sdl_error(presenter, "SDL_CreateRenderer");

    presenter->vsync_state = SNESRECOMP_VSYNC_UNSUPPORTED;
    if (!software) {
        if (!SDL_SetRenderVSync(
                context->renderer, config->vsync ? 1 : 0)) {
            fprintf(
                stderr,
                "[snesrecomp-platform] could not set SDL VSync: %s\n",
                SDL_GetError());
        }
        int active = 0;
        if (SDL_GetRenderVSync(context->renderer, &active)) {
            presenter->vsync_state = active
                ? SNESRECOMP_VSYNC_ENABLED
                : SNESRECOMP_VSYNC_DISABLED;
        } else {
            presenter->vsync_state = SNESRECOMP_VSYNC_UNKNOWN;
        }
    }

    if (!create_texture(
            presenter,
            config->pixel_format,
            config->frame_width,
            config->frame_height)) {
        return false;
    }

    if (config->fullscreen &&
        !SDL_SetWindowFullscreen(context->window, true)) {
        return set_sdl_error(presenter, "SDL_SetWindowFullscreen");
    }

    presenter->backend = software
        ? SNESRECOMP_PRESENT_BACKEND_SDL_SOFTWARE
        : SNESRECOMP_PRESENT_BACKEND_SDL;
    presenter->capabilities = SNESRECOMP_PRESENT_CAP_BASIC;

    const char *renderer_name = SDL_GetRendererName(context->renderer);
    snprintf(
        presenter->backend_name,
        sizeof(presenter->backend_name),
        "SDL%s%s",
        software ? " software/" : "/",
        renderer_name && renderer_name[0] ? renderer_name : "");
    presenter->last_error[0] = '\0';
    return true;
}
