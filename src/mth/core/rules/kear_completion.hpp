#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

#include "mth/core/ap/ap_events.hpp"
#include "mth/core/ap/ap_ids.hpp"

namespace mth
{

// Collection index of the KeyMiser reward; ap_loc_id() maps it into the AP location id space.
inline constexpr int kKearCompletionLocIdx = 150;

// Vanilla kears are fungible and all carry the one Universal Kear id, so the gate counts receipts.
inline constexpr int kVanillaKearTotal = 40;

// Per-lock kear items (kear_rando=1): every lock in kAreaLocks except 2306, the second engine lock the 2304 item removes. Mirrors SingleKears in
// the apworld's data/items/kears.py.
inline constexpr auto kSingleKearItemIds = []
{
    std::array<std::int64_t, std::size(kAreaLocks) - 1> ids{};
    std::size_t n = 0;
    for (const AreaLock &lock : kAreaLocks)
        if (lock.lock_item_id != kMMSecondDoubleKearBlockID)
            ids.at(n++) = lock.lock_item_id; // throws, so fails to compile, unless 2306 is in the table
    return ids;
}();
inline constexpr int kSingleKearCount = static_cast<int>(kSingleKearItemIds.size());
// HasAllKears() clears at 30 of the 39, not the full set.
inline constexpr int kSingleKearRequired = 30;

namespace detail
{
[[nodiscard]] inline bool received_has(const std::vector<ReceivedItem> &received, std::int64_t id) noexcept
{
    return std::any_of(received.begin(), received.end(), [id](const ReceivedItem &it) { return it.item_id == id; });
}
} // namespace detail

// twin: latch_kear_completion in mth/features/save_pollers.hpp latches the save flag off this.
// True once the run holds the kears the active mode demands (#174). The apworld gates location 150 on
// the same condition, so this must track HasAllKears() in its data/rules/state_rules.py.
[[nodiscard]] inline bool have_all_kears(KearMode mode, const std::vector<ReceivedItem> &received) noexcept
{
    if (mode == KearMode::Vanilla)
    {
        constexpr std::int64_t kear = ap_item_id(kUniversalKearItemType);
        const auto n = std::count_if(received.begin(), received.end(), [](const ReceivedItem &it) { return it.item_id == kear; });
        return n >= kVanillaKearTotal;
    }

    if (mode == KearMode::ApItems)
    {
        // Counts distinct ids held, so a duplicate receipt can't stand in for a missing kear.
        const auto n = std::count_if(std::begin(kSingleKearItemIds), std::end(kSingleKearItemIds),
                                     [&received](std::int64_t id) { return detail::received_has(received, id); });
        return n >= kSingleKearRequired;
    }

    // Per-area kear items (kear_rando=2): every area item in kAreaLocks.
    return std::all_of(std::begin(kAreaLocks), std::end(kAreaLocks),
                       [&received](const AreaLock &lock) { return detail::received_has(received, lock.area_item_id); });
}

} // namespace mth
