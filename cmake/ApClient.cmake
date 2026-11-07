# ApClient.cmake - header-only Archipelago client deps via FetchContent.
#
# apclientpp -> wswrap -> websocketpp form the Archipelago net stack. Fetching them here keeps the patched websocketpp copy in build/_deps, away
# from tracked files. Pins are exact commits. asio and OpenSSL come from vcpkg (found here); ZLIB and json are found in src/mth.
#
# Exposes: mthap::apclient (INTERFACE: system include dirs, the net stack's compile definitions, asio and OpenSSL).

include_guard(GLOBAL)
include(FetchContent)

# SOURCE_SUBDIR points at a path with no CMakeLists.txt, so MakeAvailable populates each dependency without add_subdirectory (header-only).
set(_apclient_no_build "_headers_only")

FetchContent_Declare(
    websocketpp
    GIT_REPOSITORY https://github.com/zaphoyd/websocketpp.git
    GIT_TAG        56123c87598f8b1dd471be83ca841ceae07f95ba # 0.8.2
    SOURCE_SUBDIR  ${_apclient_no_build}
)

FetchContent_Declare(
    wswrap
    GIT_REPOSITORY https://github.com/black-sliver/wswrap.git
    GIT_TAG        aeba7ac428028723fb26ce92488f260660f786b1
    GIT_SUBMODULES "" # skip subprojects/wsjs (emscripten backend, unused)
    SOURCE_SUBDIR  ${_apclient_no_build}
)

FetchContent_Declare(
    apclientpp
    GIT_REPOSITORY https://github.com/black-sliver/apclientpp.git
    GIT_TAG        79621690a3e845645f43888b0fe234a99c74892e
    SOURCE_SUBDIR  ${_apclient_no_build}
)

FetchContent_MakeAvailable(websocketpp wswrap apclientpp)

find_package(asio CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

# Upstream replaces the asio error with a generic tls_handshake_failed, which merges certificate rejection, version mismatch and mid-handshake reset.
# The asio code carries the OpenSSL reason and nothing here matches the generic one. The rewrite is idempotent and fails configure if neither text matches.
set(_ws_tls "${websocketpp_SOURCE_DIR}/websocketpp/transport/asio/security/tls.hpp")
set(_ws_tls_old "m_ec = socket::make_error_code(socket::error::tls_handshake_failed);")
set(_ws_tls_new "m_ec = ec;")
file(READ "${_ws_tls}" _ws_tls_text)
string(FIND "${_ws_tls_text}" "${_ws_tls_old}" _ws_tls_at)
if(NOT _ws_tls_at EQUAL -1)
    string(REPLACE "${_ws_tls_old}" "${_ws_tls_new}" _ws_tls_text "${_ws_tls_text}")
    file(WRITE "${_ws_tls}" "${_ws_tls_text}")
elseif(NOT _ws_tls_text MATCHES "if \\(ec\\) \\{\n *m_ec = ec;\n *\\} else \\{")
    message(FATAL_ERROR "websocketpp tls.hpp matches neither the original nor the patched handle_init text")
endif()

add_library(mthap_apclient INTERFACE)
add_library(mthap::apclient ALIAS mthap_apclient)
target_include_directories(mthap_apclient SYSTEM INTERFACE
    "${apclientpp_SOURCE_DIR}"
    "${wswrap_SOURCE_DIR}/include"
    "${websocketpp_SOURCE_DIR}"
)
target_compile_definitions(mthap_apclient INTERFACE
    ASIO_STANDALONE
    AP_NO_SCHEMA
    _WEBSOCKETPP_CPP11_STL_
    _WEBSOCKETPP_CPP11_THREAD_
    _WEBSOCKETPP_NO_BOOST_
    WSWRAP_ASYNC_CLEANUP
)
target_link_libraries(mthap_apclient INTERFACE asio::asio OpenSSL::SSL OpenSSL::Crypto)
