#ifndef SNESRECOMP_PLATFORM_PRESENTER_H
#define SNESRECOMP_PLATFORM_PRESENTER_H
#include "snesrecomp_platform/ppu_blit.h"
#include "snesrecomp_platform/abi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "snesrecomp_platform/snes_ppu_mode7.h"
#include "snesrecomp_platform/snes_ppu_obj.h"
#include "snesrecomp_platform/snes_ppu_frame.h"
#include "snesrecomp_platform/snes_ppu_job.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnesRecompPresenter SnesRecompPresenter;

enum {
    SNESRECOMP_PPU_CAP_MODE1 = 1u << 0,
    SNESRECOMP_PPU_CAP_OBJ = 1u << 1,
    SNESRECOMP_PPU_CAP_WINDOWS = 1u << 2,
    SNESRECOMP_PPU_CAP_COLOUR_MATH = 1u << 3,
    SNESRECOMP_PPU_CAP_MODE7 = 1u << 4,
    SNESRECOMP_PPU_CAP_RASTER_PALETTE = 1u << 5,
    SNESRECOMP_PPU_CAP_RESOLVED_BG = 1u << 6,
    SNESRECOMP_PPU_CAP_POST_OVERLAYS = 1u << 7,
    /* Exact resolved source/destination segments, including clipped sampling. */
    SNESRECOMP_PPU_CAP_ELASTIC = 1u << 8,
    /* Integer source brightness for unconditional RGB5 post images. */
    SNESRECOMP_PPU_CAP_POST_RGB5_BRIGHTNESS = 1u << 9,
    SNESRECOMP_PPU_CAP_BG_SOURCES = 1u << 10,
    /* Resolved opaque rectangles over a GPU-generated PPU base. */
    SNESRECOMP_PPU_CAP_POST_BLITS = 1u << 11,
    SNESRECOMP_PPU_CAP_PRIMITIVES = 1u << 12,
    SNESRECOMP_PPU_CAP_INDEXED_LAYOUT = 1u << 13
};

typedef struct SnesRecompPpuStats {
    uint64_t prepared, submitted, cancelled;
    uint64_t rejected[SNESRECOMP_PPU_RESULT_COUNT];
    uint64_t capture_us, cache_us, submit_us, complete_us, present_us, wait_us, fallback_us;
    uint64_t upload_bytes;
    uint32_t draw_calls;
    SnesRecompPpuResult last_result;
    SnesPpuUnsupported unsupported;
    /* A zero duration is a measurement only when its validity bit is set.
     * Completion describes the previous GPU frame; presentation can include
     * queue pacing. These overlapping costs must not be added blindly. */
    uint32_t timing_valid;
    uint32_t primitive_commands, primitive_commands_generated;
    uint32_t primitive_sources_reused, primitive_image_reused;
    uint32_t background_prefix_reused;
} SnesRecompPpuStats;

enum {
    SNESRECOMP_PPU_TIME_CAPTURE = 1u << 0,
    SNESRECOMP_PPU_TIME_CACHE = 1u << 1,
    SNESRECOMP_PPU_TIME_SUBMIT = 1u << 2,
    SNESRECOMP_PPU_TIME_COMPLETE_PREVIOUS = 1u << 3,
    SNESRECOMP_PPU_TIME_PRESENT = 1u << 4,
    SNESRECOMP_PPU_TIME_WAIT = 1u << 5,
    SNESRECOMP_PPU_TIME_FALLBACK = 1u << 6
};

uint32_t snesrecomp_presenter_ppu_capabilities(const SnesRecompPresenter *presenter);
/* Clear per-frame costs, retaining lifetime counters. Pending jobs are unchanged. */
bool snesrecomp_presenter_ppu_begin_frame(SnesRecompPresenter *presenter);
/* Preparation pins the immutable snapshot until submit or cancel.
 * No display or guest side effects are allowed during preparation.
 * Submit consumes the reservation, including on failure. A failed submit
 * must not have published a partial frame. GPU data must be owned by the
 * backend before submit returns; the source may then be reused. */
SnesRecompPpuResult snesrecomp_presenter_ppu_prepare(
    SnesRecompPresenter *presenter, const SnesRecompPpuFrame *frame);
/* A nonempty post-overlay job requires the backend's explicit job operation.
 * Legacy adapters can only prepare empty jobs; required art is never ignored. */
