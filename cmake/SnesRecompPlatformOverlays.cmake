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
#if SNESRECOMP_REVERSE_DEBUG
]=])
    set(_loop_new [=[
    for (; steps < step_cap; steps++) {
        const uint32_t pc_before = ((uint32_t)in.k << 16) | in.pc;

        /* Platform-owned policy; bridge-owned unwind mechanics. */
        if (snesrecomp_platform_nested_lle_deadline_due(
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

#if SNESRECOMP_REVERSE_DEBUG
]=])
    string(FIND "${_patched}" "${_loop_old}" _loop_pos)
    if(_loop_pos EQUAL -1)
        message(FATAL_ERROR
            "The pinned interp_bridge.c loop context changed.")
    endif()
    string(REPLACE "${_loop_old}" "${_loop_new}"
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

    # C11 compatibility: the pinned UI revision uses four C23 empty
    # initializers. Compile a generated copy without rewriting the dependency.
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

    # UX overlay: custom renderer vocabularies are real choices, so present
    # them as a dropdown. Hosts without a custom list retain the legacy toggle.
    set(_imgui_upstream
        "${recomp_ui_root}/src/common/backends/imgui/launcher_imgui.cpp")
    set(_imgui_overlay
        "${_generated_root}/common/backends/imgui/launcher_imgui.cpp")
    if(NOT EXISTS "${_imgui_upstream}")
        message(FATAL_ERROR "Missing recomp-ui launcher_imgui.cpp")
    endif()
    file(READ "${_imgui_upstream}" _imgui_source)
    set(_renderer_old [=[
    if (m->has_renderer) {
        row_label("Renderer", th);
        if (ImGui::Button(launcher_model_renderer_label(m), ImVec2(px(220), px(30))))
            launcher_model_toggle_renderer(m);
    }
]=])
    set(_renderer_new [=[
    if (m->has_renderer) {
        row_label("Renderer", th);
        if (m->renderer_labels && m->num_renderers > 0) {
            ImGui::SetNextItemWidth(px(220));
            if (ImGui::BeginCombo("##renderer",
                                  launcher_model_renderer_label(m))) {
                for (int i = 0; i < m->num_renderers; ++i) {
                    const bool selected = m->s.renderer == i;
                    if (ImGui::Selectable(m->renderer_labels[i], selected))
                        m->s.renderer = i;
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        } else if (ImGui::Button(launcher_model_renderer_label(m),
                                 ImVec2(px(220), px(30)))) {
            launcher_model_toggle_renderer(m);
        }
    }
]=])
    string(FIND "${_imgui_source}" "${_renderer_old}" _renderer_pos)
    if(_renderer_pos EQUAL -1)
        message(FATAL_ERROR
            "The pinned recomp-ui renderer control context changed.")
    endif()
    string(REPLACE "${_renderer_old}" "${_renderer_new}"
        _imgui_patched "${_imgui_source}")

    # Restore the model's existing widescreen control in both display layouts.
    set(_legacy_fullscreen_old [=[
        row_label("Fullscreen", th, cw);
        ImGui::PushID("fullscreen");
        if (ImGui::Button(launcher_model_fullscreen_label(m), ImVec2(px(120), px(30))))
            launcher_model_cycle_fullscreen(m);
        ImGui::PopID();
]=])
    set(_legacy_fullscreen_new [=[
        row_label("Fullscreen", th, cw);
        ImGui::PushID("fullscreen");
        if (ImGui::Button(launcher_model_fullscreen_label(m), ImVec2(px(120), px(30))))
            launcher_model_cycle_fullscreen(m);
        ImGui::PopID();
        if (m->widescreen_supported) {
            row_label("Widescreen", th, cw);
            bool widescreen = m->s.widescreen != 0;
            if (ImGui::Checkbox("##widescreen", &widescreen))
                launcher_model_toggle_widescreen(m);
        }
]=])
    string(FIND "${_imgui_patched}" "${_legacy_fullscreen_old}"
        _legacy_fullscreen_pos)
    if(_legacy_fullscreen_pos EQUAL -1)
        message(FATAL_ERROR
            "The pinned recomp-ui legacy fullscreen context changed.")
    endif()
    string(REPLACE "${_legacy_fullscreen_old}" "${_legacy_fullscreen_new}"
        _imgui_patched "${_imgui_patched}")

    set(_deep_supersampling_marker [=[
    if (m->has_supersampling) {
]=])
    set(_deep_widescreen_replacement [=[
    if (m->widescreen_supported) {
        row_label("Widescreen", th);
        bool widescreen = m->s.widescreen != 0;
        if (ImGui::Checkbox("##widescreen", &widescreen))
            launcher_model_toggle_widescreen(m);
    }

    if (m->has_supersampling) {
]=])
    string(FIND "${_imgui_patched}" "${_deep_supersampling_marker}"
        _deep_supersampling_pos)
    if(_deep_supersampling_pos EQUAL -1)
        message(FATAL_ERROR
            "The pinned recomp-ui deep display context changed.")
    endif()
    string(REPLACE
        "${_deep_supersampling_marker}" "${_deep_widescreen_replacement}"
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
