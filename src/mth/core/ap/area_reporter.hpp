#pragma once

#include <cstdint>
#include <optional>

namespace mth
{

class IApLink;

// Publishes the packed screen id (area<<16 | room) to AP data storage when it changes while connected.
// A disconnect clears the last-sent value, so the first connected tick republishes for the reconnecting tracker. Pure.
class AreaReporter
{
  public:
    explicit AreaReporter(IApLink &link);

    // Call once per fixed update. `screen_id` is empty when not yet readable.
    void tick(bool connected, std::optional<std::uint32_t> screen_id);

  private:
    IApLink &link_;
    std::optional<std::uint32_t> last_sent_;
};

} // namespace mth
