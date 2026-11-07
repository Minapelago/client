#include "mth/core/ap/area_reporter.hpp"

#include "mth/core/ap/ap_link.hpp"

namespace mth
{

AreaReporter::AreaReporter(IApLink &link) : link_(link)
{
}

void AreaReporter::tick(bool connected, std::optional<std::uint32_t> screen_id)
{
    if (!connected)
    {
        last_sent_.reset(); // republish after a reconnect
        return;
    }
    if (!screen_id)
        return;
    if (last_sent_ == screen_id)
        return;
    last_sent_ = screen_id;
    link_.report_area(static_cast<int>(*screen_id));
}

} // namespace mth
