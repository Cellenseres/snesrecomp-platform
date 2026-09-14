include_guard(GLOBAL)

# Cached, not set(): this file is included from one scope and used in another.
option(SNESRECOMP_PLATFORM_LLE_DEADLINE_SEAM
    "Drop snesrecomp's sticky LLE deadline flag so the unwind stays a coroutine switch" ON)

# An hle_func stub has no prologue to overwrite the seeded entry-S, so the
# ancestor scan matches it and the unwind stops one frame short.
option(SNESRECOMP_PLATFORM_HLE_ENTRY_S_SEAM
    "Drop upstream's entry-S seeding in RecompStackPush" ON)

function(snesrecomp_platform_overlay_cpu_infra sources_var snesrecomp_root)
    if(NOT SNESRECOMP_PLATFORM_HLE_ENTRY_S_SEAM)
        message(STATUS "snesrecomp-platform: entry-S seam OFF (upstream behaviour)")
        return()
    endif()

    set(_upstream "${snesrecomp_root}/runner/src/common_cpu_infra.c")
    if(NOT EXISTS "${_upstream}")
        message(FATAL_ERROR "Missing snesrecomp cpu infra: ${_upstream}")
    endif()
    file(READ "${_upstream}" _source)

    set(_seed_old [=[
    g_cpu_entry_s[slot] = g_cpu.S;
]=])
    set(_seed_new "")
    string(FIND "${_source}" "${_seed_old}" _seed_pos)
    if(_seed_pos EQUAL -1)
        # Absent is the state this overlay produces: a core that predates the
        # seeding, or one that has dropped it again, needs no copy.
        if(_source MATCHES "g_cpu_entry_s.slot. *=")
            message(FATAL_ERROR
                "The pinned RecompStackPush entry-S context changed.")
        endif()
        message(STATUS "snesrecomp-platform: core does not seed entry-S")
        return()
    endif()
    string(REPLACE "${_seed_old}" "${_seed_new}" _patched "${_source}")

    set(_overlay_dir "${CMAKE_BINARY_DIR}/generated/snesrecomp-platform")
    set(_overlay "${_overlay_dir}/common_cpu_infra.c")
    file(MAKE_DIRECTORY "${_overlay_dir}")
    file(WRITE "${_overlay}" "${_patched}")

    set_source_files_properties("${_overlay}" PROPERTIES
        INCLUDE_DIRECTORIES
            "${snesrecomp_root}/runner/src;${snesrecomp_root}/runner/src/snes"
    )

    set(_sources "${${sources_var}}")
    list(REMOVE_ITEM _sources "${_upstream}")
    list(APPEND _sources "${_overlay}")
    set(${sources_var} "${_sources}" PARENT_SCOPE)
endfunction()

function(snesrecomp_platform_prepare_runner_sources sources_var snesrecomp_root)
    snesrecomp_platform_overlay_cpu_infra("${sources_var}" "${snesrecomp_root}")
    set(${sources_var} "${${sources_var}}" PARENT_SCOPE)

    # OFF leaves interp_bridge.c pristine, for an A/B of the unwind policy.
    if(DEFINED SNESRECOMP_PLATFORM_BRIDGE_OVERLAY AND
       NOT SNESRECOMP_PLATFORM_BRIDGE_OVERLAY)
        message(STATUS "snesrecomp-platform: bridge overlay OFF")
        return()
    endif()
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

    # Drop the sticky flag so a deadline unwind stays a coroutine switch;
    # returning instead abandons the callsite's JSR frame on the guest stack.
    set(_deadline_old [=[
    if (reached)
        s_lle_next_unwind_is_deadline = 1;
    return reached;
]=])
    set(_deadline_new [=[
    return reached;
]=])
    if(SNESRECOMP_PLATFORM_LLE_DEADLINE_SEAM)
        string(FIND "${_patched}" "${_deadline_old}" _deadline_pos)
        if(_deadline_pos EQUAL -1)
            message(FATAL_ERROR
                "The pinned LLE deadline unwind context changed.")
        endif()
        string(REPLACE "${_deadline_old}" "${_deadline_new}"
            _patched "${_patched}")
    else()
        message(STATUS
            "snesrecomp-platform: LLE deadline seam OFF (upstream behaviour)")
    endif()

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

    # The pinned UI supports widescreen but does not draw its control.
    # Patch a build-tree copy; keep fetched sources untouched.
    set(_imgui_upstream
        "${recomp_ui_root}/src/common/backends/imgui/launcher_imgui.cpp")
    set(_imgui_overlay
        "${_generated_root}/common/backends/imgui/launcher_imgui.cpp")
    if(NOT EXISTS "${_imgui_upstream}")
        message(FATAL_ERROR "Missing recomp-ui launcher_imgui.cpp")
    endif()
    file(READ "${_imgui_upstream}" _imgui_source)

    set(_legacy_widescreen_old [=[
        row_fullscreen(m, th);
        if (m->num_display_layouts > 0) {
]=])
    set(_legacy_widescreen_new [=[
        row_fullscreen(m, th);
        if (m->widescreen_supported) {
            row_label_right("Widescreen", th, cb);
            bool widescreen = m->s.widescreen != 0;
            if (ImGui::Checkbox("##widescreen", &widescreen))
                launcher_model_toggle_widescreen(m);
        }
        if (m->num_display_layouts > 0) {
]=])
    string(FIND "${_imgui_source}" "${_legacy_widescreen_old}"
        _legacy_widescreen_pos)
    if(_legacy_widescreen_pos EQUAL -1)
        message(FATAL_ERROR
            "The pinned recomp-ui legacy fullscreen context changed.")
    endif()
    string(REPLACE "${_legacy_widescreen_old}" "${_legacy_widescreen_new}"
        _imgui_patched "${_imgui_source}")

    set(_deep_widescreen_old [=[
    row_fullscreen(m, th);
    if (m->num_display_layouts > 0) {
]=])
    set(_deep_widescreen_new [=[
    row_fullscreen(m, th);
    if (m->widescreen_supported) {
        row_label_right("Widescreen", th, cb);
        bool widescreen = m->s.widescreen != 0;
        if (ImGui::Checkbox("##widescreen", &widescreen))
            launcher_model_toggle_widescreen(m);
    }
    if (m->num_display_layouts > 0) {
]=])
    string(FIND "${_imgui_patched}" "${_deep_widescreen_old}"
        _deep_widescreen_pos)
    if(_deep_widescreen_pos EQUAL -1)
        message(FATAL_ERROR
            "The pinned recomp-ui deep fullscreen context changed.")
    endif()
    string(REPLACE "${_deep_widescreen_old}" "${_deep_widescreen_new}"
        _imgui_patched "${_imgui_patched}")

    file(MAKE_DIRECTORY "${_generated_root}/common/backends/imgui")
    file(WRITE "${_imgui_overlay}" "${_imgui_patched}")
    set_source_files_properties("${_imgui_overlay}" PROPERTIES
        INCLUDE_DIRECTORIES
            "${recomp_ui_root}/src/common/backends/imgui;${recomp_ui_root}/src/common;${recomp_ui_root}/src;${recomp_ui_root}/src/third_party/imgui;${recomp_ui_root}/src/third_party/imgui/backends"
    )

    get_target_property(_sources "${target}" SOURCES)
    list(REMOVE_ITEM _sources "${_binds_upstream}" "${_imgui_upstream}")
    set_property(TARGET "${target}" PROPERTY SOURCES "${_sources}")
    target_sources("${target}" PRIVATE "${_binds_overlay}" "${_imgui_overlay}")
endfunction()
