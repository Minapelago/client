# Catch2 is fetched at configure time with FetchContent.
include(FetchContent)

set(CATCH2_VERSION "v3.5.4")

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        ${CATCH2_VERSION}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

file(GLOB _mthap_test_sources CONFIGURE_DEPENDS unit/*.cpp)
add_executable(mthap_unit_tests ${_mthap_test_sources})

target_include_directories(mthap_unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(mthap_unit_tests PRIVATE
    Catch2::Catch2WithMain
    mthap_core
    mthap::minamodapi
)
# PAL pieces the tests exercise without Frida or the module: the file-log backend (ILog seam,
# set_default_log, logf) and the cert store.
target_sources(mthap_unit_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/pal/log.cpp"
    "${CMAKE_SOURCE_DIR}/src/pal/linux/log_linux.cpp"
    "${CMAKE_SOURCE_DIR}/src/pal/linux/cert_linux.cpp"
    # DeathHooks lives in the mthap module (not linked by tests); compile it in directly so its
    # sparkless-demise gating can be unit-tested against mthap_core (mod:: and the gate).
    "${CMAKE_SOURCE_DIR}/src/mth/features/death_hooks.cpp"
    # Same trick for the deathlink payload codec: it lives in the module, which the tests do not link,
    # but it only needs nlohmann (already exposed by mthap_core).
    "${CMAKE_SOURCE_DIR}/src/mth/net/deathlink.cpp")

catch_discover_tests(mthap_unit_tests)
