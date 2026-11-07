#include "mth/core/rules/progression.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/ap/ap_state.hpp"

namespace mth
{

void StatCapState::recompute(const ApState &state)
{
    for (int s = 0; s < kStatCount; ++s)
        counts_[s] = 0;
    for (const auto &it : state.received_items())
    {
        if (!is_stat_cap_item(it.item_id))
            continue;
        if (is_stat_cap_all_item(it.item_id)) // one receipt raises every stat's cap
            for (int s = 0; s < kStatCount; ++s)
                ++counts_[s];
        else
            ++counts_[stat_cap_item_stat(it.item_id)];
    }
}

void StatCapState::set_counts(int attack, int defense, int sidearm)
{
    counts_[0] = attack;
    counts_[1] = defense;
    counts_[2] = sidearm;
}

int StatCapState::granted(int stat) const
{
    if (stat < 0 || stat >= kStatCount)
        return 0;
    return counts_[stat];
}

int StatCapState::enforced_cap(int stat, int vanilla_cap) const
{
    if (stat < 0 || stat >= kStatCount)
        return vanilla_cap;
    return std::min(vanilla_cap, counts_[stat]);
}

namespace
{
// Appends "<open><cap><close>" to the first line, replacing a suffix of the same shape that is
// already there so re-applying every frame is stable. `close` may be empty, in which case the run of
// digits after the last `open` is what gets replaced.
std::string with_cap_suffix(const std::string &text, int display_cap, const char *open, const char *close)
{
    if (text.empty())
        return text;

    const std::size_t eol = text.find('\n');
    std::string head = text.substr(0, eol == std::string::npos ? text.size() : eol);
    const std::string tail = eol == std::string::npos ? std::string() : text.substr(eol);
    if (head.empty())
        return text; // no title line to annotate

    // Only a bare number is ours to replace. Callers annotate the three real stats only, whose first line is "<Stat> Level <n>"; the bone-bank
    // row, the one description ending in a parenthesised number, never reaches here.
    const std::size_t close_len = std::strlen(close);
    if (head.size() >= close_len && head.compare(head.size() - close_len, close_len, close) == 0)
    {
        const std::size_t open_pos = head.rfind(open);
        if (open_pos != std::string::npos)
        {
            const std::size_t first = open_pos + std::strlen(open);
            const std::size_t last = head.size() - close_len;
            bool numeric = last > first;
            for (std::size_t i = first; i < last && numeric; ++i)
                numeric = head[i] >= '0' && head[i] <= '9';
            if (numeric)
                head.erase(open_pos);
        }
    }

    return head + open + std::to_string(display_cap) + close + tail;
}
} // namespace

std::string boneup_with_cap_suffix(const std::string &text, int display_cap)
{
    return with_cap_suffix(text, display_cap, " (", ")");
}

std::string status_panel_with_cap_suffix(const std::string &text, int display_cap)
{
    // Two digits fit the panel's remaining width, so a slot at the game's absolute ceiling shows 99 without wrapping the label out of the box.
    return with_cap_suffix(text, std::min(display_cap, 99), "/", "");
}

void UpgradeState::recompute(const ApState &state)
{
    int next[kUpgradeCount]{};
    for (const auto &it : state.received_items())
        if (is_capacity_upgrade_item(it.item_id))
            ++next[upgrade_index(it.item_id)];
    for (int i = 0; i < kUpgradeCount; ++i)
        next[i] = std::min(next[i], kUpgradeCaps[i]);

    // OR (not assign) so a force_dirty re-arm survives an unchanged-count recompute; mark_applied clears it.
    dirty_ = dirty_ || !std::equal(next, next + kUpgradeCount, applied_);
    std::copy(next, next + kUpgradeCount, counts_);
}

void UpgradeState::mark_applied()
{
    std::copy(counts_, counts_ + kUpgradeCount, applied_);
    dirty_ = false;
}

} // namespace mth
