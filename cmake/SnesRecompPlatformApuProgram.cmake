include_guard(GLOBAL)

# Replaces an anchor that must occur exactly once.
function(_snesrecomp_apu_program_edit text_var old new)
    set(_text "${${text_var}}")
    string(REPLACE "${old}" "" _removed "${_text}")
    string(LENGTH "${_text}" _before)
    string(LENGTH "${_removed}" _after)
    string(LENGTH "${old}" _length)
    math(EXPR _delta "${_before} - ${_after}")
    if(NOT _delta EQUAL _length)
        message(FATAL_ERROR "APU program overlay anchor must occur exactly once: ${old}")
    endif()
    string(REPLACE "${old}" "${new}" _text "${_text}")
    set(${text_var} "${_text}" PARENT_SCOPE)
endfunction()

# Lets a native program replace the interpreted SPC-700 (see
# include/snesrecomp_platform/apu_program.h). The APU clock, port queue,
# timers and S-DSP of the pinned core stay; the fetched tree is not modified.
#
# Owns apu.c: no other overlay may replace apu.c in the same source list.
function(snesrecomp_platform_overlay_apu_program sources_var snesrecomp_root)
    set(_original "${snesrecomp_root}/runner/src/snes/apu.c")
    set(_sources "${${sources_var}}")
    list(FIND _sources "${_original}" _index)
    if(_index EQUAL -1)
        message(FATAL_ERROR "The APU program overlay expects the pinned apu.c exactly once")
    endif()

    file(READ "${_original}" _text)
    string(REPLACE "\r\n" "\n" _text "${_text}")
    string(SHA256 _hash "${_text}")
    if(NOT _hash STREQUAL "0f2838934ae5830c68dcbe5a63948168325a2c88142b8abc38ae9379fa8b4402")
        message(FATAL_ERROR
            "The APU program overlay requires the reviewed apu.c; review the "
            "cycle and port contract, then accept ${_hash}")
    endif()

    set(_platform "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    _snesrecomp_apu_program_edit(_text
        "#include \"../audio_trace.h\"\n"
        "#include \"../audio_trace.h\"\n\nstatic bool apu_program_cycle(Apu *apu);\nstatic bool apu_program_take_over(Apu *apu);\nstatic void apu_program_poke(void);\nstatic void apu_program_saveload(Apu *apu, SaveLoadInfo *sli);\n")
    _snesrecomp_apu_program_edit(_text
        "  spc_saveload(apu->spc, sli);\n}\n"
        "  spc_saveload(apu->spc, sli);\n  apu_program_saveload(apu, sli);\n}\n")
    _snesrecomp_apu_program_edit(_text
        "  apu->inPorts[port] = val;\n  audio_trace_on_cpu_port_apply(port, val);\n"
        "  apu->inPorts[port] = val;\n  audio_trace_on_cpu_port_apply(port, val);\n  apu_program_poke();\n")
    _snesrecomp_apu_program_edit(_text
        "void apu_cycle(Apu* apu) {\n  apu_drainPortQueue(apu);\n"
        "void apu_cycle(Apu* apu) {\n  if (apu_program_cycle(apu))\n    return;\n  apu_drainPortQueue(apu);\n")
    _snesrecomp_apu_program_edit(_text
        "    apu->cpuCyclesLeft = spc_runOpcode(apu->spc);\n"
        "    apu->cpuCyclesLeft = apu_program_take_over(apu) ? 1 : spc_runOpcode(apu->spc);\n")
    string(APPEND _text "\n#include \"${_platform}/src/apu_program.inc\"\n")

    set(_directory "${CMAKE_BINARY_DIR}/generated/apu-program")
    set(_overlay "${_directory}/apu.c")
    file(MAKE_DIRECTORY "${_directory}")
    file(WRITE "${_overlay}" "${_text}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_original}" "${_platform}/src/apu_program.inc")

    list(REMOVE_AT _sources ${_index})
    list(INSERT _sources ${_index} "${_overlay}")
    foreach(_prop IN ITEMS COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FLAGS
            INCLUDE_DIRECTORIES LANGUAGE)
        get_source_file_property(_value "${_original}" "${_prop}")
        if(NOT _value STREQUAL "NOTFOUND")
            set_property(SOURCE "${_overlay}" PROPERTY "${_prop}" "${_value}")
        endif()
    endforeach()
    set_property(SOURCE "${_overlay}" APPEND PROPERTY INCLUDE_DIRECTORIES
        "${snesrecomp_root}/runner/src/snes;${snesrecomp_root}/runner/src;${_platform}/include")
    set(${sources_var} "${_sources}" PARENT_SCOPE)
endfunction()
