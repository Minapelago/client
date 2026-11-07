cmake_minimum_required(VERSION 3.25)

# No VERSION on project(): release-please stamps prerelease versions like 1.0.1-beta.1 and project()
# rejects those outright ("VERSION format invalid"), which would break configure on every beta. The
# build's real version comes from git describe in cmake/SimpleGitVersion.cmake.
project(mth-apclient
    LANGUAGES C CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The mod is loaded into the game's process, so it must export only its entry point:
# anything else is interposition surface against the host and any other loaded mod.
# MinaMod_Init carries an explicit default-visibility attribute and is unaffected.
# --exclude-libs,ALL additionally localizes the symbols of the static archives.
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# The unit tests exercise the Linux PAL pieces, so they are off for Windows builds.
if(WIN32)
    set(_mthap_tests_default OFF)
else()
    set(_mthap_tests_default ON)
endif()
option(MTHAP_BUILD_TESTS "Build unit tests (Catch2)" ${_mthap_tests_default})

set(MTHAP_MIN_GAME_VERSION 148662)
set(MTHAP_MAX_GAME_VERSION 999999)

include(SimpleGitVersion)

# The LLVM-MinGW dev cross routes printf-family calls through msvcrt, which drops
# C99 specifiers like %lld/%zu unless we switch mingw to its own conformant stdio.
# Harmless on the canonical clang-cl (UCRT) and Linux builds, where MINGW is unset.
if(MINGW)
    add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
endif()

if(WIN32)
    include(MinHook)
else()
    include(FridaGum)
endif()
# Sets MTHAP_OVERLAY_FONT_SOURCE, compiled into the module.
include(OverlayFont)

# Archipelago net stack (apclientpp/wswrap/websocketpp), fetched into the build
# dir. ApClient.cmake rewrites one line of websocketpp's tls.hpp after the fetch.
include(ApClient)
include(DearImGui)
include(MinaModApi)

# Directory-scope flags reach only targets created after this point, so every
# FetchContent dependency (imgui, minhook, apclientpp, ...) is declared above and
# keeps its own flags.
include(compiler_setup)

add_subdirectory(src/mth)

if(MTHAP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

file(CREATE_LINK
    "${CMAKE_BINARY_DIR}/compile_commands.json"
    "${CMAKE_SOURCE_DIR}/compile_commands.json"
    SYMBOLIC)
