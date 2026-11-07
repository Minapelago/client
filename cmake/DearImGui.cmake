# DearImGui.cmake - Dear ImGui via FetchContent, built as an OBJECT lib. The
# backend pair is selected per platform: Vulkan + hand-rolled SDL input on Linux,
# the stock D3D12 + Win32 backends on Windows. Vendored third-party: warnings
# silenced, not held to our -Werror policy.
include_guard(GLOBAL)
include(FetchContent)

FetchContent_Declare(
    dearimgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(dearimgui)

add_library(mthap_imgui OBJECT
    "${dearimgui_SOURCE_DIR}/imgui.cpp"
    "${dearimgui_SOURCE_DIR}/imgui_draw.cpp"
    "${dearimgui_SOURCE_DIR}/imgui_tables.cpp"
    "${dearimgui_SOURCE_DIR}/imgui_widgets.cpp"
)
target_include_directories(mthap_imgui SYSTEM PUBLIC
    "${dearimgui_SOURCE_DIR}"
    "${dearimgui_SOURCE_DIR}/backends"
)

if(WIN32)
    # Windows: stock D3D12 + Win32 backends. They include only <d3d12.h>,
    # <dxgi*.h>, <windows.h> - all provided by the MSVC SDK and by mingw-w64,
    # so no third-party find_package is needed (unlike the Vulkan/SDL branch).
    target_sources(mthap_imgui PRIVATE
        "${dearimgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp"
        "${dearimgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    )
    # Skip the XInput gamepad path (avoids the xinput import lib + runtime dep).
    target_compile_definitions(mthap_imgui PUBLIC IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
    # d3dcompiler: imgui_impl_dx12 compiles its shaders at init via D3DCompile.
    # dxguid: COM IID_* symbols. dwmapi: imgui_impl_win32's alpha-compositing
    # helper calls Dwm*. clang-cl picks these up via #pragma comment(lib) too,
    # but mingw ignores those pragmas, so list them explicitly for both.
    target_link_libraries(mthap_imgui PUBLIC d3d12 dxgi dxguid d3dcompiler dwmapi user32)
else()
    # Linux: Vulkan backend; SDL2 headers only (input is hand-rolled). We never
    # link the SDL2 library - the game has its own static SDL. Both header sets
    # are fetched at the versions vcpkg's baseline resolves and are never built.
    FetchContent_Declare(
        sdl2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-2.32.10
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _headers_only
    )
    FetchContent_Declare(
        vulkan-headers
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG        vulkan-sdk-1.4.350.0
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _headers_only
    )
    FetchContent_MakeAvailable(sdl2 vulkan-headers)

    target_sources(mthap_imgui PRIVATE "${dearimgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")

    # We resolve Vulkan entry points at runtime from the game's loader; the
    # backend must not assume linked prototypes.
    target_compile_definitions(mthap_imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
    target_include_directories(mthap_imgui SYSTEM PUBLIC "${vulkan-headers_SOURCE_DIR}/include")

    target_include_directories(mthap_imgui SYSTEM PUBLIC "${sdl2_SOURCE_DIR}/include")

    # Pin ImGui's libm float calls to ancient glibc symbol versions so the
    # LD_PRELOAD .so still loads under the older Steam Linux Runtime glibc.
    target_compile_options(mthap_imgui PRIVATE -include "${CMAKE_CURRENT_LIST_DIR}/glibc_compat.h")
endif()

set_target_properties(mthap_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Vendored third-party: silence its warnings (we are -Werror elsewhere).
if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(mthap_imgui PRIVATE /W0)
else()
    target_compile_options(mthap_imgui PRIVATE -w)
endif()

add_library(mthap::imgui ALIAS mthap_imgui)
