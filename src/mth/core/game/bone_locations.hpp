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

} // namespace mth
