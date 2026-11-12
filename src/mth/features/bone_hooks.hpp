#pragma once

namespace mth
{
class RandoBridge;
}

namespace mth::bone_hooks
{

// Game thread only.
void on_pickup(void *pickup);                          // PickupOnPickup, before the original: marks a bone pickup, clears the mark otherwise
[[nodiscard]] int take_grant(int slot, int item_type); // ItemsOnPickup: the bone location id when this pays out the marked pickup, else -1
void on_pickup_init(void *pickup);                     // Pickup::Init: a reused, re-Inited marked pickup is no longer in progress
void draw_labels(const RandoBridge &bridge);           // WorldUpdateEnd of the gamestate World: ids over each bone in the room, when on
void set_labels(bool on);                              // toggles draw_labels
void tick();                                           // once per game tick: clears the mark

} // namespace mth::bone_hooks
