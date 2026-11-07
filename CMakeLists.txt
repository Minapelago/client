cmake_minimum_required(VERSION 3.25)

# project() has no VERSION: it rejects prerelease versions like 1.0.1-beta.1. The real version comes from git describe (cmake/SimpleGitVersion.cmake).
project(mth-apclient
    LANGUAGES C CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The module exports only MinaMod_Init (explicit default visibility); any other export is interposition surface in the host process.
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# The unit tests exercise the Linux PAL pieces.
if(WIN32)
    set(_mthap_tests_default OFF)
else()
    set(_mthap_tests_default ON)
endif()
option(MTHAP_BUILD_TESTS "Build unit tests (Catch2)" ${_mthap_tests_default})

set(MTHAP_MIN_GAME_VERSION 148662)
set(MTHAP_MAX_GAME_VERSION 999999)

include(SimpleGitVersion)

# llvm-mingw routes printf through msvcrt, which drops %lld/%zu; switch to mingw's own stdio. MINGW is unset on clang-cl and Linux.
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

# Archipelago net stack (apclientpp/wswrap/websocketpp), fetched into the build dir. ApClient.cmake patches one line of websocketpp's tls.hpp.
include(ApClient)
include(DearImGui)
include(MinaModApi)

# Directory-scope flags apply only to targets created below, so the FetchContent dependencies above keep their own flags. Catch2 (tests/) gets them.
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
