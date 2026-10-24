include_guard(GLOBAL)

# Preserve the CPU bus contract while bypassing redundant mapper dispatch for
# a live, plain LoROM read that provably fits its current allocation. No ROM
# identity or pointer is cached; hardware, chips and mirrored reads stay original.
function(snesrecomp_platform_overlay_plain_rom_reads sources_var core_root)
    foreach(_contract IN ITEMS "snes/cart.c|39de892deee820c7184e0b08aad8482db7627b5be3c1869f48bfb4bbfaa0190f" "common_rtl.c|c6ca644bebeba1890c9ef098f4c1594fa8463020c4c6cab313569b1785af8f4c" "cpu_state.h|540b8eaa8a6ee3886164e206b0555e06dcd2be99800c3ec0e268f1a31a283bbd")
        string(REPLACE "|" ";" _parts "${_contract}")
        list(GET _parts 0 _path)
        list(GET _parts 1 _expected)
        file(READ "${core_root}/runner/src/${_path}" _contract_text)
        string(REPLACE "\r\n" "\n" _contract_text "${_contract_text}")
        string(SHA256 _hash "${_contract_text}")
        if(NOT _hash STREQUAL _expected)
            message(FATAL_ERROR "Review plain ROM bus dependency ${_path}: ${_hash}")
        endif()
    endforeach()
    set(_result)
    set(_count 0)
    foreach(_input IN LISTS ${sources_var})
        get_filename_component(_name "${_input}" NAME)
        if(NOT _name STREQUAL "cpu_state.c")
            list(APPEND _result "${_input}")
            continue()
        endif()
        file(READ "${_input}" _text)
        string(REPLACE "\r\n" "\n" _text "${_text}")
        string(SHA256 _hash "${_text}")
        if(NOT _hash STREQUAL "848b3fd754def166ee3e00d6bbde42160f7e2a848352c6fd7aa07260022f4870")
            message(FATAL_ERROR "Review plain ROM CPU bus contract: ${_hash}")
        endif()
        set(_old [=[    if (off >= 0) return cpu_latch_read8(cpu, cpu->ram[off]);
    if (is_hw_reg(bank, addr)) {]=])
        set(_new [=[    if (off >= 0) return cpu_latch_read8(cpu, cpu->ram[off]);
    /* Plain, in-allocation LoROM has no mapper side effects. Keep the
     * preceding live bus note and all MMIO, mirroring and chip fallbacks. */
    Cart *plain = g_snes ? g_snes->cart : NULL;
    if (addr >= 0x8000u && plain && plain->type == CART_LOROM && plain->rom) {
        const uint32_t offset = ((uint32_t)(bank & 0x7fu) << 15) | (addr & 0x7fffu);
        if (offset < plain->romSize)
            return cpu_latch_read8(cpu, plain->rom[offset]);
    }
    if (is_hw_reg(bank, addr)) {]=])
        string(FIND "${_text}" "${_old}" _offset)
        if(_offset EQUAL -1)
            message(FATAL_ERROR "Missing reviewed plain ROM bus seam")
        endif()
        string(REPLACE "${_old}" "${_new}" _text "${_text}")
        set(_output "${CMAKE_BINARY_DIR}/generated/plain-rom-bus/cpu_state.c")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/plain-rom-bus")
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
        message(FATAL_ERROR "Plain ROM bus overlay expected one CPU source")
    endif()
    set(${sources_var} "${_result}" PARENT_SCOPE)
endfunction()
