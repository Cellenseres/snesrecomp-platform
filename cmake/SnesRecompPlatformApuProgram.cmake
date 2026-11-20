include_guard(GLOBAL)
option(SNESRECOMP_PLATFORM_APU_CPU_SPANS "Experimental certified CPU-only APU spans" OFF)

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

# Batches idle cycles in snes_catchupApu's relative catch-up loop.
# Applies only after the APU program overlay; the caller owns snes.c.
function(snesrecomp_platform_patch_apu_catchup text_var)
    get_property(_overlay GLOBAL PROPERTY SNESRECOMP_PLATFORM_APU_PROGRAM_OVERLAY)
    if(NOT _overlay)
        return()
    endif()
    set(_text "${${text_var}}")
    _snesrecomp_apu_program_edit(_text
        "  for(int i = 0; i < catchupCycles; i++) {\n    apu_cycle(snes->apu);\n  }\n"
        "  extern uint32_t snesrecomp_apu_program_skip(Apu *apu, uint32_t cycles);\n  for(int i = 0; i < catchupCycles;) {\n    const uint32_t idle = catchupCycles - i > 1 ?\n        snesrecomp_apu_program_skip(snes->apu, (uint32_t)(catchupCycles - i)) : 0;\n    if (idle) {\n      i += (int)idle;\n      continue;\n    }\n    apu_cycle(snes->apu);\n    i++;\n  }\n")
    _snesrecomp_apu_program_edit(_text
        "  int catchupCycles = (int) snes->apuCatchupCycles;\n  if (catchupCycles < 0) catchupCycles = 0;\n"
        "  int catchupCycles = (int) snes->apuCatchupCycles;\n  if (catchupCycles < 0) catchupCycles = 0;\n#ifndef SNESRECOMP_INTERP_PROFILE\n  /* No cycle, event or sample is due for this fractional credit. Preserve\n   * the producer's final state and call counter without idle discovery. */\n  if (!catchupCycles && snes->apuCatchupCycles >= 0.0) {\n    audio_trace_set_producer(AUDIO_TRACE_PRODUCER_UNKNOWN);\n    ++s_catchup_calls;\n    return;\n  }\n#endif\n")
    if(SNESRECOMP_PLATFORM_APU_CPU_SPANS)
        _snesrecomp_apu_program_edit(_text
            "  for(int i = 0; i < catchupCycles;) {\n"
            "  extern uint32_t snesrecomp_apu_cpu_span_defer_cycles(Apu *,uint32_t);\n  const uint32_t deferred=snesrecomp_apu_cpu_span_defer_cycles(snes->apu,(uint32_t)catchupCycles);\n  for(int i = (int)deferred; i < catchupCycles;) {\n")
    endif()
    set(${text_var} "${_text}" PARENT_SCOPE)
endfunction()

# Compose a getter into the currently selected trace owner. No assumed producer
# value is used when deferred synthesis temporarily enters the CPU producer.
function(_snesrecomp_apu_span_trace sources_var)
    set(_result "")
    set(_count 0)
    foreach(_input IN LISTS ${sources_var})
        get_filename_component(_name "${_input}" NAME)
        if(NOT _name STREQUAL "audio_trace.c")
            list(APPEND _result "${_input}")
            continue()
        endif()
        file(READ "${_input}" _text)
        string(REPLACE "\r\n" "\n" _text "${_text}")
        _snesrecomp_apu_program_edit(_text
            "void audio_trace_set_producer(int producer) {\n  s_producer = producer;\n}"
            "void audio_trace_set_producer(int producer) {\n  s_producer = producer;\n}\nint snesrecomp_apu_span_trace_producer(void) { return s_producer; }")
        set(_output "${CMAKE_BINARY_DIR}/generated/apu-cpu-span/audio_trace.c")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/apu-cpu-span")
        file(WRITE "${_output}" "${_text}")
        foreach(_prop COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FLAGS INCLUDE_DIRECTORIES LANGUAGE)
            get_source_file_property(_value "${_input}" "${_prop}")
            if(NOT _value STREQUAL "NOTFOUND")
                set_property(SOURCE "${_output}" PROPERTY "${_prop}" "${_value}")
            endif()
        endforeach()
        get_filename_component(_include "${_input}" DIRECTORY)
        set_property(SOURCE "${_output}" APPEND PROPERTY INCLUDE_DIRECTORIES "${_include}")
        list(APPEND _result "${_output}")
        math(EXPR _count "${_count}+1")
    endforeach()
    if(NOT _count EQUAL 1)
        message(FATAL_ERROR "APU CPU spans require one reviewed audio trace owner")
    endif()
    set(${sources_var} "${_result}" PARENT_SCOPE)
endfunction()

