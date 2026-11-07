# FridaGum.cmake - build a slim, x86-only frida-gum devkit from source and expose it as `frida::gum`.
#
# Frida is the hook backend on Linux; Windows uses MinHook (cmake/MinHook.cmake) because the prebuilt Windows devkit is MSVC-built and needs
# MSVC-only codegen helpers that llvm-mingw can't resolve.
#
# Why a source build: upstream's devkit bundles Capstone with all ~16 disassembler architectures, and only x86 is used (Interceptor relocator and
# Stalker). Capstone's arch-dispatch table references every backend, so the linker can't drop them, and ~7 MB of ARM/AArch64/MIPS/PPC code
# ships in libmthap.so. Building frida-gum with `-Dcapstone:archs=x86` removes it. `-Ddevkits=gum` emits the same single `frida-gum.h` +
# `libfrida-gum.a` shape as the prebuilt devkit, so `#include <frida-gum.h>` is unchanged.
#
# Requirements: a C/C++ compiler, `python3`, `git`, `make`, and network at configure time. frida-gum's `configure` bootstraps its own meson
# (releng/meson submodule) and downloads its toolchain (including ninja) and SDK glib, so no system `meson` is needed. The build runs once per
# clean build dir; reconfigures reuse it.
#
# Usage:
#   include(FridaGum)   # Linux only; include(MinHook) on Windows.
#   target_link_libraries(my_target PRIVATE frida::gum)

set(FRIDA_GUM_VERSION "16.5.9")
# Pinned to the exact commit the tag points at.
set(FRIDA_GUM_GIT_TAG "e49e63c8dd1961d1490d11b44e86166a6e776c14") # == tag ${FRIDA_GUM_VERSION}

include(FetchContent)
FetchContent_Declare(
    frida_gum
    GIT_REPOSITORY "https://github.com/frida/frida-gum.git"
    GIT_TAG "${FRIDA_GUM_GIT_TAG}"
    GIT_SHALLOW TRUE
    # frida's own `configure` fetches submodules (releng + vendored meson); skipping FetchContent's handling avoids heavy test-data submodules.
    GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(frida_gum)  # no root CMakeLists -> populate only

set(_frida_devkit "${frida_gum_SOURCE_DIR}/build/gum/devkit")
set(_frida_lib "${_frida_devkit}/libfrida-gum.a")

# Build the x86-only devkit once (guarded on the produced archive). A clean build dir rebuilds; a reconfigure reuses it.
if(NOT EXISTS "${_frida_lib}")
    message(STATUS "FridaGum: building x86-only devkit from source (downloads frida toolchain+SDK; first time only)...")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}"
                ./configure "--prefix=${frida_gum_SOURCE_DIR}/_unused-prefix" --
                -Ddevkits=gum -Ddefault_library=static
                -Dcapstone:archs=x86 --force-fallback-for=capstone
        WORKING_DIRECTORY "${frida_gum_SOURCE_DIR}"
        RESULT_VARIABLE _frida_cfg_res
    )
    if(NOT _frida_cfg_res EQUAL 0)
        message(FATAL_ERROR "FridaGum: frida-gum ./configure failed (exit ${_frida_cfg_res})")
    endif()

    execute_process(
        COMMAND make
        WORKING_DIRECTORY "${frida_gum_SOURCE_DIR}"
        RESULT_VARIABLE _frida_build_res
    )
    if(NOT _frida_build_res EQUAL 0)
        message(FATAL_ERROR "FridaGum: frida-gum build failed (exit ${_frida_build_res})")
    endif()

    if(NOT EXISTS "${_frida_lib}")
        message(FATAL_ERROR "FridaGum: build succeeded but ${_frida_lib} is missing")
    endif()
endif()

add_library(frida::gum STATIC IMPORTED GLOBAL)
set_target_properties(frida::gum PROPERTIES
    IMPORTED_LOCATION "${_frida_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_frida_devkit}"
)
target_link_libraries(frida::gum INTERFACE dl rt pthread resolv m)

message(STATUS "FridaGum: ${FRIDA_GUM_VERSION} x86-only devkit -> ${_frida_lib}")
