include_guard(GLOBAL)

# Avoid a 64-bit division for the overwhelmingly common sub-scanline update.
# The unchanged fallback handles line crossings, rollback, teleports and wrap.
function(snesrecomp_platform_overlay_refresh_clock sources_var)
    set(_result)
    set(_count 0)
    foreach(_input IN LISTS ${sources_var})
        get_filename_component(_name "${_input}" NAME)
        if(NOT _name STREQUAL "common_cpu_infra.c")
            list(APPEND _result "${_input}")
            continue()
        endif()
        file(READ "${_input}" _text)
        string(REPLACE "\r\n" "\n" _text "${_text}")
        string(SHA256 _hash "${_text}")
        if(NOT _hash STREQUAL "63409584bed99bb1efe1c166760ccd3fff117f0d0d61cb11f210a85bb52d25be")
            message(FATAL_ERROR "Review refresh-clock core contract at ${_input}: ${_hash}")
        endif()
        set(_old [=[  s_refresh_phase += delta;
  uint64_t lines = s_refresh_phase / 1364u;]=])
        set(_new [=[  s_refresh_phase += delta;
  if (s_refresh_phase < 1364u) {
    g_refresh_charged_upto = g_cpu.master_cycles;
    return;
  }
  uint64_t lines = s_refresh_phase / 1364u;]=])
        string(FIND "${_text}" "${_old}" _offset)
        if(_offset EQUAL -1)
            message(FATAL_ERROR "Missing reviewed refresh-clock seam")
        endif()
        string(REPLACE "${_old}" "${_new}" _text "${_text}")
        set(_output "${CMAKE_BINARY_DIR}/generated/refresh-clock/common_cpu_infra.c")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/refresh-clock")
        file(WRITE "${_output}" "${_text}")
        foreach(_prop COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FLAGS INCLUDE_DIRECTORIES LANGUAGE)
            get_source_file_property(_value "${_input}" "${_prop}")
            if(NOT _value STREQUAL "NOTFOUND")
                set_property(SOURCE "${_output}" PROPERTY "${_prop}" "${_value}")
            endif()
        endforeach()
        list(APPEND _result "${_output}")
        math(EXPR _count "${_count} + 1")
    endforeach()
    if(NOT _count EQUAL 1)
        message(FATAL_ERROR "Refresh-clock overlay expected exactly one CPU infra source")
    endif()
    set(${sources_var} "${_result}" PARENT_SCOPE)
endfunction()