SnesRecompPpuResult snesrecomp_presenter_ppu_prepare_job(
    SnesRecompPresenter *presenter, const SnesRecompPpuJob *job);
SnesRecompPpuResult snesrecomp_presenter_ppu_submit(SnesRecompPresenter *presenter);
void snesrecomp_presenter_ppu_cancel(SnesRecompPresenter *presenter);
void snesrecomp_presenter_ppu_stats(const SnesRecompPresenter *presenter,
                                   SnesRecompPpuStats *out);

typedef enum SnesRecompPresentBackend {
    SNESRECOMP_PRESENT_BACKEND_AUTO = 0,
    SNESRECOMP_PRESENT_BACKEND_SDL,
    SNESRECOMP_PRESENT_BACKEND_SDL_SOFTWARE,
    SNESRECOMP_PRESENT_BACKEND_OPENGL,
    SNESRECOMP_PRESENT_BACKEND_SDL_GPU,
    SNESRECOMP_PRESENT_BACKEND_NATIVE,
    /* Appended. The existing values travel in games' stored configuration,
     * so they are never renumbered. */
    SNESRECOMP_PRESENT_BACKEND_VULKAN
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
    SNESRECOMP_PRESENT_CAP_HD_MODE7 = 1u << 5,
    SNESRECOMP_PRESENT_CAP_RESOLVED_BLITS = 1u << 6
} SnesRecompPresentCapability;

/*
 * Backend-neutral final-scaling policy, named after the technique rather than
 * after any game's preset, so a backend honours it without knowing which
 * preset asked for it. A backend that cannot do it presents as DEFAULT and
 * says so through its backend name; this is a look, not a correctness gate.
 */
typedef enum SnesRecompPresentScaling {
    /* Plain nearest or linear sampling, chosen by linear_filtering. */
    SNESRECOMP_PRESENT_SCALING_DEFAULT = 0,
    /* Point-sample within a texel, blend only across texel boundaries, and
     * snap to nearest where the scale factor is an integer. */
    SNESRECOMP_PRESENT_SCALING_SHARP_BILINEAR
} SnesRecompPresentScaling;

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
SNESRECOMP_STATIC_ASSERT(sizeof(SnesRecompPresentBackend) == sizeof(int) &&
                   sizeof(SnesRecompPixelFormat) == sizeof(int) &&
                   sizeof(SnesRecompOverlaySpace) == sizeof(int) &&
                   sizeof(SnesRecompPresentScaling) == sizeof(int) &&
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
    /* Appended, and zero keeps the previous behaviour, so a caller that
     * memsets its config needs no change. */
    SnesRecompPresentScaling scaling;
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
    /* Internal rendering scale. An unadvertised one is refused, never
     * quietly reduced. */
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
/* Synchronous opaque composition before presentation filtering. Sources stay
 * immutable through this call. Failure presents nothing; the consumer applies
 * the CPU oracle and calls ordinary present. No implicit base-only display. */
bool snesrecomp_presenter_present_blits(SnesRecompPresenter *presenter,
    const SnesRecompVideoFrame *base, const SnesRecompBlitPlan *plan);

/* Returns false without presenting when this backend cannot render the
 * requested semantic frame. The caller can then submit its authentic frame. */
bool snesrecomp_presenter_present_mode7_hd(
    SnesRecompPresenter *presenter,
    const SnesRecompMode7HdFrame *frame);

bool snesrecomp_presenter_set_fullscreen(
    SnesRecompPresenter *presenter,
    bool fullscreen);

/* Change presentation pacing without recreating the window. Returns false
 * when unsupported or rejected. Vulkan applies the request on its next present;
 * vsync_state reports the active mode, which may differ on some drivers. */
bool snesrecomp_presenter_set_vsync(
    SnesRecompPresenter *presenter,
    bool enabled);

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

/* Display refresh in millihertz; zero when unknown. */
unsigned snesrecomp_presenter_display_millihertz(
    SnesRecompPresenter *presenter);

uint32_t snesrecomp_presenter_capabilities(
    const SnesRecompPresenter *presenter);

/* Internal Mode 7 scales this presenter can run, as a mask of 1u << scale;
 * zero when it has no semantic Mode 7 path. */
uint32_t snesrecomp_presenter_mode7_scales(
    const SnesRecompPresenter *presenter);

bool snesrecomp_presenter_mode7_scale_supported(
    const SnesRecompPresenter *presenter,
    unsigned scale);

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
