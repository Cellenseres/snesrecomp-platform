include_guard(GLOBAL)

# Vulkan shader build: tracked GLSL 450 sources become SPIR-V in the build
# tree and are embedded as generated C. Nothing is compiled at runtime, and no
# generated binary is tracked.

set(SNESRECOMP_PLATFORM_GLSLC "" CACHE FILEPATH
    "Path to glslc. Empty searches the Vulkan SDK and then PATH.")

function(snesrecomp_platform_find_glslc out_var)
    if(SNESRECOMP_PLATFORM_GLSLC)
        if(NOT EXISTS "${SNESRECOMP_PLATFORM_GLSLC}")
            message(FATAL_ERROR
                "SNESRECOMP_PLATFORM_GLSLC is set to a file that does not "
                "exist: ${SNESRECOMP_PLATFORM_GLSLC}")
        endif()
        set(${out_var} "${SNESRECOMP_PLATFORM_GLSLC}" PARENT_SCOPE)
        return()
    endif()

    # Vulkan::glslc appears when find_package(Vulkan) locates an SDK that
    # ships it, which is the common case on Windows and on a full Linux SDK.
    if(TARGET Vulkan::glslc)
        set(${out_var} "$<TARGET_FILE:Vulkan::glslc>" PARENT_SCOPE)
        return()
    endif()

    set(_hints "")
    if(DEFINED ENV{VULKAN_SDK})
        list(APPEND _hints "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
    endif()
    if(Vulkan_INCLUDE_DIR)
        get_filename_component(_sdk "${Vulkan_INCLUDE_DIR}" DIRECTORY)
        list(APPEND _hints "${_sdk}/bin" "${_sdk}/Bin")
    endif()

    find_program(SNESRECOMP_PLATFORM_GLSLC_FOUND
        NAMES glslc glslc.exe
        HINTS ${_hints})
    if(NOT SNESRECOMP_PLATFORM_GLSLC_FOUND)
        message(FATAL_ERROR
            "SNESRECOMP_PLATFORM_ENABLE_VULKAN needs glslc to compile the "
            "Vulkan shaders. Install the Vulkan SDK, or point "
            "SNESRECOMP_PLATFORM_GLSLC at a glslc binary.")
    endif()
    set(${out_var} "${SNESRECOMP_PLATFORM_GLSLC_FOUND}" PARENT_SCOPE)
endfunction()

# Compiles every Vulkan shader and attaches the generated C to `target`.
function(snesrecomp_platform_target_vulkan_shaders target shader_dir)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Unknown Vulkan shader target: ${target}")
    endif()

    snesrecomp_platform_find_glslc(_glslc)

    set(_shaders
        blit.vert blit.frag
        overlay.vert overlay.frag
        mode7.vert mode7.frag
        sharp_bilinear_prescale.frag sharp_bilinear_resolve.frag)

    set(_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/vulkan")
    file(MAKE_DIRECTORY "${_generated_dir}")

    set(_spv_files "")
    set(_symbol_names "")
    foreach(_shader IN LISTS _shaders)
        set(_source "${shader_dir}/${_shader}")
        if(NOT EXISTS "${_source}")
            message(FATAL_ERROR "Missing Vulkan shader source: ${_source}")
        endif()
        string(REPLACE "." "_" _symbol "${_shader}")
        set(_spv "${_generated_dir}/${_symbol}.spv")

        add_custom_command(
            OUTPUT "${_spv}"
            # No -O. glslc's -O runs spirv-opt, which access-violates on
            # mode7.frag (Vulkan SDK 1.4.357.0); the same shader compiles
            # cleanly without it. The optimiser buys module size, not
            # correctness or speed -- every driver optimises the SPIR-V it is
            # given -- so dropping it costs nothing worth having.
            COMMAND "${_glslc}"
                    --target-env=vulkan1.0
                    -o "${_spv}"
                    "${_source}"
            DEPENDS "${_source}"
            COMMENT "Compiling Vulkan shader ${_shader}"
            VERBATIM)

        list(APPEND _spv_files "${_spv}")
        list(APPEND _symbol_names "${_symbol}")
    endforeach()

    set(_generated_c "${_generated_dir}/vulkan_shaders.c")
    set(_generated_h "${_generated_dir}/vulkan_shaders.h")

    add_custom_command(
        OUTPUT "${_generated_c}" "${_generated_h}"
        COMMAND "${CMAKE_COMMAND}"
                "-DEMBED_INPUTS=${_spv_files}"
                "-DEMBED_NAMES=${_symbol_names}"
                "-DEMBED_OUTPUT_C=${_generated_c}"
                "-DEMBED_OUTPUT_H=${_generated_h}"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedBinary.cmake"
        DEPENDS ${_spv_files}
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedBinary.cmake"
        COMMENT "Embedding Vulkan SPIR-V modules"
        VERBATIM)

    target_sources(${target} PRIVATE "${_generated_c}" "${_generated_h}")
    target_include_directories(${target} PRIVATE "${_generated_dir}")
    set_source_files_properties("${_generated_h}" PROPERTIES
        GENERATED TRUE HEADER_FILE_ONLY TRUE)
endfunction()
