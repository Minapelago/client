# MinaModApi.cmake - the game's native mod API header via FetchContent.
#
# Single C header (MinaModAPI.h) consumed only by the PAL native entry TU.
#
# The header defines the struct layout the game hands to MinaMod_Init, so a stale pin holds the mod on an old ABI.
# .github/workflows/modapi-upstream.yml compares MINAMODAPI_COMMIT below against upstream main on a schedule and opens an issue when they diverge;
# bump the pin from that issue.
#
# Exposes: mthap::minamodapi (INTERFACE, system include dir only).

include_guard(GLOBAL)
include(FetchContent)

# Parsed by .github/workflows/modapi-upstream.yml; keep it one bare 40-char sha on this line.
set(MINAMODAPI_COMMIT 26efcf46f0e6b05b8d751111929193e17230306f) # main @ 2026-08-05

# SOURCE_SUBDIR points at a path with no CMakeLists.txt, so MakeAvailable populates without add_subdirectory (header-only).
FetchContent_Declare(
    minamodapi
    GIT_REPOSITORY https://github.com/YachtClubGames/MinaModAPI.git
    GIT_TAG ${MINAMODAPI_COMMIT}
    SOURCE_SUBDIR _headers_only
)
FetchContent_MakeAvailable(minamodapi)

add_library(mthap_minamodapi INTERFACE)
add_library(mthap::minamodapi ALIAS mthap_minamodapi)
target_include_directories(mthap_minamodapi SYSTEM INTERFACE "${minamodapi_SOURCE_DIR}")
