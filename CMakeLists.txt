cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(snesrecomp_platform C)
endif()

if(NOT TARGET SDL3::SDL3)
    find_package(SDL3 CONFIG REQUIRED)
endif()

add_library(snesrecomp_platform_runtime STATIC
    src/runtime_policy.c
    src/sha1.c
    src/rom_verify.c
    src/msu_pack.c
)
add_library(snesrecomp::runtime ALIAS snesrecomp_platform_runtime)
target_compile_features(snesrecomp_platform_runtime PUBLIC c_std_11)
target_include_directories(snesrecomp_platform_runtime
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
set_target_properties(snesrecomp_platform_runtime PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

add_library(snesrecomp_platform STATIC
    src/presenter.c
    src/presenter_sdl.c
    src/task.c
    # SNES semantics shared by reference renderers and native GPU backends.
    # These sources contain no console SDK or game-specific code.
    src/snes_ppu_capture.c
    src/snes_ppu_mode7.c
    src/snes_tiles.c
    src/snes_bg_upload.c
    src/snes_bg_strips.c
    src/snes_ppu_semantics.c
    src/snes_ppu_semantic_gpu.c
    src/snes_ppu_obj.c
    src/snes_ppu_trace.c
)

# A target adapter may contribute one native presenter implementation.  The
# portable project consumes a generic source/include/library contract and does
# not need to know whether the backend is GXM or another console API.
if(SNESRECOMP_PLATFORM_NATIVE_PRESENTER_SOURCES)
    target_sources(snesrecomp_platform PRIVATE
        ${SNESRECOMP_PLATFORM_NATIVE_PRESENTER_SOURCES})
    if(SNESRECOMP_PLATFORM_NATIVE_PRESENTER_INCLUDE_DIRS)
        target_include_directories(snesrecomp_platform PRIVATE
            ${SNESRECOMP_PLATFORM_NATIVE_PRESENTER_INCLUDE_DIRS})
    endif()
    if(SNESRECOMP_PLATFORM_NATIVE_PRESENTER_LIBRARIES)
        target_link_libraries(snesrecomp_platform PRIVATE
            ${SNESRECOMP_PLATFORM_NATIVE_PRESENTER_LIBRARIES})
    endif()
    target_compile_definitions(snesrecomp_platform PUBLIC
        SNESRECOMP_PLATFORM_HAS_NATIVE_PRESENTER=1
        SNESRECOMP_PLATFORM_HAS_GPU_BG=1)
else()
    target_compile_definitions(snesrecomp_platform PUBLIC
        SNESRECOMP_PLATFORM_HAS_NATIVE_PRESENTER=0
        SNESRECOMP_PLATFORM_HAS_GPU_BG=0)
endif()

add_library(snesrecomp::platform ALIAS snesrecomp_platform)

target_compile_features(snesrecomp_platform PUBLIC c_std_11)

target_include_directories(snesrecomp_platform
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(snesrecomp_platform PRIVATE SDL3::SDL3)

option(SNESRECOMP_PLATFORM_ENABLE_OPENGL
    "Build the OpenGL 3.3 presentation backend" OFF)
set(SNESRECOMP_PLATFORM_GL_CORE_DIR "" CACHE PATH
    "Directory containing gl_core_3_1.c and gl_core_3_1.h")

if(SNESRECOMP_PLATFORM_ENABLE_OPENGL)
    if(NOT EXISTS "${SNESRECOMP_PLATFORM_GL_CORE_DIR}/gl_core_3_1.c" OR
       NOT EXISTS "${SNESRECOMP_PLATFORM_GL_CORE_DIR}/gl_core_3_1.h")
        message(FATAL_ERROR
            "SNESRECOMP_PLATFORM_GL_CORE_DIR must contain gl_core_3_1.c/.h")
    endif()

    find_package(OpenGL REQUIRED)
    target_sources(snesrecomp_platform PRIVATE
        src/presenter_opengl.c
        "${SNESRECOMP_PLATFORM_GL_CORE_DIR}/gl_core_3_1.c")
    target_include_directories(snesrecomp_platform PRIVATE
        "${SNESRECOMP_PLATFORM_GL_CORE_DIR}")
    target_link_libraries(snesrecomp_platform PRIVATE OpenGL::GL)
    target_compile_definitions(snesrecomp_platform PRIVATE
        SNESRECOMP_PLATFORM_HAS_OPENGL=1)
else()
    target_compile_definitions(snesrecomp_platform PRIVATE
        SNESRECOMP_PLATFORM_HAS_OPENGL=0)
endif()

# The native Vulkan backend. Off by default: a console or SDL-only build must
# not acquire a Vulkan SDK dependency, and nothing outside this block refers
# to Vulkan headers or to the loader.
option(SNESRECOMP_PLATFORM_ENABLE_VULKAN
    "Build the native Vulkan presentation backend" OFF)
option(SNESRECOMP_PLATFORM_VULKAN_VALIDATION
    "Request VK_LAYER_KHRONOS_validation at instance creation" OFF)

if(SNESRECOMP_PLATFORM_ENABLE_VULKAN)
    find_package(Vulkan REQUIRED)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/SnesRecompPlatformVulkan.cmake")

    target_sources(snesrecomp_platform PRIVATE
        src/presenter_vulkan.c
        src/vulkan_policy.c)
    target_link_libraries(snesrecomp_platform PRIVATE Vulkan::Vulkan)
    # Public, because a game gates its launcher entry and its configuration
    # on whether this build can select the backend at all.
    target_compile_definitions(snesrecomp_platform PUBLIC
        SNESRECOMP_PLATFORM_HAS_VULKAN=1)
    if(SNESRECOMP_PLATFORM_VULKAN_VALIDATION)
        # Development only. A release build never asks for a layer.
        target_compile_definitions(snesrecomp_platform PRIVATE
            SNESRECOMP_PLATFORM_VULKAN_VALIDATION=1)
    endif()

    snesrecomp_platform_target_vulkan_shaders(snesrecomp_platform
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan")
else()
    target_compile_definitions(snesrecomp_platform PUBLIC
        SNESRECOMP_PLATFORM_HAS_VULKAN=0)
endif()

set_target_properties(snesrecomp_platform PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

# GCC defaults to short enums for arm-*-eabi. Public presenter structures carry
# enums before scalar fields, so every consumer must use the same enum ABI.
if(SNESRECOMP_PLATFORM_REQUIRE_INT_ENUMS)
    target_compile_options(snesrecomp_platform_runtime PUBLIC -fno-short-enums)
    target_compile_options(snesrecomp_platform PUBLIC -fno-short-enums)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cmake/SnesRecompPlatformOverlays.cmake")

function(snesrecomp_platform_target_glsl_presets target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Unknown GLSL preset consumer target: ${target}")
    endif()
    if(NOT SNESRECOMP_PLATFORM_ENABLE_OPENGL)
        message(FATAL_ERROR
            "GLSL presets require SNESRECOMP_PLATFORM_ENABLE_OPENGL")
    endif()
    if(NOT COMMAND snesrecomp_target_glsl_shader)
        message(FATAL_ERROR
            "Include snesrecomp runner.cmake before enabling GLSL presets")
    endif()

    target_sources(${target} PRIVATE
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/glsl_shader_adapter.c")
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/glsl-compat"
        "${SNESRECOMP_PLATFORM_GL_CORE_DIR}")
    snesrecomp_target_glsl_shader(${target})
endfunction()

# Exactly one host-boot implementation is attached to the game target. Console
# adapters supply their own source; desktop builds use the recomp-ui launcher.
function(snesrecomp_platform_target_host_boot target)
    cmake_parse_arguments(HB "" "RECOMP_UI_ROOT" "" ${ARGN})

    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Unknown host boot target: ${target}")
    endif()

    if(SNESRECOMP_PLATFORM_HOST_BOOT_SOURCE)
        target_sources(${target} PRIVATE
            "${SNESRECOMP_PLATFORM_HOST_BOOT_SOURCE}")
        message(STATUS "${target}: host boot = console (no launcher)")
    else()
        if(NOT EXISTS "${HB_RECOMP_UI_ROOT}/src/recomp_launcher.h")
            message(FATAL_ERROR
                "Desktop host boot needs RECOMP_UI_ROOT <recomp-ui source tree>; got '${HB_RECOMP_UI_ROOT}'")
        endif()
        target_sources(${target} PRIVATE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/host_boot_desktop.c")
        target_include_directories(${target} PRIVATE
            "${HB_RECOMP_UI_ROOT}/src")
        message(STATUS "${target}: host boot = desktop (recomp-ui launcher)")
    endif()
endfunction()

option(SNESRECOMP_PLATFORM_BUILD_TESTS
    "Build snesrecomp-platform smoke tests" OFF)

if(SNESRECOMP_PLATFORM_BUILD_TESTS)
    enable_testing()
    add_executable(snesrecomp_platform_runtime_policy_smoke
        tests/runtime_policy_smoke.c)
    target_link_libraries(snesrecomp_platform_runtime_policy_smoke PRIVATE
        snesrecomp::runtime)
    add_test(NAME snesrecomp_platform_runtime_policy_smoke
        COMMAND snesrecomp_platform_runtime_policy_smoke)

    add_executable(snesrecomp_platform_rom_verify_smoke
        tests/rom_verify_smoke.c)
    target_link_libraries(snesrecomp_platform_rom_verify_smoke PRIVATE
        snesrecomp::runtime)
    add_test(NAME snesrecomp_platform_rom_verify_smoke
        COMMAND snesrecomp_platform_rom_verify_smoke)

    add_executable(snesrecomp_platform_smoke
        tests/presenter_smoke.c)
    target_link_libraries(snesrecomp_platform_smoke PRIVATE
        snesrecomp::platform
        SDL3::SDL3)
    if(WIN32 AND TARGET SDL3::SDL3-shared)
        add_custom_command(TARGET snesrecomp_platform_smoke POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:SDL3::SDL3-shared>"
                "$<TARGET_FILE_DIR:snesrecomp_platform_smoke>")
    endif()
    add_test(NAME snesrecomp_platform_smoke
        COMMAND snesrecomp_platform_smoke)

    add_executable(snesrecomp_platform_mode7_smoke
        tests/snes_ppu_mode7_smoke.c)
    target_link_libraries(snesrecomp_platform_mode7_smoke PRIVATE
        snesrecomp::platform)
    add_test(NAME snesrecomp_platform_mode7_smoke
        COMMAND snesrecomp_platform_mode7_smoke)
    set_tests_properties(snesrecomp_platform_smoke PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")
    if(SNESRECOMP_PLATFORM_ENABLE_OPENGL)
        add_test(NAME snesrecomp_platform_opengl_smoke
            COMMAND snesrecomp_platform_smoke opengl)
        set_tests_properties(snesrecomp_platform_opengl_smoke PROPERTIES
            SKIP_RETURN_CODE 77)
    endif()
endif()
