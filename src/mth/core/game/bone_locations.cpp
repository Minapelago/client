#include "mth/core/game/bone_locations.hpp"

#include <algorithm>
#include <iterator>

namespace mth
{

namespace
{

constexpr BoneRow kRows[] = {
#include "mth/core/game/bone_table.inc"
};

} // namespace

std::span<const BoneRow> bone_rows()
{
    return kRows;
}

const BoneRow *find_bone(std::uint64_t key)
{
    const auto it = std::ranges::lower_bound(kRows, key, {}, &BoneRow::key);
    return it != std::end(kRows) && it->key == key ? &*it : nullptr;
}

} // namespace mth
