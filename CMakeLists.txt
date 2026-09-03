cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(snesrecomp_platform C)
endif()

if(NOT TARGET SDL3::SDL3)
    find_package(SDL3 CONFIG REQUIRED)
endif()

add_library(snesrecomp_platform_runtime STATIC
    src/runtime_policy.c
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
)

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

set_target_properties(snesrecomp_platform PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

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
    set_tests_properties(snesrecomp_platform_smoke PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")
    if(SNESRECOMP_PLATFORM_ENABLE_OPENGL)
        add_test(NAME snesrecomp_platform_opengl_smoke
            COMMAND snesrecomp_platform_smoke opengl)
        set_tests_properties(snesrecomp_platform_opengl_smoke PROPERTIES
            SKIP_RETURN_CODE 77)
    endif()
endif()
