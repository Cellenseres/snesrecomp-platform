#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "snesrecomp_platform/presenter.h"

static int fail(const char *message) {
    fprintf(stderr, "platform smoke test failed: %s\n", message);
    return 1;
}

int main(int argc, char **argv) {
    const bool test_opengl =
        argc > 1 && strcmp(argv[1], "opengl") == 0;
    if (!SDL_Init(SDL_INIT_VIDEO))
        return test_opengl ? 77 : fail(SDL_GetError());

    SnesRecompPresentConfig config;
    memset(&config, 0, sizeof(config));
    config.window_title = "snesrecomp-platform smoke test";
    config.backend = test_opengl
        ? SNESRECOMP_PRESENT_BACKEND_OPENGL
        : SNESRECOMP_PRESENT_BACKEND_SDL_SOFTWARE;
    config.pixel_format = SNESRECOMP_PIXEL_FORMAT_ARGB8888;
    config.frame_width = 4;
    config.frame_height = 4;
    config.window_scale = 1;
    config.pixel_aspect_numerator = 2;
    config.pixel_aspect_denominator = 1;
    config.preserve_aspect = true;

    char error[256];
    SnesRecompPresenter *presenter = NULL;
    if (!snesrecomp_presenter_create(
            &config, &presenter, error, sizeof(error))) {
        SDL_Quit();
        return test_opengl ? 77 : fail(error);
    }

    uint32_t pixels[16];
    memset(pixels, 0, sizeof(pixels));
    const SnesRecompVideoFrame frame = {
        .pixels = pixels,
        .pixel_format = SNESRECOMP_PIXEL_FORMAT_ARGB8888,
        .width = 4,
        .height = 4,
        .pitch = 4 * (int)sizeof(uint32_t),
    };

    int result = 0;
    int drawable_width = 0;
    int drawable_height = 0;
    if (!(snesrecomp_presenter_capabilities(presenter) &
          SNESRECOMP_PRESENT_CAP_BASIC)) {
        result = fail("basic capability is missing");
    } else if (!snesrecomp_presenter_present(presenter, &frame)) {
        result = fail(snesrecomp_presenter_last_error(presenter));
    } else if (!snesrecomp_presenter_set_window_title(
                   presenter, "updated smoke test")) {
        result = fail(snesrecomp_presenter_last_error(presenter));
    } else if (!snesrecomp_presenter_set_window_scale(presenter, 2)) {
        result = fail(snesrecomp_presenter_last_error(presenter));
    } else if (!snesrecomp_presenter_get_drawable_size(
                   presenter, &drawable_width, &drawable_height)) {
        result = fail(snesrecomp_presenter_last_error(presenter));
    } else if (drawable_width <= 0 || drawable_height <= 0) {
        result = fail("drawable dimensions are invalid");
    } else if (drawable_width != drawable_height * 2) {
        result = fail("pixel aspect was not applied to the drawable");
    }

    const SnesRecompVSyncState vsync_state =
        snesrecomp_presenter_vsync_state(presenter);
    if (result == 0 &&
        (unsigned)vsync_state > (unsigned)SNESRECOMP_VSYNC_UNSUPPORTED) {
        result = fail("VSync state is invalid");
    } else if (result == 0 && !snesrecomp_vsync_state_name(vsync_state)[0]) {
        result = fail("VSync state name is empty");
    }

    snesrecomp_presenter_destroy(presenter);
    SDL_Quit();
    return result;
}
