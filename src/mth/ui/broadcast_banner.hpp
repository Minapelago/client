#pragma once

namespace mth
{

class BannerQueue;

// Draws the active BannerQueue message in the upper-right corner on the ImGui foreground draw list, so it never captures input. Drawn every frame
// regardless of the dev-console toggle.
class BroadcastBanner
{
  public:
    explicit BroadcastBanner(BannerQueue &queue) : queue_(queue)
    {
    }

    void draw(); // call between ImGui::NewFrame() and Render()

  private:
    BannerQueue &queue_;
};

} // namespace mth
