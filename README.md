# snesrecomp-platform

A small host-platform library for games built with
[`snesrecomp`](https://github.com/mstan/snesrecomp).

It gives recomp projects one shared place for window handling, frame output and
graphics backends. Games submit a raw SNES frame and choose a backend; the
library takes care of presenting it.

## Current features

- SDL accelerated and software output
- OpenGL 3.3 output
- pixel-aspect handling, scaling, fullscreen, filtering and VSync reporting
- optional GLSL/GLSLP shader presets
- a public native-presenter backend contract for target adapters
- clear capability reporting and error messages
- an SDL-free runtime-policy target for shared host behavior
- portable SNES PPU capture, tile, raster, OBJ and Mode-7 semantics
- SHA-1 and a ROM identity gate driven by a per-game specification
- a host-boot boundary, so a game needs no platform `#ifdef`s

The public API contains no game-specific state and does not depend on generated
recomp code. It is intended to be shared by more than one SNES recomp project.

## Using it

Add the directory to CMake and link the targets you need:

```cmake
add_subdirectory(path/to/snesrecomp-platform)

target_link_libraries(MyRecomp PRIVATE
    snesrecomp::platform
    snesrecomp::runtime
)
```

OpenGL is optional:

```cmake
set(SNESRECOMP_PLATFORM_ENABLE_OPENGL ON)
set(SNESRECOMP_PLATFORM_GL_CORE_DIR "path/to/gl/loader")
```

After including `snesrecomp`'s runner CMake file, shader presets can be enabled
with:

```cmake
snesrecomp_platform_target_glsl_presets(MyRecomp)
```

## Native target adapters

A console adapter can export native presenter sources, include paths and
libraries before this project is added. The portable library owns SNES state
and semantic compilation; the adapter owns SDK objects and draw submission.
This keeps GXM or another target API out of game code and out of portable
tests.

Mode 7 follows the same boundary: `snes_ppu_mode7.h` compiles the affine state
on any host, and a backend consumes it without reinterpreting PPU registers.

## Build overlays

The included CMake helpers can create reviewed build-tree copies for small
version-pinned integrations with `snesrecomp` and `recomp-ui`. Fetched
dependency sources stay unchanged.

## Tests

Configure with `SNESRECOMP_PLATFORM_BUILD_TESTS=ON` to build the SDL/OpenGL
presenter smoke tests and the runtime-policy test.
