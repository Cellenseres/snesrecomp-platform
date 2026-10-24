include_guard(GLOBAL)

# A generated overlay replaces a translation unit, including its compiler
# contract. Target-level settings alone do not preserve per-source production
# diagnostics, ABI options or generated-header dependencies.
function(snesrecomp_platform_copy_source_contract source output)
    foreach(_property COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FLAGS
            LANGUAGE OBJECT_DEPENDS SKIP_PRECOMPILE_HEADERS)
        get_source_file_property(_value "${source}" "${_property}")
        if(NOT _value STREQUAL "NOTFOUND")
            set_source_files_properties("${output}" PROPERTIES
                "${_property}" "${_value}")
        endif()
    endforeach()
    # The adapter supplies include paths separately: console config adapters
    # must retain their position ahead of upstream's desktop headers.
endfunction()