# Lets a native program replace the interpreted SPC-700 (see
# include/snesrecomp_platform/apu_program.h). The APU clock, port queue,
# timers and S-DSP of the pinned core stay; the fetched tree is not modified.
#
# Owns apu.c: no other overlay may replace apu.c in the same source list.
function(snesrecomp_platform_overlay_apu_program sources_var snesrecomp_root)
    set_property(GLOBAL PROPERTY SNESRECOMP_PLATFORM_APU_PROGRAM_OVERLAY ON)
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
        "#include \"../audio_trace.h\"\n\nstatic bool apu_program_cycle(Apu *apu);\nstatic bool apu_program_take_over(Apu *apu);\nstatic void apu_program_poke(void);\nstatic void apu_program_saveload(Apu *apu, SaveLoadInfo *sli);\nstatic uint32_t apu_program_skip(Apu *apu, uint64_t target, uint32_t max_cycles);\n")
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
    _snesrecomp_apu_program_edit(_text
        "    if (apu->portClock >= target && !due_event)\n      return true;\n    if (max_cycles-- == 0)\n      return false;\n    apu_cycle(apu);\n"
        "    if (apu->portClock >= target && !due_event)\n      return true;\n    if (max_cycles == 0)\n      return false;\n    /* A one-cycle request cannot amortize idle-span discovery. The exact\n     * cycle routine already handles its ports, counters, native program and\n     * DSP boundary. Larger requests retain the certified idle batching. */\n    if (max_cycles > 1u && target > apu->portClock && target - apu->portClock > 1u) {\n      const uint32_t idle = apu_program_skip(apu, target, max_cycles);\n      max_cycles -= idle;\n      if (idle)\n        continue;\n    }\n    max_cycles--;\n    apu_cycle(apu);\n")

    # Compose audio coherence into this sole owner of apu.c. Never apply a
    # second APU overlay to the already-generated translation unit. The
    # default native-SPC/CPU synthesis build remains byte-for-byte unchanged.
    if(SNESRECOMP_PLATFORM_SNES_DSP_BRIDGE)
        _snesrecomp_apu_program_edit(_text
            "#include \"../audio_trace.h\"\n"
            "#include \"../audio_trace.h\"\n#include \"snesrecomp_platform/snes_dsp_bridge.h\"\n")
        _snesrecomp_apu_program_edit(_text
            "void apu_saveload(Apu *apu, SaveLoadInfo *sli) {\n"
            "void apu_saveload(Apu *apu, SaveLoadInfo *sli) {\n  snesrecomp_dsp_bridge_before_saveload(apu->dsp);\n")
        _snesrecomp_apu_program_edit(_text
            "  return apu->ram[adr];\n"
            "  snesrecomp_dsp_bridge_before_read(apu->dsp);\n  return apu->ram[adr];\n")
        _snesrecomp_apu_program_edit(_text
            "  apu->ram[adr] = val;\n"
            "  apu->ram[adr] = val;\n  snesrecomp_dsp_bridge_aram_written(apu->dsp, adr, 1);\n")
    endif()
    if(SNESRECOMP_PLATFORM_APU_CPU_SPANS)
        _snesrecomp_apu_program_edit(_text "#include \"../audio_trace.h\"\n"
            "#include \"../audio_trace.h\"\n#define SNESRECOMP_PLATFORM_APU_CPU_SPANS 1\nstatic void apu_program_span_before_access(const Apu *);\nstatic bool apu_program_span_target(Apu *,uint64_t,uint32_t);\n")
        # Every public access which can expose or replace deferred state ends
        # it BEFORE touching ports, RAM, timers, saved state or native callbacks.
        foreach(_entry IN ITEMS
            "void apu_free(Apu* apu) {"
            "void apu_reset(Apu* apu) {"
            "void apu_clearPortQueue(Apu* apu) {"
            "void apu_writePortNow(Apu* apu, uint8_t port, uint8_t val) {"
            "uint32_t apu_portQueueDepth(const Apu* apu) {"
            "bool apu_runUntilPortQueueEmpty(Apu* apu, uint32_t max_cycles) {"
            "void apu_port_sched_save(const Apu *apu, ApuPortSched *out) {"
            "void apu_port_sched_restore(Apu *apu, const ApuPortSched *in) {"
            "void apu_saveload(Apu *apu, SaveLoadInfo *sli) {"
            "void apu_cycle(Apu* apu) {"
            "uint8_t apu_cpuRead(Apu* apu, uint16_t adr) {"
            "void apu_cpuWrite(Apu* apu, uint16_t adr, uint8_t val) {")
            _snesrecomp_apu_program_edit(_text "${_entry}\n"
                "${_entry}\n  apu_program_span_before_access(apu);\n")
        endforeach()
        _snesrecomp_apu_program_edit(_text
            "                           uint64_t guest_cycle) {\n"
            "                           uint64_t guest_cycle) {\n  apu_program_span_before_access(apu);\n")
        _snesrecomp_apu_program_edit(_text
            "                              uint8_t request_value, uint32_t max_cycles) {\n"
            "                              uint8_t request_value, uint32_t max_cycles) {\n  apu_program_span_before_access(apu);\n")
        _snesrecomp_apu_program_edit(_text
            "                           uint32_t max_cycles) {\n  apu_writePortNow"
            "                           uint32_t max_cycles) {\n  apu_program_span_before_access(apu);\n  apu_writePortNow")
        _snesrecomp_apu_program_edit(_text
            "  if (target < apu->portLastTarget)\n    target = apu->portLastTarget;\n\n  for (;;) {"
            "  if (target < apu->portLastTarget)\n    target = apu->portLastTarget;\n  if (apu_program_span_target(apu,target,max_cycles)) return true;\n\n  for (;;) {")
    endif()
    string(APPEND _text "\n#include \"${_platform}/src/apu_program.inc\"\n")

    set(_directory "${CMAKE_BINARY_DIR}/generated/apu-program")
    set(_overlay "${_directory}/apu.c")
    file(MAKE_DIRECTORY "${_directory}")
    file(WRITE "${_overlay}" "${_text}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_original}" "${_platform}/src/apu_program.inc" "${_platform}/src/apu_cpu_span.inc")

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
    if(SNESRECOMP_PLATFORM_APU_CPU_SPANS)
        _snesrecomp_apu_span_trace(_sources)
    endif()
    set(${sources_var} "${_sources}" PARENT_SCOPE)
endfunction()
