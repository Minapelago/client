# MinHook.cmake - fetch MinHook from upstream and expose as `minhook::minhook`.
#
# MinHook is the hook backend on Windows (MinGW cross-compile friendly). Upstream: https://github.com/TsudaKageyu/minhook. Its own CMakeLists.txt
# makes FetchContent_MakeAvailable produce a static library directly.

set(MINHOOK_VERSION "v1.3.4")

include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        ${MINHOOK_VERSION}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(minhook)

# Upstream's target is `minhook`; the namespaced alias keeps consumers on minhook::minhook.
add_library(minhook::minhook ALIAS minhook)

message(STATUS "MinHook: ${MINHOOK_VERSION} -> target 'minhook'")
