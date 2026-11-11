#include "mth/features/bone_hooks.hpp"

#include "mth/core/game/bone_locations.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/features/name_key.hpp"
#include "pal/pal_mem.hpp"

namespace mth::bone_hooks
{

namespace
{
void *g_marked = nullptr; // the bone pickup whose OnPickup is running, until its grant, the next pickup or the tick
}

void on_pickup(void *pickup)
{
    const bool bone = is_bone_pickup(pal::at<int>(pickup, layout::kPickupLocIdxOff), pal::at<int>(pickup, layout::kPickupItemTypeOff));
    g_marked = bone ? pickup : nullptr;
}

int take_grant(int slot, int item_type)
{
    // Pickup::OnPickup calls Items::OnPickup only on its collect path, after the SaveSlot writes, so a buried bone's dig-up call never gets here.
    const int live_type = g_marked != nullptr ? pal::at<int>(g_marked, layout::kPickupItemTypeOff) : -1;
    if (!is_bone_grant(slot, item_type, live_type))
        return -1;
    void *pickup = g_marked;
    g_marked = nullptr;
    const BoneRow *row = find_bone(component_name_key(pickup));
    return row != nullptr ? bone_location_id(*row) : -1;
}

void on_pickup_init(void *pickup)
{
    if (g_marked == pickup)
        g_marked = nullptr;
}

void tick()
{
    g_marked = nullptr;
}

} // namespace mth::bone_hooks
