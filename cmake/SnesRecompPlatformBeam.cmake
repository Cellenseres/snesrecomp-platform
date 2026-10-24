include_guard(GLOBAL)

# The current owner of snes.c composes this into its source overlay. The
# SDK-free certificate excludes IRQ comparators, HBlank/line/field edges and
# the automatic-pad latch; every excluded case keeps the complete beam walk.
function(snesrecomp_platform_patch_beam_clock text_var)
    set(_text "${${text_var}}")
    set(_marker [=[static uint32_t snes_advance_beam(Snes *snes, uint32_t clocks, bool check_irq) {]=])
    string(FIND "${_text}" "${_marker}" _start)
    if(_start EQUAL -1)
        message(FATAL_ERROR "Missing reviewed SNES beam routine")
    endif()
    string(SUBSTRING "${_text}" ${_start} -1 _tail)
    string(FIND "${_tail}" "\n}\n\nvoid snes_advance_master_cycles" _end)
    if(_end EQUAL -1)
        message(FATAL_ERROR "Missing reviewed SNES beam boundary")
    endif()
    math(EXPR _length "${_end} + 2")
    string(SUBSTRING "${_tail}" 0 ${_length} _function)
    string(SHA256 _hash "${_function}")
    if(NOT _hash STREQUAL "b6cb5ebc90a7fb6ea454b5e8193d535f89c8cffd76be1885a42abc73fd32f4db")
        message(FATAL_ERROR "Review SNES eventless beam contract: ${_hash}")
    endif()
    set(_fast [=[
  if (snesrecomp_beam_eventless_step(snes->hPos, snes->vPos, clocks,
          check_irq, snes->hIrqEnabled || snes->vIrqEnabled, snes->autoJoyRead)) {
    snes->hPos = (uint16_t)(snes->hPos + clocks);
    if (snes->autoJoyTimer)
      snes->autoJoyTimer = clocks >= snes->autoJoyTimer
                        ? 0 : (uint16_t)(snes->autoJoyTimer - clocks);
    snes->inVblank = snes->vPos >= 225u;
    return clocks;
  }
]=])
    string(REPLACE "${_marker}" "${_marker}${_fast}" _text "${_text}")
    string(PREPEND _text "#include \"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../include/snesrecomp_platform/beam_clock.h\"\n")
    string(APPEND _text "\nuint32_t snesrecomp_beam_hold_active(void) { return s_beam_hold != 0; }\n")
    set_property(GLOBAL PROPERTY SNESRECOMP_PLATFORM_BEAM_HOLD_QUERY ON)
    set(${text_var} "${_text}" PARENT_SCOPE)
endfunction()
