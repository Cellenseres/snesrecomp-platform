#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

#include "snesrecomp_platform/presenter_backend.h"

static int fail(const char *message) {
    fprintf(stderr, "VSync smoke failed: %s\n", message);
    return 1;
}

int main(int argc, char **argv) {
    /* Older native backends may omit the optional operation. */
    const SnesRecompPresenterOps legacy_ops = {0};
    SnesRecompPresenter legacy = {0};
    legacy.ops = &legacy_ops;
    if (snesrecomp_presenter_set_vsync(NULL, false) ||
        snesrecomp_presenter_set_vsync(&legacy, false) ||
        !strstr(snesrecomp_presenter_last_error(&legacy), "unsupported"))
        return fail("unsupported operation contract");

    SnesRecompPresentConfig config = {0};
    config.window_title = "Runtime VSync smoke";
    config.backend = SNESRECOMP_PRESENT_BACKEND_OPENGL;
    if (argc > 1 && strcmp(argv[1], "sdl") == 0)
        config.backend = SNESRECOMP_PRESENT_BACKEND_SDL;
    else if (argc > 1 && strcmp(argv[1], "software") == 0)
        config.backend = SNESRECOMP_PRESENT_BACKEND_SDL_SOFTWARE;
    else if (argc > 1 && strcmp(argv[1], "vulkan") == 0)
        config.backend = SNESRECOMP_PRESENT_BACKEND_VULKAN;
    config.pixel_format = SNESRECOMP_PIXEL_FORMAT_ARGB8888;
    config.frame_width = 256;
    config.frame_height = 224;
    config.window_scale = 1;
    config.vsync = true;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "VSync smoke unavailable: %s\n", SDL_GetError());
        return 77;
    }
    char error[256];
    SnesRecompPresenter *presenter = NULL;
    if (!snesrecomp_presenter_create(&config, &presenter, error, sizeof(error))) {
        fprintf(stderr, "VSync smoke unavailable: %s\n", error);
        SDL_Quit();
        return 77;
    }

    int result = 0;
    if (config.backend == SNESRECOMP_PRESENT_BACKEND_SDL_SOFTWARE) {
        if (snesrecomp_presenter_set_vsync(presenter, false) ||
            snesrecomp_presenter_vsync_state(presenter) !=
                SNESRECOMP_VSYNC_UNSUPPORTED)
            result = fail("software backend changed its unsupported state");
    } else {
        /* A second GL presenter makes sure the setter selects its own context. */
        SnesRecompPresenter *other = NULL;
        if (config.backend == SNESRECOMP_PRESENT_BACKEND_OPENGL) {
            if (!snesrecomp_presenter_create(&config, &other, error, sizeof(error)))
                result = fail(error);
        }
        static uint32_t pixels[256 * 224];
        const SnesRecompVideoFrame frame = {
            .pixels = pixels,
            .pixel_format = SNESRECOMP_PIXEL_FORMAT_ARGB8888,
            .width = 256, .height = 224, .pitch = 256 * 4,
        };
        const bool modes[] = {true, false, true, false, false, true};
        for (unsigned i = 0; !result && i < sizeof(modes) / sizeof(modes[0]); ++i) {
            if (other && !snesrecomp_presenter_present(other, &frame)) {
                result = fail(snesrecomp_presenter_last_error(other));
                break;
            }
            if (!snesrecomp_presenter_set_vsync(presenter, modes[i])) {
                result = fail(snesrecomp_presenter_last_error(presenter));
                break;
            }
            const Uint64 start = SDL_GetPerformanceCounter();
            for (unsigned j = 0; j < 20; ++j) {
                SDL_PumpEvents();
                if (!snesrecomp_presenter_present(presenter, &frame)) {
                    result = fail(snesrecomp_presenter_last_error(presenter));
                    break;
                }
            }
            const double elapsed_ms =
                1000.0 * (double)(SDL_GetPerformanceCounter() - start) /
                (double)SDL_GetPerformanceFrequency();
            const SnesRecompVSyncState state =
                snesrecomp_presenter_vsync_state(presenter);
            fprintf(stderr, "%s request=%d active=%s 20 presents=%.2fms\n",
                snesrecomp_presenter_backend_name(presenter), modes[i],
                snesrecomp_vsync_state_name(state), elapsed_ms);
            /* Vulkan may only expose FIFO. Report that actual mode honestly. */
            if (config.backend != SNESRECOMP_PRESENT_BACKEND_VULKAN &&
                state != (modes[i] ? SNESRECOMP_VSYNC_ENABLED :
                                    SNESRECOMP_VSYNC_DISABLED))
                result = fail("active VSync did not follow the request");
            if (other) {
                int interval = 0;
                if (!SDL_GL_GetSwapInterval(&interval) ||
                    (interval != 0) != modes[i])
                    result = fail("wrong OpenGL context or swap interval");
                if (!snesrecomp_presenter_present(other, &frame) ||
                    !SDL_GL_GetSwapInterval(&interval) || interval != 1)
                    result = fail("another OpenGL context was changed");
            }
        }
        snesrecomp_presenter_destroy(other);
    }
    snesrecomp_presenter_destroy(presenter);
    SDL_Quit();
    return result;
}
