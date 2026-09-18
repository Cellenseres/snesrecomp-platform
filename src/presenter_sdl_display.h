#ifndef SNESRECOMP_PLATFORM_PRESENTER_SDL_DISPLAY_H
#define SNESRECOMP_PLATFORM_PRESENTER_SDL_DISPLAY_H

#include <SDL3/SDL.h>

/* Refresh of a window's display, in millihertz. */
static unsigned snesrecomp_sdl_display_millihertz(SDL_Window *window) {
    const SDL_DisplayMode *mode;
    SDL_DisplayID display;

    if (!window)
        return 0u;
    display = SDL_GetDisplayForWindow(window);
    if (!display)
        return 0u;
    mode = SDL_GetCurrentDisplayMode(display);
    if (!mode || mode->refresh_rate <= 0.0f)
        return 0u;
    return (unsigned)((double)mode->refresh_rate * 1000.0 + 0.5);
}

#endif
