#pragma once

#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mth
{

// rgba is IM_COL32 layout (R|G<<8|B<<16|A<<24) so the UI draws it without conversion; no ImGui dep here.
struct BannerSegment
{
    std::string text;
    std::uint32_t rgba{};
};

// PrintJSON types addressed to the room (chat, /say, countdowns, command replies). They carry no slot to match, so the slot gate would drop them.
[[nodiscard]] bool broadcast_is_announcement(std::string_view print_type);

// Relevant when the message is an announcement, or our slot is the actor (`slot`), item receiver (`receiving`) or item finder (`item_player`, a
// check we sent). The team must match; an absent team passes (single-team default).
[[nodiscard]] bool broadcast_relevant(std::string_view print_type, int our_team, int our_slot, std::optional<int> team, std::optional<int> slot,
                                      std::optional<int> receiving, std::optional<int> item_player);

// AP-palette color for a PrintJSON node, mirroring apclientpp render_json (explicit color wins, else by
// type/flags). Alpha is 255; the fade scales it.
[[nodiscard]] std::uint32_t banner_color(std::string_view type, std::string_view explicit_color, unsigned item_flags, unsigned hint_status, bool is_self);

// Attribution line for an inbound deathlink. By AP convention `cause` is a full sentence naming the dead player and is shown verbatim; `source`
// is used only when there is no cause.
[[nodiscard]] std::string deathlink_banner_text(std::string_view source, std::string_view cause);

struct BannerFrame
{
    std::vector<BannerSegment> segments;
    float alpha{}; // 0..1
};

// Thread-safe FIFO showing up to kMaxVisible messages at once: push() on the producer thread, update(now) on the render thread. Each message has
// its own hold+fade timer and waits for a free slot plus the promotion interval, so bursts stagger in. `now` is monotonic seconds (e.g.
// ImGui::GetTime()), injected for tests.
class BannerQueue
{
  public:
    static constexpr double kHoldSeconds = 5.0;            // fully opaque
    static constexpr double kFadeSeconds = 1.0;            // then fades to gone
    static constexpr double kPromoteIntervalSeconds = 1.0; // spacing between two messages appearing
    static constexpr int kMaxVisible = 5;                  // messages shown stacked at once

    void push(std::vector<BannerSegment> segments);

    // Advances the queue against `now` and returns the active messages to draw (oldest first), each with its
    // own alpha. Empty when idle. The renderer stacks them top-to-bottom.
    [[nodiscard]] std::vector<BannerFrame> update(double now);

  private:
    struct Active
    {
        std::vector<BannerSegment> segments;
        double start{0.0};
    };
    std::mutex mutex_;
    std::deque<std::vector<BannerSegment>> pending_;
    std::deque<Active> active_;                                     // render-thread only (under mutex); oldest at front, <= kMaxVisible
    double next_promote_{-std::numeric_limits<double>::infinity()}; // first message after an idle spell is never held back
};

} // namespace mth
