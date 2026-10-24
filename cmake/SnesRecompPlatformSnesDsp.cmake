include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/SnesRecompPlatformApuProgram.cmake")

function(_sr_snes_dsp_replace_source sources_var original replacement includes)
    set(_sources "${${sources_var}}")
    set(_count 0)
    foreach(_source IN LISTS _sources)
        if(_source STREQUAL "${original}")
            math(EXPR _count "${_count} + 1")
        endif()
    endforeach()
    if(NOT _count EQUAL 1)
        message(FATAL_ERROR "SNES DSP overlay requires exactly one original source: ${original}")
    endif()
    list(FIND _sources "${original}" _index)
    list(REMOVE_AT _sources ${_index})
    list(INSERT _sources ${_index} "${replacement}")
    foreach(_prop IN ITEMS COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FLAGS
            INCLUDE_DIRECTORIES LANGUAGE)
        get_source_file_property(_value "${original}" "${_prop}")
        if(NOT _value STREQUAL "NOTFOUND")
            set_property(SOURCE "${replacement}" PROPERTY "${_prop}" "${_value}")
        endif()
    endforeach()
    set_property(SOURCE "${replacement}" APPEND PROPERTY INCLUDE_DIRECTORIES "${includes}")
    set(${sources_var} "${_sources}" PARENT_SCOPE)
endfunction()

function(_sr_snes_dsp_read_checked path expected output_var)
    file(READ "${path}" _source)
    string(REPLACE "\r\n" "\n" _source "${_source}")
    string(SHA256 _hash "${_source}")
    if(NOT _hash STREQUAL "${expected}")
        message(FATAL_ERROR
            "SNES DSP overlay requires its reviewed source; review arithmetic, "
            "output/coherence and save contracts before accepting ${path}: ${_hash}")
    endif()
    set(${output_var} "${_source}" PARENT_SCOPE)
endfunction()

# Call once, AFTER snesrecomp_platform_overlay_apu_program with
# SNESRECOMP_PLATFORM_SNES_DSP_BRIDGE=ON. This owns dsp.c and common_rtl.c;
# the native-program overlay continues to own apu.c. No SDK/SDL dependencies
# are introduced here; link the portable snesrecomp DSP target separately.
function(snesrecomp_platform_overlay_snes_dsp sources_var snesrecomp_root)
    if(NOT SNESRECOMP_PLATFORM_SNES_DSP_BRIDGE)
        message(FATAL_ERROR "Set SNESRECOMP_PLATFORM_SNES_DSP_BRIDGE before the APU program overlay")
    endif()
    set(_sources "${${sources_var}}")
    set(_apu "${CMAKE_BINARY_DIR}/generated/apu-program/apu.c")
    list(FIND _sources "${_apu}" _apu_index)
    if(_apu_index EQUAL -1)
        message(FATAL_ERROR "Compose the reviewed native-program APU overlay before the DSP overlay")
    endif()
    file(READ "${_apu}" _apu_text)
    if(NOT _apu_text MATCHES "snesrecomp_dsp_bridge_before_saveload")
        message(FATAL_ERROR "APU overlay was generated without DSP ARAM coherence hooks")
    endif()

    set(_platform "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    set(_directory "${CMAKE_BINARY_DIR}/generated/snes-dsp")
    file(MAKE_DIRECTORY "${_directory}")
    set(_dsp "${snesrecomp_root}/runner/src/snes/dsp.c")
    _sr_snes_dsp_read_checked("${_dsp}"
        "a8bb2d6ffcf661676eaa576e0477333689b0853270cdcc558babd00567c86874" _text)
    set(_prefix "")
    set(_suffix "")
    foreach(_name IN ITEMS init free reset cycle read write saveload available
            getSamples trimSamples output_ring_save output_ring_restore
            output_ring_set_write output_ring_write)
        string(APPEND _prefix "#define dsp_${_name} sr_reference_dsp_${_name}\n")
        string(APPEND _suffix "#undef dsp_${_name}\n")
    endforeach()
    string(APPEND _suffix "#include \"${_platform}/src/snes_dsp_bridge.inc\"\n")
    set(_overlay "${_directory}/dsp.c")
    file(WRITE "${_overlay}" "${_prefix}\n${_text}\n${_suffix}")
    set(_includes "${snesrecomp_root}/runner/src/snes;${snesrecomp_root}/runner/src;${_platform}/include;${_platform}/src")
    _sr_snes_dsp_replace_source(_sources "${_dsp}" "${_overlay}" "${_includes}")

    # The HLE uploader is the only direct ARAM writer outside the native
    # driver/core APU in the pinned framework. Split 16-bit wrapping writes.
    set(_rtl "${snesrecomp_root}/runner/src/common_rtl.c")
    _sr_snes_dsp_read_checked("${_rtl}"
        "c6ca644bebeba1890c9ef098f4c1594fa8463020c4c6cab313569b1785af8f4c" _text)
    _snesrecomp_apu_program_edit(_text
        "    for (uint16_t i = 0; i < n; i++)\n      g_snes->apu->ram[(uint16_t)(target + i)] = p[i];\n"
        "    for (uint16_t i = 0; i < n; i++)\n      g_snes->apu->ram[(uint16_t)(target + i)] = p[i];\n    {\n      uint32_t first = 65536u - target;\n      if (first > n) first = n;\n      snesrecomp_dsp_bridge_clock(g_snes->apu->dsp, g_snes->apu->cycles);\n      snesrecomp_dsp_bridge_aram_written(g_snes->apu->dsp, target, first);\n      if (n > first)\n        snesrecomp_dsp_bridge_aram_written(g_snes->apu->dsp, 0, n - first);\n    }\n")
    set(_rtl_overlay "${_directory}/common_rtl.c")
    file(WRITE "${_rtl_overlay}"
        "#include \"snesrecomp_platform/snes_dsp_bridge.h\"\n${_text}")
    _sr_snes_dsp_replace_source(_sources "${_rtl}" "${_rtl_overlay}" "${_includes}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_dsp}" "${_rtl}" "${_platform}/src/snes_dsp_bridge.inc"
        "${_platform}/src/snes_dsp_bridge_state.inc"
        "${_platform}/include/snesrecomp_platform/snes_dsp_bridge.h")
    set(${sources_var} "${_sources}" PARENT_SCOPE)
endfunction()
