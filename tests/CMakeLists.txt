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
target_compile_definitions(mthap_unit_tests PRIVATE MTHAP_CONTRACT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/contract.json")
target_link_libraries(mthap_unit_tests PRIVATE
    Catch2::Catch2WithMain
    mthap_core
    mthap::minamodapi
)
# PAL pieces the tests exercise without Frida or the module: the file-log backend (ILog seam, set_default_log, logf) and the cert store.
target_sources(mthap_unit_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/pal/log.cpp"
    "${CMAKE_SOURCE_DIR}/src/pal/linux/cert_linux.cpp"
    # DeathHooks lives in the mthap module, which tests do not link; compile it in to unit-test its sparkless-demise gating against mthap_core.
    "${CMAKE_SOURCE_DIR}/src/mth/features/death_hooks.cpp"
    # The deathlink payload codec and the AP link live in the module; ap_link_net_test drives the real link against a loopback server.
    "${CMAKE_SOURCE_DIR}/src/mth/net/deathlink.cpp"
    "${CMAKE_SOURCE_DIR}/src/mth/net/ap_link_apclient.cpp")
target_link_libraries(mthap_unit_tests PRIVATE mthap::apclient)

catch_discover_tests(mthap_unit_tests PROPERTIES TIMEOUT 60)
