cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(snesrecomp_platform C)
endif()

option(SNESRECOMP_PLATFORM_ENABLE_SDL "Build SDL presentation and workers" ON)
if(SNESRECOMP_PLATFORM_ENABLE_SDL AND NOT TARGET SDL3::SDL3)
    find_package(SDL3 CONFIG REQUIRED)
endif()

add_library(snesrecomp_platform_runtime STATIC
    src/runtime_policy.c
    src/sha1.c
    src/rom_verify.c
    src/msu_pack.c
    src/margin_pack.c
)
add_library(snesrecomp::runtime ALIAS snesrecomp_platform_runtime)
target_compile_features(snesrecomp_platform_runtime PUBLIC c_std_11)
if(NOT WIN32)
    target_compile_definitions(snesrecomp_platform_runtime PRIVATE _POSIX_C_SOURCE=200809L)
endif()
target_include_directories(snesrecomp_platform_runtime
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
set_target_properties(snesrecomp_platform_runtime PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

add_library(snesrecomp_ppu STATIC
    # SNES semantics shared by reference renderers and native GPU backends.
    # These sources contain no console SDK or game-specific code.
    src/snes_ppu_capture.c
    src/snes_bg_continuity.c
    src/snes_obj_continuity.c
    src/snes_ppu_mode7.c
    src/snes_ppu_mode7_world.c
    src/snes_tiles.c
    src/snes_bitplanes.c
    src/snes_ppu_index_row.c
    src/snes_ppu_source_compact.c
    src/snes_obj_ranges.c
    src/snes_bg_upload.c
    src/snes_bg_strips.c
    src/snes_ppu_semantics.c
    src/snes_ppu_semantic_gpu.c
    src/snes_ppu_obj.c
    src/snes_ppu_obj_run.c
    src/snes_ppu_trace.c
    src/snes_ppu_frame.c
    src/snes_ppu_job.c
    src/snes_ppu_primitive_colour.c
    src/snes_ppu_index_delta.c
    src/snes_ppu_repeat_axis.c
    src/snes_ppu_indexed_projection.c
    src/snes_ppu_indexed_draw.c
    src/snes_ppu_indexed_palette.c
    src/snes_ppu_image_certificate.c
    src/snes_ppu_indexed_brightness.c
    src/snes_ppu_tile_plan.c
    src/snes_ppu_raw_plan.c
    src/snes_ppu_obj_geometry.c
    src/ppu_blit.c
    src/image_delta.c
)
add_library(snesrecomp::ppu ALIAS snesrecomp_ppu)
target_compile_features(snesrecomp_ppu PUBLIC c_std_11)
target_include_directories(snesrecomp_ppu PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
set_target_properties(snesrecomp_ppu PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

add_library(snesrecomp_platform STATIC
    src/presenter.c src/presenter_ppu.c
    src/frame_overscan.c src/present_timeline.c)
target_link_libraries(snesrecomp_platform PUBLIC snesrecomp::ppu)
if(SNESRECOMP_PLATFORM_ENABLE_SDL)
    target_sources(snesrecomp_platform PRIVATE src/presenter_sdl.c src/task.c)
    target_link_libraries(snesrecomp_platform PRIVATE SDL3::SDL3)
    target_compile_definitions(snesrecomp_platform PRIVATE SNESRECOMP_PLATFORM_HAS_SDL=1)
else()
    if(SNESRECOMP_PLATFORM_NATIVE_TASK_SOURCE)
        target_sources(snesrecomp_platform PRIVATE ${SNESRECOMP_PLATFORM_NATIVE_TASK_SOURCE})
    else()
        target_sources(snesrecomp_platform PRIVATE src/task_serial.c)
    endif()
    target_compile_definitions(snesrecomp_platform PRIVATE SNESRECOMP_PLATFORM_HAS_SDL=0)
endif()

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
        SNESRECOMP_PLATFORM_HAS_NATIVE_PRESENTER=1)
else()
    target_compile_definitions(snesrecomp_platform PUBLIC
        SNESRECOMP_PLATFORM_HAS_NATIVE_PRESENTER=0)
endif()
if(NOT DEFINED SNESRECOMP_PLATFORM_NATIVE_GPU_BG)
    if(SNESRECOMP_PLATFORM_NATIVE_PRESENTER_SOURCES)
        set(SNESRECOMP_PLATFORM_NATIVE_GPU_BG ON)
    else()
        set(SNESRECOMP_PLATFORM_NATIVE_GPU_BG OFF)
    endif()
endif()
target_compile_definitions(snesrecomp_platform PUBLIC
    SNESRECOMP_PLATFORM_HAS_GPU_BG=$<BOOL:${SNESRECOMP_PLATFORM_NATIVE_GPU_BG}>)

add_library(snesrecomp::platform ALIAS snesrecomp_platform)

target_compile_features(snesrecomp_platform PUBLIC c_std_11)

target_include_directories(snesrecomp_platform
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

option(SNESRECOMP_PLATFORM_ENABLE_OPENGL
    "Build the OpenGL 3.3 presentation backend" OFF)
set(SNESRECOMP_PLATFORM_GL_CORE_DIR "" CACHE PATH
    "Directory containing gl_core_3_1.c and gl_core_3_1.h")

if(SNESRECOMP_PLATFORM_ENABLE_OPENGL)
    if(NOT SNESRECOMP_PLATFORM_ENABLE_SDL)
        message(FATAL_ERROR "The desktop OpenGL backend requires SDL")
    endif()
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
    if(NOT SNESRECOMP_PLATFORM_ENABLE_SDL)
        message(FATAL_ERROR "The desktop Vulkan backend requires SDL")
    endif()
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
    target_compile_options(snesrecomp_ppu PUBLIC -fno-short-enums)
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

    add_executable(snesrecomp_platform_margin_pack_smoke
        tests/margin_pack_smoke.c)
    target_link_libraries(snesrecomp_platform_margin_pack_smoke PRIVATE
        snesrecomp::runtime)
    add_test(NAME snesrecomp_platform_margin_pack_smoke
        COMMAND snesrecomp_platform_margin_pack_smoke)

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

    add_executable(snesrecomp_platform_vsync_smoke
        tests/presenter_vsync_smoke.c)
    target_link_libraries(snesrecomp_platform_vsync_smoke PRIVATE
        snesrecomp::platform SDL3::SDL3)
    if(WIN32 AND TARGET SDL3::SDL3-shared)
        add_custom_command(TARGET snesrecomp_platform_vsync_smoke POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:SDL3::SDL3-shared>"
                "$<TARGET_FILE_DIR:snesrecomp_platform_vsync_smoke>")
    endif()
    add_test(NAME snesrecomp_platform_vsync_software
        COMMAND snesrecomp_platform_vsync_smoke software)
    set_tests_properties(snesrecomp_platform_vsync_software PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy" SKIP_RETURN_CODE 77)
    add_test(NAME snesrecomp_platform_vsync_sdl
        COMMAND snesrecomp_platform_vsync_smoke sdl)
    set_tests_properties(snesrecomp_platform_vsync_sdl PROPERTIES
        SKIP_RETURN_CODE 77)
    if(SNESRECOMP_PLATFORM_ENABLE_OPENGL)
        add_test(NAME snesrecomp_platform_vsync_opengl
            COMMAND snesrecomp_platform_vsync_smoke opengl)
        set_tests_properties(snesrecomp_platform_vsync_opengl PROPERTIES
            SKIP_RETURN_CODE 77)
    endif()
    if(SNESRECOMP_PLATFORM_ENABLE_VULKAN)
        add_test(NAME snesrecomp_platform_vsync_vulkan
            COMMAND snesrecomp_platform_vsync_smoke vulkan)
        set_tests_properties(snesrecomp_platform_vsync_vulkan PROPERTIES
            SKIP_RETURN_CODE 77)
    endif()

    # Measures presentation pacing only; not registered as a test because it
    # reports timings rather than passing or failing.
    add_executable(snesrecomp_platform_presenter_bench
        tests/presenter_bench.c)
    target_link_libraries(snesrecomp_platform_presenter_bench PRIVATE
        snesrecomp::platform
        SDL3::SDL3)
    if(WIN32 AND TARGET SDL3::SDL3-shared)
        add_custom_command(TARGET snesrecomp_platform_presenter_bench
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:SDL3::SDL3-shared>"
                "$<TARGET_FILE_DIR:snesrecomp_platform_presenter_bench>")
    endif()

    add_executable(snesrecomp_platform_present_timeline_smoke
        tests/present_timeline_smoke.c)
    target_link_libraries(snesrecomp_platform_present_timeline_smoke
        PRIVATE snesrecomp::platform)
    add_test(NAME snesrecomp_platform_present_timeline_smoke
        COMMAND snesrecomp_platform_present_timeline_smoke)

    add_executable(snesrecomp_platform_obj_continuity_smoke
        tests/snes_obj_continuity_smoke.c)
    target_link_libraries(snesrecomp_platform_obj_continuity_smoke
        PRIVATE snesrecomp::platform)
    add_test(NAME snesrecomp_platform_obj_continuity_smoke
        COMMAND snesrecomp_platform_obj_continuity_smoke)

    add_executable(snesrecomp_platform_bg_continuity_smoke
        tests/snes_bg_continuity_smoke.c)
    target_link_libraries(snesrecomp_platform_bg_continuity_smoke
        PRIVATE snesrecomp::platform)
    add_test(NAME snesrecomp_platform_bg_continuity_smoke
        COMMAND snesrecomp_platform_bg_continuity_smoke)

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
    if(SNESRECOMP_PLATFORM_ENABLE_VULKAN)
        # Pure selection policy: no instance, no device, no window.
        # vulkan_policy.c already lives in snesrecomp::platform; compiling it
        # again here would define the same symbols twice.
        add_executable(snesrecomp_platform_vulkan_policy_smoke
            tests/vulkan_policy_smoke.c)
        target_include_directories(
            snesrecomp_platform_vulkan_policy_smoke PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(snesrecomp_platform_vulkan_policy_smoke PRIVATE
            snesrecomp::platform
            Vulkan::Vulkan)
        add_test(NAME snesrecomp_platform_vulkan_policy_smoke
            COMMAND snesrecomp_platform_vulkan_policy_smoke)

        # Needs a real device, so it reports 77 where none is usable.
        add_test(NAME snesrecomp_platform_vulkan_smoke
            COMMAND snesrecomp_platform_smoke vulkan)
        set_tests_properties(snesrecomp_platform_vulkan_smoke PROPERTIES
            SKIP_RETURN_CODE 77)
    endif()
endif()
