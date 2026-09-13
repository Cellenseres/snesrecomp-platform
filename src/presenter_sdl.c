#include "snesrecomp_platform/presenter_backend.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SdlPresenterContext {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_Texture *overlay_textures[SNESRECOMP_OVERLAY_MAX_LAYERS];
    int overlay_widths[SNESRECOMP_OVERLAY_MAX_LAYERS];
    int overlay_heights[SNESRECOMP_OVERLAY_MAX_LAYERS];
    bool overlay_linear[SNESRECOMP_OVERLAY_MAX_LAYERS];
    SnesRecompPixelFormat texture_format;
    int texture_width;
    int texture_height;
    bool preserve_aspect;
    bool linear_filtering;
} SdlPresenterContext;

static bool set_sdl_error(
    SnesRecompPresenter *presenter,
    const char *operation);

static bool valid_overlay(const SnesRecompOverlayFrame *overlay,
                          int frame_width, int frame_height) {
    if (!overlay)
        return true;
    if (!overlay->layers || overlay->layer_count == 0 ||
        overlay->layer_count > SNESRECOMP_OVERLAY_MAX_LAYERS ||
        overlay->canvas_width <= 0 || overlay->canvas_height <= 0 ||
        (overlay->space != SNESRECOMP_OVERLAY_SPACE_FRAME &&
         overlay->space != SNESRECOMP_OVERLAY_SPACE_PRESENTATION) ||
        (overlay->space == SNESRECOMP_OVERLAY_SPACE_FRAME &&
         (overlay->canvas_width != frame_width ||
          overlay->canvas_height != frame_height))) {
        return false;
    }
    for (size_t i = 0; i < overlay->layer_count; i++) {
        const SnesRecompOverlayLayer *layer = &overlay->layers[i];
        if (!layer->pixels ||
            layer->pixel_format != SNESRECOMP_PIXEL_FORMAT_ARGB8888 ||
            layer->width <= 0 || layer->height <= 0 ||
            layer->pitch < layer->width * 4 || (layer->pitch & 3) != 0 ||
            layer->display_width <= 0 || layer->display_height <= 0) {
            return false;
        }
    }
    return true;
}

