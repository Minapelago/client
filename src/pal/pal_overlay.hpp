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

// Linux: Vulkan/SDL overlay. Windows: D3D12/Win32 overlay. overlay_start installs the render and input hooks and draws ui every frame. Each platform
// resolves its own game symbols.
void overlay_start(IOverlayUi *ui);

} // namespace pal
