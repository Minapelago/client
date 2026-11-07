# mth-apclient: pure game logic (mthap_core) + final mod module (mthap).
#
#   mthap_core : pure, cross-platform logic plus the MinaModAPI wrapper (mod::). The unit tests
#                link it without the module or a hook backend, so it has no PAL-impl, backend or
#                OS headers; header includes from src/pal/ are fine.
#   mthap      : final module (mod.so / mod.dll). Composition root; also holds the PAL, the net
#                link and the ImGui overlay sources.
#
# Source lists are globs. CONFIGURE_DEPENDS re-runs the glob when a file is added or removed.

# Header-only; mthap_core parses the server's slot_data blob.
find_package(nlohmann_json CONFIG REQUIRED)

# crc32 + raw deflate for the save container.
find_package(ZLIB REQUIRED)

# The save container writes on its own thread so the game thread never waits on disk.
find_package(Threads REQUIRED)

find_package(asio CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

file(GLOB_RECURSE _mthap_core_sources CONFIGURE_DEPENDS core/*.cpp)

add_library(mthap_core STATIC ${_mthap_core_sources} "${CMAKE_SOURCE_DIR}/src/mod/mod_api.cpp")

target_include_directories(mthap_core PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(mthap_core PUBLIC nlohmann_json::nlohmann_json ZLIB::ZLIB Threads::Threads mthap::minamodapi)
target_compile_features(mthap_core PUBLIC cxx_std_23)

add_git_version_info(mthap_core)

# PAL: the platform-neutral TUs in pal/ plus the per-platform directory. The platform-neutral ones
# read only struct offsets that match on both builds, or reach a divergent offset through a pal::
# seam. Anything whose offsets differ stays in linux/ or windows/.
if(WIN32)
    set(_mthap_pal_platform windows)
else()
    set(_mthap_pal_platform linux)
endif()
file(GLOB _mthap_pal_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/pal/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/pal/${_mthap_pal_platform}/*.cpp")
# llvm-mingw + static link needs an __imp__vsnprintf shim for vcpkg's OpenSSL (msvcrt dllimport);
# clang-cl/MSVC does not.
if(NOT MINGW)
    list(REMOVE_ITEM _mthap_pal_sources "${CMAKE_SOURCE_DIR}/src/pal/windows/mingw_crt_shim.cpp")
endif()

file(GLOB_RECURSE _mthap_module_sources CONFIGURE_DEPENDS app/*.cpp features/*.cpp ui/*.cpp net/*.cpp)

# MinaMod_Init (pal/native_mod_entry.cpp) is a direct source of the module, so no archive
# drops it as unreferenced.
add_library(mthap MODULE
    ${_mthap_module_sources}
    ${_mthap_pal_sources}
    "${MTHAP_OVERLAY_FONT_SOURCE}"
)

target_compile_definitions(mthap PRIVATE
    ASIO_STANDALONE
    AP_NO_SCHEMA
    _WEBSOCKETPP_CPP11_STL_
    _WEBSOCKETPP_CPP11_THREAD_
    _WEBSOCKETPP_NO_BOOST_
    WSWRAP_ASYNC_CLEANUP
)

target_link_libraries(mthap PRIVATE
    mthap_core
    mthap::apclient
    mthap::imgui
    asio::asio
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(WIN32)
    target_link_libraries(mthap PRIVATE minhook::minhook crypt32 dbghelp)
else()
    target_link_libraries(mthap PRIVATE frida::gum dl pthread)
endif()

if(WIN32)
    # Output is `mod.dll`: loaded by the native MinaModAPI from mods/apclient/.
    # CRT linkage is controlled by CMAKE_MSVC_RUNTIME_LIBRARY (per-preset,
    # static via the x64-windows-static triplet).
    set(_mthap_mod_name "mod.dll")
    set_target_properties(mthap PROPERTIES
        OUTPUT_NAME "mod"
        PREFIX ""
        SUFFIX ".dll"
        # Without this the PDB is named after the target (mthap.pdb) and mod.dll's debug
        # directory points at the wrong file.
        PDB_NAME "mod"
    )
else()
    set(_mthap_mod_name "mod.so")
    set_target_properties(mthap PROPERTIES
        OUTPUT_NAME "mod"
        PREFIX ""
        SUFFIX ".so"
    )
    # Static-link our C++ runtime so the dynamic linker can't resolve our
    # modern libstdc++ symbols against whatever the Steam runtime provides.
    target_link_options(mthap PRIVATE
        -static-libgcc
        -static-libstdc++
        "LINKER:-z,now"
        "LINKER:--exclude-libs,ALL"
    )
    # Guard against the .so requiring glibc symbol versions newer than the oldest
    # supported host distro (which makes the mod fail to load in-game silently:
    # no log, no window). Turns that into a loud build failure. See the script's
    # header for why the ceiling is not the sniper container's own glibc.
    add_custom_command(TARGET mthap POST_BUILD
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-glibc-max.sh" "$<TARGET_FILE:mthap>"
        VERBATIM
        COMMENT "Checking max required glibc symbol version (host glibc floor)")
endif()

# Mod manifest: packed semver + configured game-version range. add_git_version_info
# (called on mthap_core above) exported MTHAP_MOD_VERSION into this scope. Staged outside the
# drop-in so the copy rules below, not configure, own everything that lands in mods/apclient/.
configure_file(
    "${CMAKE_SOURCE_DIR}/packaging/mod.yc.in"
    "${CMAKE_BINARY_DIR}/generated/mod.yc"
    @ONLY
)

# Assemble the drop-in: mods/apclient/{mod.yc, mod.<dll|so>, LICENSE,
# third_party_notices/}. The library is self-contained (static runtime + deps), so no
# sidecar libraries ship, but the licences of what is linked in travel with it.
# These are declared OUTPUTs of a target rather than a POST_BUILD step because a POST_BUILD is
# invisible to the generator: deleting mods/ (the directory you copy into the game) left the
# build reporting "no work to do" and shipping nothing. As real outputs they get restored.
set(_mthap_dropin "${CMAKE_BINARY_DIR}/mods/apclient")
add_custom_command(
    OUTPUT "${_mthap_dropin}/${_mthap_mod_name}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_mthap_dropin}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:mthap>" "${_mthap_dropin}/${_mthap_mod_name}"
    DEPENDS mthap
    VERBATIM
    COMMENT "Assembling mods/apclient drop-in")
add_custom_command(
    OUTPUT "${_mthap_dropin}/mod.yc"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_mthap_dropin}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_BINARY_DIR}/generated/mod.yc" "${_mthap_dropin}/mod.yc"
    DEPENDS "${CMAKE_BINARY_DIR}/generated/mod.yc"
    VERBATIM
    COMMENT "Staging mods/apclient/mod.yc")
add_custom_command(
    OUTPUT "${_mthap_dropin}/LICENSE"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_SOURCE_DIR}/LICENSE" "${_mthap_dropin}/LICENSE"
    DEPENDS "${CMAKE_SOURCE_DIR}/LICENSE"
    VERBATIM
    COMMENT "Staging mods/apclient/LICENSE")

# mod.pdb ships beside mod.dll so the crash handler's SymFromAddr finds it and resolves frames
# in-process. MSVC-like frontends only: the MinGW cross-check emits DWARF and no PDB, so
# TARGET_PDB_FILE would not resolve there.
set(_mthap_pdb "")
if(WIN32 AND MSVC)
    add_custom_command(
        OUTPUT "${_mthap_dropin}/mod.pdb"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_mthap_dropin}"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_PDB_FILE:mthap>" "${_mthap_dropin}/mod.pdb"
        DEPENDS mthap
        VERBATIM
        COMMENT "Staging mods/apclient/mod.pdb")
    set(_mthap_pdb "${_mthap_dropin}/mod.pdb")
endif()

include(Notices)
mthap_stage_notices("${_mthap_dropin}" _mthap_notices)

add_custom_target(mthap_dropin ALL
    DEPENDS "${_mthap_dropin}/${_mthap_mod_name}" "${_mthap_dropin}/mod.yc" "${_mthap_dropin}/LICENSE"
    ${_mthap_pdb} ${_mthap_notices})