static bool update_overlay_texture(
    SnesRecompPresenter *presenter,
    size_t index,
    const SnesRecompOverlayLayer *layer) {
    SdlPresenterContext *context =
        (SdlPresenterContext *)presenter->context;
    if (!context->overlay_textures[index] ||
        context->overlay_widths[index] != layer->width ||
        context->overlay_heights[index] != layer->height) {
        SDL_Texture *texture = SDL_CreateTexture(
            context->renderer, SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING, layer->width, layer->height);
        if (!texture)
            return set_sdl_error(presenter, "SDL_CreateTexture(overlay)");
        const SDL_BlendMode premultiplied = SDL_ComposeCustomBlendMode(
            SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ONE,
            SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
        if (!SDL_SetTextureBlendMode(texture, premultiplied)) {
            SDL_DestroyTexture(texture);
            return set_sdl_error(presenter, "configure overlay texture");
        }
        if (context->overlay_textures[index])
            SDL_DestroyTexture(context->overlay_textures[index]);
        context->overlay_textures[index] = texture;
        context->overlay_widths[index] = layer->width;
        context->overlay_heights[index] = layer->height;
        context->overlay_linear[index] = !layer->linear_filtering;
    }
    if (context->overlay_linear[index] != layer->linear_filtering &&
        !SDL_SetTextureScaleMode(
            context->overlay_textures[index],
            layer->linear_filtering
                ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST)) {
        return set_sdl_error(presenter, "SDL_SetTextureScaleMode(overlay)");
    }
    context->overlay_linear[index] = layer->linear_filtering;
    if (!SDL_UpdateTexture(
            context->overlay_textures[index], NULL,
            layer->pixels, layer->pitch)) {
        return set_sdl_error(presenter, "SDL_UpdateTexture(overlay)");
    }
    return true;
}

static bool render_overlay(
    SnesRecompPresenter *presenter,
    const SnesRecompOverlayFrame *overlay) {
    SdlPresenterContext *context =
        (SdlPresenterContext *)presenter->context;
    int logical_width = 0, logical_height = 0;
    int output_width = 0, output_height = 0;
    SDL_RendererLogicalPresentation logical_mode =
        SDL_LOGICAL_PRESENTATION_DISABLED;
    SDL_FRect base;

    if (!overlay)
        return true;
    if (!SDL_GetRenderOutputSize(
            context->renderer, &output_width, &output_height)) {
        return set_sdl_error(presenter, "SDL_GetRenderOutputSize");
    }
    if (!SDL_GetRenderLogicalPresentation(
            context->renderer, &logical_width, &logical_height,
            &logical_mode)) {
        return set_sdl_error(presenter, "SDL_GetRenderLogicalPresentation");
    }
    if (overlay->space == SNESRECOMP_OVERLAY_SPACE_FRAME &&
        logical_mode != SDL_LOGICAL_PRESENTATION_DISABLED) {
        if (!SDL_GetRenderLogicalPresentationRect(context->renderer, &base))
            return set_sdl_error(
                presenter, "SDL_GetRenderLogicalPresentationRect");
    } else {
        base.x = 0.0f;
        base.y = 0.0f;
        base.w = (float)output_width;
        base.h = (float)output_height;
    }

    if (logical_mode != SDL_LOGICAL_PRESENTATION_DISABLED &&
        !SDL_SetRenderLogicalPresentation(
            context->renderer, 0, 0,
            SDL_LOGICAL_PRESENTATION_DISABLED)) {
        return set_sdl_error(
            presenter, "disable logical presentation for overlay");
    }

    bool ok = true;
    for (size_t i = 0; i < overlay->layer_count; i++) {
        const SnesRecompOverlayLayer *layer = &overlay->layers[i];
        SDL_FRect destination = {
            base.x + base.w * (float)layer->x /
                (float)overlay->canvas_width,
            base.y + base.h * (float)layer->y /
                (float)overlay->canvas_height,
            base.w * (float)layer->display_width /
                (float)overlay->canvas_width,
            base.h * (float)layer->display_height /
                (float)overlay->canvas_height,
        };
        if (!update_overlay_texture(presenter, i, layer)) {
            ok = false;
            break;
        }
        if (!SDL_RenderTexture(
                context->renderer, context->overlay_textures[i],
                NULL, &destination)) {
            set_sdl_error(presenter, "SDL_RenderTexture(overlay)");
            ok = false;
            break;
        }
    }

    if (logical_mode != SDL_LOGICAL_PRESENTATION_DISABLED &&
        !SDL_SetRenderLogicalPresentation(
            context->renderer, logical_width, logical_height,
            logical_mode)) {
        return set_sdl_error(
            presenter, "restore logical presentation after overlay");
    }
    return ok;
}

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
    for (size_t i = 0; i < SNESRECOMP_OVERLAY_MAX_LAYERS; i++) {
        if (context->overlay_textures[i])
            SDL_DestroyTexture(context->overlay_textures[i]);
    }
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
        frame->height <= 0 || frame->pitch < frame->width * 4 ||
        !valid_overlay(frame->overlay, frame->width, frame->height)) {
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
    if (!render_overlay(presenter, frame->overlay))
        return false;
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

static float sdl_get_display_scale(SnesRecompPresenter *presenter) {
    SdlPresenterContext *context =
        (SdlPresenterContext *)presenter->context;
    return SDL_GetWindowDisplayScale(context->window);
}

static const SnesRecompPresenterOps kSdlPresenterOps = {
    sdl_destroy,
    sdl_present,
    sdl_set_fullscreen,
    sdl_set_window_scale,
    sdl_set_window_title,
    sdl_get_drawable_size,
    NULL,
    sdl_get_display_scale,
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
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
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
    presenter->capabilities =
        SNESRECOMP_PRESENT_CAP_BASIC | SNESRECOMP_PRESENT_CAP_OVERLAYS;

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
