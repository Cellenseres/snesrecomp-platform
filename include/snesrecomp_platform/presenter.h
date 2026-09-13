#ifndef SNESRECOMP_PLATFORM_PRESENTER_H
#define SNESRECOMP_PLATFORM_PRESENTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "snesrecomp_platform/snes_ppu_mode7.h"
#include "snesrecomp_platform/snes_ppu_obj.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnesRecompPresenter SnesRecompPresenter;

typedef enum SnesRecompPresentBackend {
    SNESRECOMP_PRESENT_BACKEND_AUTO = 0,
    SNESRECOMP_PRESENT_BACKEND_SDL,
    SNESRECOMP_PRESENT_BACKEND_SDL_SOFTWARE,
    SNESRECOMP_PRESENT_BACKEND_OPENGL,
    SNESRECOMP_PRESENT_BACKEND_SDL_GPU,
    SNESRECOMP_PRESENT_BACKEND_NATIVE
} SnesRecompPresentBackend;

typedef enum SnesRecompPixelFormat {
    SNESRECOMP_PIXEL_FORMAT_ARGB8888 = 0
} SnesRecompPixelFormat;

typedef enum SnesRecompPresentCapability {
    SNESRECOMP_PRESENT_CAP_BASIC = 1u << 0,
    SNESRECOMP_PRESENT_CAP_SHADER = 1u << 1,
    SNESRECOMP_PRESENT_CAP_MULTIPASS = 1u << 2,
    SNESRECOMP_PRESENT_CAP_OVERLAYS = 1u << 3,
    SNESRECOMP_PRESENT_CAP_3D = 1u << 4,
    SNESRECOMP_PRESENT_CAP_HD_MODE7 = 1u << 5
} SnesRecompPresentCapability;

typedef enum SnesRecompOverlaySpace {
    /* Coordinates follow the submitted game's logical frame. */
    SNESRECOMP_OVERLAY_SPACE_FRAME = 0,
    /* Coordinates are presentation pixels, independent of the game frame. */
    SNESRECOMP_OVERLAY_SPACE_PRESENTATION
} SnesRecompOverlaySpace;

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

/* These types cross static-library boundaries. GCC's arm-*-eabi default can
 * otherwise give the adapter and its caller different structure layouts. */
_Static_assert(sizeof(SnesRecompPresentBackend) == sizeof(int) &&
                   sizeof(SnesRecompPixelFormat) == sizeof(int) &&
                   sizeof(SnesRecompOverlaySpace) == sizeof(int) &&
                   sizeof(SnesRecompVSyncState) == sizeof(int),
               "snesrecomp_platform requires int-sized enums; build this "
               "target with -fno-short-enums.");

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
    /* Optional premultiplied-alpha UI, composited after shaders and semantic
     * passes so it never changes the guest framebuffer. */
    const struct SnesRecompOverlayFrame *overlay;
} SnesRecompVideoFrame;

typedef struct SnesRecompOverlayLayer {
    const void *pixels;
    SnesRecompPixelFormat pixel_format;
    int width;
    int height;
    int pitch;
    /* Destination rectangle, with a top-left origin, in the overlay frame's
     * coordinate space. Non-positive display dimensions are invalid. */
    int x;
    int y;
    int display_width;
    int display_height;
    /* Independent from game filtering. Pixel-art UI normally leaves this
     * false; high-resolution/vector-derived surfaces may opt into it. */
    bool linear_filtering;
} SnesRecompOverlayLayer;

#define SNESRECOMP_OVERLAY_MAX_LAYERS 8u

typedef struct SnesRecompOverlayFrame {
    const SnesRecompOverlayLayer *layers;
    size_t layer_count;
    SnesRecompOverlaySpace space;
    /* Coordinate canvas. FRAME overlays normally use the submitted logical
     * frame size. PRESENTATION overlays use the drawable size observed while
     * composing; backends rescale safely if the window changes before draw. */
    int canvas_width;
    int canvas_height;
} SnesRecompOverlayFrame;

/* Optional semantic Mode 7 presentation. This is deliberately separate from
 * SnesRecompVideoFrame: the backend reconstructs the plane from authoritative
 * SNES data rather than sharpening an already-rasterised image. */
typedef struct SnesRecompMode7HdFrame {
    const SnesPpuFrameCapture *capture;
    const SnesRecompMode7Line *lines;
    unsigned line_count;
    const SnesRecompObjFrame *obj;
    /* Optional full logical tile-number plane. NULL preserves the native
     * 128x128 Mode 7 map captured from VRAM. */
    const SnesRecompMode7MapSource *map_source;
    unsigned scale;

    /* Enhancements, off by default so the pass stays pixel exact against the
     * CPU reference. Filtering resolves four taps to colour and blends them;
     * interpolation samples the plane between two native scanlines instead of
     * repeating one. */
    bool filter_bg;
    bool interpolate_lines;
    const SnesRecompOverlayFrame *overlay;
} SnesRecompMode7HdFrame;

bool snesrecomp_presenter_create(
    const SnesRecompPresentConfig *config,
    SnesRecompPresenter **out_presenter,
    char *error,
    size_t error_size);

void snesrecomp_presenter_destroy(SnesRecompPresenter *presenter);

bool snesrecomp_presenter_present(
    SnesRecompPresenter *presenter,
    const SnesRecompVideoFrame *frame);

/* Returns false without presenting when this backend cannot render the
 * requested semantic frame. The caller can then submit its authentic frame. */
bool snesrecomp_presenter_present_mode7_hd(
    SnesRecompPresenter *presenter,
    const SnesRecompMode7HdFrame *frame);

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

/* Host UI scale reported by the window system (1.0 at 100%, 2.0 at 200%).
 * It is intentionally unrelated to the emulated frame or window zoom. */
float snesrecomp_presenter_display_scale(
    SnesRecompPresenter *presenter);

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
