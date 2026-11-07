# Notices.cmake - stage each linked dependency's own licence file into the drop-in.
#
# One level deep: these are the libraries the mod links against, and what they vendor is reachable from their upstream. Sources are the trees the
# build already fetched, so each platform stages what it links: Frida-gum on Linux, MinHook on Windows.
include_guard(GLOBAL)

# Fetched but not linked into the mod, so nothing of theirs ships.
set(_MTHAP_NOTICE_SKIP
    catch2      # test binary only
    minamodapi  # game ABI declarations, no upstream licence file
)

# Stage licence files under <dropin>/third_party_notices/<component>/ and return the staged paths in out_var. They are custom-command outputs, not a
# POST_BUILD step, so deleting mods/ restores them.
function(mthap_stage_notices dropin out_var)
    set(_dir "${dropin}/third_party_notices")
    set(_staged "")
    set(_missing "")

    # FetchContent checkouts: <name>-src/{LICENSE*,COPYING*,LICENSES/*}.
    file(GLOB _dep_dirs "${FETCHCONTENT_BASE_DIR}/*-src")
    foreach(_dep_dir IN LISTS _dep_dirs)
        get_filename_component(_dep "${_dep_dir}" NAME)
        string(REGEX REPLACE "-src$" "" _dep "${_dep}")
        if(_dep IN_LIST _MTHAP_NOTICE_SKIP)
            continue()
        endif()

        file(GLOB _files LIST_DIRECTORIES false "${_dep_dir}/LICENSE*" "${_dep_dir}/COPYING*" "${_dep_dir}/LICENSES/*")
        if(NOT _files)
            list(APPEND _missing "${_dep}")
            continue()
        endif()
        _mthap_stage_notice_files("${_dir}" "${_dep}" "${_files}" _staged)
    endforeach()

    # vcpkg ports: the copyright file vcpkg installs for every port it builds.
    file(GLOB _copyrights "${CMAKE_BINARY_DIR}/vcpkg_installed/*/share/*/copyright")
    foreach(_copyright IN LISTS _copyrights)
        get_filename_component(_port "${_copyright}" DIRECTORY)
        get_filename_component(_port "${_port}" NAME)
        # vcpkg-cmake* are build-time helpers; no code of theirs reaches the binary.
        if(_port MATCHES "^vcpkg-")
            continue()
        endif()
        _mthap_stage_notice_files("${_dir}" "${_port}" "${_copyright}" _staged)
    endforeach()

    if(_missing)
        list(JOIN _missing ", " _missing_text)
        message(WARNING "notices: no licence file found for ${_missing_text}")
    endif()

    # Configured into the build tree and copied in as a real output, like mod.yc, so deleting mods/ restages it.
    set(_readme_src "${CMAKE_BINARY_DIR}/generated/third_party_notices.md")
    configure_file("${CMAKE_SOURCE_DIR}/packaging/third_party_notices.md.in" "${_readme_src}" @ONLY)
    add_custom_command(
        OUTPUT "${_dir}/README.md"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_readme_src}" "${_dir}/README.md"
        DEPENDS "${_readme_src}"
        VERBATIM
        COMMENT "Staging third_party_notices/README.md")
    list(APPEND _staged "${_dir}/README.md")

    set(${out_var} "${_staged}" PARENT_SCOPE)
endfunction()

# Copy one component's licence files under their upstream names.
function(_mthap_stage_notice_files dir component files staged_var)
    set(_staged "${${staged_var}}")
    foreach(_file IN LISTS files)
        get_filename_component(_name "${_file}" NAME)
        set(_out "${dir}/${component}/${_name}")
        add_custom_command(
            OUTPUT "${_out}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_file}" "${_out}"
            DEPENDS "${_file}"
            VERBATIM
            COMMENT "Staging third_party_notices/${component}/${_name}")
        list(APPEND _staged "${_out}")
    endforeach()
    set(${staged_var} "${_staged}" PARENT_SCOPE)
endfunction()
