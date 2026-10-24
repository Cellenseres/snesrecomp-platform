include_guard(GLOBAL)

# Exact candidate filter for the pinned scheduler state ring. No game PCs,
# memory reads, device synchronization or yielding rules are changed.
function(snesrecomp_platform_overlay_quiescence sources_var)
    set(_result)
    set(_count 0)
    foreach(_input IN LISTS ${sources_var})
        get_filename_component(_name "${_input}" NAME)
        if(NOT _name STREQUAL "interp_bridge.c")
            list(APPEND _result "${_input}")
            continue()
        endif()
        file(READ "${_input}" _text)
        macro(srqi_replace old new)
            string(REPLACE "${old}" "" _removed "${_text}")
            string(LENGTH "${_text}" _before)
            string(LENGTH "${_removed}" _after)
            string(LENGTH "${old}" _length)
            math(EXPR _delta "${_before} - ${_after}")
            if(NOT _delta EQUAL _length)
                message(FATAL_ERROR "Quiescence candidate index requires the reviewed scheduler ring")
            endif()
            string(REPLACE "${old}" "${new}" _text "${_text}")
        endmacro()
        srqi_replace([=[    memset(qring, 0, sizeof qring);]=]
            [=[    memset(qring, 0, sizeof qring);
    SnesRecompQuiescenceIndex qindex;
    snesrecomp_quiescence_init(&qindex);]=])
        srqi_replace([=[            for (unsigned qi=0; qi<64; qi++) {
                QuiescentState *old=&qring[qi];]=]
            [=[            uint64_t candidates = snesrecomp_quiescence_candidates(
                &qindex, now.pc, now.continuous_read_epoch, now.write_epoch);
            for (unsigned qi = snesrecomp_quiescence_pop(&candidates); qi < 64;
                 qi = snesrecomp_quiescence_pop(&candidates)) {
                QuiescentState *old=&qring[qi];]=])
        srqi_replace([=[            qring[steps & 63]=now;]=]
            [=[            snesrecomp_quiescence_set(&qindex, (unsigned)(steps & 63),
                now.pc, now.continuous_read_epoch, now.write_epoch, now.step != 0);
            qring[steps & 63]=now;]=])
        set(_output "${CMAKE_BINARY_DIR}/generated/quiescence-index/interp_bridge.c")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/quiescence-index")
        file(WRITE "${_output}" "#include \"snesrecomp_platform/quiescence_index.h\"\n${_text}")
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
        message(FATAL_ERROR "Quiescence candidate index expected exactly one bridge")
    endif()
    set(${sources_var} "${_result}" PARENT_SCOPE)
endfunction()
