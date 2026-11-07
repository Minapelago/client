#include "mth/ui/overlay_root.hpp"

namespace mth
{

OverlayRoot::OverlayRoot(App &app, BannerQueue &banner_queue) : console_(app, banner_queue), login_(app)
{
}

void OverlayRoot::draw(const pal::OverlayVisibility &vis)
{
    console_.draw(vis.console_open);
    login_.draw(vis.login_open);
}

} // namespace mth
