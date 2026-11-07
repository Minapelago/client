#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mth/features/scene_gates/scene_walk.hpp"

namespace mth
{
class ApState;

// Demotes the intro weapon chest from starter mode to the ordinary weapon-change mode. Vanilla, it offers all three default weapons and its confirm writes the
// SaveSlot ownership fields, a free weapon on top of the seed's starter; clamping afterwards leaves that weapon equipped and still offered. The ordinary mode
// lists owned weapons only, equips the pick and grants nothing.
//
// The mode is one byte on the chest, copied into the menu on open, so both are cleared. A CheckpointChest is an UnderlabUpgrade (not a Chest) with no named
// hook or carved signature, so it is found by scene walk on its RTTI type id.
//
// The walk repeats on a slow cadence because the intro NPC re-arms the byte from its state machine. Nothing is walked unless armed, and no game pointer is held
// across ticks.
class IntroChestGate
{
  public:
    // Clamps weapon ownership to the AP-granted tiers (bound AP save only, since the write is durable) and arms the chest demotion. Game thread, per tick.
    void enforce_weapon_grants(const ApState &state, bool authed, bool slot_ok);

    void tick();             // game thread; walks nothing unless armed
    void on_world_destroy(); // re-walk on the next tick

  private:
    void demote_chest(void *chest);

    bool armed_{false};          // published by the game-thread tick, read by the game-thread drain
    bool logged_demoted_{false}; // Info on the first demotion so the silent write shows up, Debug after
    SceneWalker walker_{"intro", "the weapon chest"};
};

} // namespace mth
