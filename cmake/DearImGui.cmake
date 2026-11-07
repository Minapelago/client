# DearImGui.cmake - Dear ImGui via FetchContent, built as an OBJECT lib. Backends: Vulkan + hand-rolled SDL input on Linux, stock D3D12 + Win32 on
# Windows. Vendored third-party: warnings silenced, outside our -Werror policy.
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
    # Windows: stock D3D12 + Win32 backends. They include only <d3d12.h>, <dxgi*.h> and <windows.h>, which the MSVC SDK and mingw-w64 provide.
    target_sources(mthap_imgui PRIVATE
        "${dearimgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp"
        "${dearimgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    )
    # Skip the XInput gamepad path (avoids the xinput import lib and runtime dep).
    target_compile_definitions(mthap_imgui PUBLIC IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
    # d3dcompiler: imgui_impl_dx12 compiles shaders at init via D3DCompile. dxguid: COM IID_* symbols. dwmapi: imgui_impl_win32's Dwm* calls.
    # clang-cl also gets these from #pragma comment(lib), but mingw ignores pragmas.
    target_link_libraries(mthap_imgui PUBLIC d3d12 dxgi dxguid d3dcompiler dwmapi user32)
else()
    # Linux: Vulkan backend plus SDL2 headers only (input is hand-rolled); the game has its own static SDL, so the SDL2 library is never linked.
    # Both header sets come at the versions vcpkg's baseline resolves and are never built.
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

    # Vulkan entry points resolve at runtime from the game's loader; the backend assumes no linked prototypes.
    target_compile_definitions(mthap_imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
    target_include_directories(mthap_imgui SYSTEM PUBLIC "${vulkan-headers_SOURCE_DIR}/include")

    target_include_directories(mthap_imgui SYSTEM PUBLIC "${sdl2_SOURCE_DIR}/include")

    # Pin ImGui's libm float calls to old glibc symbol versions so the LD_PRELOAD .so loads under the older Steam Linux Runtime glibc.
    target_compile_options(mthap_imgui PRIVATE -include "${CMAKE_CURRENT_LIST_DIR}/glibc_compat.h")
endif()

set_target_properties(mthap_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Vendored third-party: silence its warnings (we use -Werror elsewhere).
if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(mthap_imgui PRIVATE /W0)
else()
    target_compile_options(mthap_imgui PRIVATE -w)
endif()

add_library(mthap::imgui ALIAS mthap_imgui)
