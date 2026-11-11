#pragma once

#include <cstdint>
#include <span>

#include "mth/core/ap/ap_ids.hpp"

namespace mth
{

struct BoneRow
{
    std::uint64_t key;    // spawn-point name hash, what component_name_key reads off the Pickup
    std::uint16_t index;  // frozen; location id = kBoneLocBase + index
    std::uint32_t screen; // room_tracker screen id, (gamestate << 16) | room
    std::uint16_t x;      // level pixels, y down
    std::uint16_t y;
    bool excluded; // never a location; only the labels read it
};

[[nodiscard]] std::span<const BoneRow> bone_rows();        // all rows, sorted by key
[[nodiscard]] const BoneRow *find_bone(std::uint64_t key); // null for 0 and any key not in the table
[[nodiscard]] constexpr int bone_location_id(const BoneRow &row) noexcept
{
    return kBoneLocBase + row.index;
}

inline constexpr int kFirstBoneItemType = 40; // kItemType_Treasure_Smallest
inline constexpr int kLastBoneItemType = 50;  // BoneDust; 40..50 are all item kind 6

[[nodiscard]] constexpr bool is_bone_pickup(int loc_idx, int item_type) noexcept
{
    return loc_idx == -1 && item_type >= kFirstBoneItemType && item_type <= kLastBoneItemType;
}
// in_progress_item_type is the marked pickup's live type, -1 for none.
[[nodiscard]] constexpr bool is_bone_grant(int slot, int item_type, int in_progress_item_type) noexcept
{
    return slot == -1 && item_type == in_progress_item_type && is_bone_pickup(-1, item_type);
}
[[nodiscard]] constexpr bool should_destroy_bone(bool checked, bool natively_killed) noexcept
{
    return checked && !natively_killed;
}

} // namespace mth
