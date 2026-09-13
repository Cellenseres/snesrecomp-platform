include_guard(GLOBAL)

function(snesrecomp_platform_prepare_runner_sources sources_var snesrecomp_root)
    set(_upstream
        "${snesrecomp_root}/runner/src/snes/interp_bridge.c")
    if(NOT EXISTS "${_upstream}")
        message(FATAL_ERROR "Missing snesrecomp interpreter bridge: ${_upstream}")
    endif()

    set(_overlay_dir
        "${CMAKE_BINARY_DIR}/generated/snesrecomp-platform/snes")
    set(_overlay "${_overlay_dir}/interp_bridge.c")
    file(MAKE_DIRECTORY "${_overlay_dir}")
    file(READ "${_upstream}" _source)

    if(_source MATCHES
       "Yield nested LLE at the owning scheduler's frame deadline")
        message(FATAL_ERROR
            "The fetched snesrecomp source contains the former in-place "
            "nested-LLE patch. Restore the pinned dependency before configuring.")
    endif()

    set(_include_old [=[
#include "interp_bridge.h"
#include "interp816.h"
]=])
    set(_include_new [=[
#include "interp_bridge.h"
#include "snesrecomp_platform/runtime_policy.h"
#include "interp816.h"
]=])
    string(FIND "${_source}" "${_include_old}" _include_pos)
    if(_include_pos EQUAL -1)
        message(FATAL_ERROR
            "The pinned interp_bridge.c include context changed.")
    endif()
    string(REPLACE "${_include_old}" "${_include_new}"
        _patched "${_source}")

    set(_loop_old [=[
    for (; steps < step_cap; steps++) {
        const uint32_t pc_before = ((uint32_t)in.k << 16) | in.pc;
]=])
    set(_loop_new [=[
    for (; steps < step_cap; steps++) {
        const uint32_t pc_before = ((uint32_t)in.k << 16) | in.pc;

        /* Platform-owned policy; bridge-owned unwind mechanics. */
        if (snesrecomp_nested_lle_deadline_due(
                yield_pc != 0, stop_on_rti, s_lle_sched_depth,
                s_interp_bounce_owner_depth, s_lle_master_deadline,
                cpu->master_cycles)) {
            s_lle_unwind_active = 1;
            s_lle_unwind_pc24 = pc_before & 0xFFFFFFu;
            s_lle_unwind_owner_depth = s_interp_bounce_owner_depth;
            sync_interp_to_cpu(&in, cpu);
            bridge_apu_flush(cpu);
            return 1;
        }

]=])
    string(FIND "${_patched}" "${_loop_old}" _loop_pos)
    if(_loop_pos EQUAL -1)
        message(FATAL_ERROR
            "The pinned interp_bridge.c loop context changed.")
    endif()
    string(REPLACE "${_loop_old}" "${_loop_new}"
        _patched "${_patched}")

    # A deadline unwind now leaves the bridge instead of switching the
    # interpreted coroutine, and the bridge decides which by a flag the
    # deadline test sets as a side effect. The yield path it replaces resumes
    # at the unwind PC inside the same frame, with the compiled callsite's JSR
    # frame still on the guest stack; returning instead hands that frame to
    # nobody and the guest eventually returns through it. Drop the side effect
    # so the flag stays clear and the coroutine switch is taken, which is the
    # behaviour this platform's nested-LLE deadline policy above assumes.
    set(_deadline_old [=[
    if (reached)
        s_lle_next_unwind_is_deadline = 1;
    return reached;
]=])
    set(_deadline_new [=[
    return reached;
]=])
    string(FIND "${_patched}" "${_deadline_old}" _deadline_pos)
    if(_deadline_pos EQUAL -1)
        message(FATAL_ERROR
            "The pinned LLE deadline unwind context changed.")
    endif()
    string(REPLACE "${_deadline_old}" "${_deadline_new}"
        _patched "${_patched}")

    file(WRITE "${_overlay}" "${_patched}")

    set_source_files_properties("${_overlay}" PROPERTIES
        INCLUDE_DIRECTORIES
            "${snesrecomp_root}/runner/src/snes;${snesrecomp_root}/runner/src"
    )

    set(_sources "${${sources_var}}")
    list(REMOVE_ITEM _sources "${_upstream}")
    list(APPEND _sources "${_overlay}")
    set(${sources_var} "${_sources}" PARENT_SCOPE)
endfunction()

function(snesrecomp_platform_target_launcher_overlays target recomp_ui_root)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Unknown launcher overlay target: ${target}")
    endif()

    set(_generated_root
        "${CMAKE_BINARY_DIR}/generated/snesrecomp-platform/recomp-ui")

    # C11 compatibility: the UI uses C23 empty initializers.
    set(_binds_upstream "${recomp_ui_root}/src/common/launcher_binds.c")
    set(_binds_overlay "${_generated_root}/common/launcher_binds.c")
    if(NOT EXISTS "${_binds_upstream}")
        message(FATAL_ERROR "Missing recomp-ui launcher_binds.c")
    endif()
    file(READ "${_binds_upstream}" _binds_source)
    string(REPLACE "= {}" "= {0}" _binds_patched "${_binds_source}")
    file(MAKE_DIRECTORY "${_generated_root}/common")
    file(WRITE "${_binds_overlay}" "${_binds_patched}")
    set_source_files_properties("${_binds_overlay}" PROPERTIES
        INCLUDE_DIRECTORIES
            "${recomp_ui_root}/src/common;${recomp_ui_root}/src"
    )

    get_target_property(_sources "${target}" SOURCES)
    list(REMOVE_ITEM _sources "${_binds_upstream}")
    set_property(TARGET "${target}" PROPERTY SOURCES "${_sources}")
    target_sources("${target}" PRIVATE "${_binds_overlay}")
endfunction()
