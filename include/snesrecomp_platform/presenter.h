#ifndef SNESRECOMP_PLATFORM_PRESENTER_H
#define SNESRECOMP_PLATFORM_PRESENTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnesRecompPresenter SnesRecompPresenter;

typedef enum SnesRecompPresentBackend {
    SNESRECOMP_PRESENT_BACKEND_AUTO = 0,
    SNESRECOMP_PRESENT_BACKEND_SDL,
    SNESRECOMP_PRESENT_BACKEND_SDL_SOFTWARE,
    SNESRECOMP_PRESENT_BACKEND_OPENGL,
    SNESRECOMP_PRESENT_BACKEND_SDL_GPU
} SnesRecompPresentBackend;

typedef enum SnesRecompPixelFormat {
    SNESRECOMP_PIXEL_FORMAT_ARGB8888 = 0
} SnesRecompPixelFormat;

typedef enum SnesRecompPresentCapability {
    SNESRECOMP_PRESENT_CAP_BASIC = 1u << 0,
    SNESRECOMP_PRESENT_CAP_SHADER = 1u << 1,
    SNESRECOMP_PRESENT_CAP_MULTIPASS = 1u << 2,
    SNESRECOMP_PRESENT_CAP_OVERLAYS = 1u << 3,
    SNESRECOMP_PRESENT_CAP_3D = 1u << 4
} SnesRecompPresentCapability;

typedef enum SnesRecompVSyncState {
    SNESRECOMP_VSYNC_UNKNOWN = 0,
    SNESRECOMP_VSYNC_DISABLED,
    SNESRECOMP_VSYNC_ENABLED,
    SNESRECOMP_VSYNC_UNSUPPORTED
} SnesRecompVSyncState;

/*
 * Optional bridge for shader-preset implementations. The presenter owns the
 * source texture and GL context; the injected renderer owns only its opaque
 * preset instance. This keeps the platform library independent of a game's
 * configuration ABI and of any particular preset parser.
 */
typedef struct SnesRecompShaderPresetInterface {
    void *(*create)(
        const char *path,
        char *error,
        size_t error_size);
    void (*destroy)(void *preset);
    void (*render)(
        void *preset,
        uint32_t source_texture,
        int source_width,
        int source_height,
        int viewport_x,
        int viewport_y,
        int viewport_width,
        int viewport_height);
} SnesRecompShaderPresetInterface;

typedef struct SnesRecompPresentConfig {
    const char *window_title;
    SnesRecompPresentBackend backend;
    SnesRecompPixelFormat pixel_format;
    int frame_width;
    int frame_height;
    int window_scale;
    bool vsync;
    /* Horizontal pixel aspect. Non-positive values select square pixels. */
    int pixel_aspect_numerator;
    int pixel_aspect_denominator;
    bool preserve_aspect;
    bool linear_filtering;
    bool fullscreen;
    const char *shader_preset_path;
    const SnesRecompShaderPresetInterface *shader_preset_interface;
} SnesRecompPresentConfig;

typedef struct SnesRecompVideoFrame {
    const void *pixels;
    SnesRecompPixelFormat pixel_format;
    int width;
    int height;
    int pitch;
} SnesRecompVideoFrame;

bool snesrecomp_presenter_create(
    const SnesRecompPresentConfig *config,
    SnesRecompPresenter **out_presenter,
    char *error,
    size_t error_size);

void snesrecomp_presenter_destroy(SnesRecompPresenter *presenter);

bool snesrecomp_presenter_present(
    SnesRecompPresenter *presenter,
    const SnesRecompVideoFrame *frame);

bool snesrecomp_presenter_set_fullscreen(
    SnesRecompPresenter *presenter,
    bool fullscreen);

bool snesrecomp_presenter_set_window_scale(
    SnesRecompPresenter *presenter,
    int scale);

bool snesrecomp_presenter_set_window_title(
    SnesRecompPresenter *presenter,
    const char *title);

bool snesrecomp_presenter_get_drawable_size(
    SnesRecompPresenter *presenter,
    int *width,
    int *height);

uint32_t snesrecomp_presenter_capabilities(
    const SnesRecompPresenter *presenter);

SnesRecompPresentBackend snesrecomp_presenter_backend(
    const SnesRecompPresenter *presenter);

const char *snesrecomp_presenter_backend_name(
    const SnesRecompPresenter *presenter);

SnesRecompVSyncState snesrecomp_presenter_vsync_state(
    const SnesRecompPresenter *presenter);

const char *snesrecomp_vsync_state_name(SnesRecompVSyncState state);

const char *snesrecomp_presenter_last_error(
    const SnesRecompPresenter *presenter);

#ifdef __cplusplus
}
#endif

#endif
