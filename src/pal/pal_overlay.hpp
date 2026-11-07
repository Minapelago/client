#pragma once

#include <cstdint>

namespace pal
{

// Per-frame visibility flags, published by the platform overlay from its toggle-key atomics.
struct OverlayVisibility
{
    bool console_open{false};
    bool login_open{false};
};

// Content sink: draw() is called every frame between NewFrame() and Render().
class IOverlayUi
{
  public:
    virtual ~IOverlayUi() = default;
    virtual void draw(const OverlayVisibility &vis) = 0;
};

// Linux: Vulkan/SDL overlay. Windows: D3D12/Win32 overlay. overlay_start installs the render and input
// hooks and draws ui every frame; overlay_stop removes the hooks and releases the ImGui context and GPU state.
// Each platform resolves whatever game symbols it needs itself: ProcessSDLEvent is Linux-only, and
// routing it through here made Windows carry a signature for a symbol it never reads.
void overlay_start(IOverlayUi *ui);
void overlay_stop();

} // namespace pal
