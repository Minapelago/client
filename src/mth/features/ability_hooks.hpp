#pragma once

#include <cstdint>
#include <functional>

#include "mth/core/ap/ability_ids.hpp"
#include "mth/core/rules/ability_gate.hpp"
#include "mth/core/rules/burrow_boundary.hpp"
#include "mth/features/scene_gates/ticket_machine_gate.hpp"

namespace mth
{
class ApState;

// twin: mth/core/rules/ability_gate.hpp (pure gate decision).
// Drives the PAL ability-gate seam from AP state. The ctor's block lambda runs on the game thread from the detours, and enforce_/gate_ are written only on the
// game-thread tick, so plain members suffice.
class AbilityHooks
{
  public:
    explicit AbilityHooks(std::function<bool(std::int64_t item_id)> is_granted);
    AbilityHooks(const AbilityHooks &) = delete;
    AbilityHooks &operator=(const AbilityHooks &) = delete;

    void set_randomized(Ability a, bool on);
    void set_enforce(bool on); // authed && active save slot is the AP slot
    // Per-destination train gating (train_rando, #98): builds the granted-line mask from received ticket itemTypes, and enforce_train_tick clamps the SaveSlot
    // unlocked-lines bitfield to it so a station visit does not unlock its destination.
    void set_train_gate(bool rando_active, const ApState &state);
    // The station donation machine sells the same unlock as the AP Train Pass (#162). If the apworld carries it as a location it becomes a cheap check:
    // progress_seed pre-pays the goal down to the configured cost, and the OnPickupDone suppression withholds the pass and sends the check. It is made
    // non-interactible when the seed lacks it, or after the check, since a suppressed grant leaves IsItemCollected(149) false and it would re-dispense forever.
    void set_ticket_machine(bool is_ap_location, bool checked, std::uint32_t progress_seed);
    void enforce_train_tick(); // game-thread; clamps train destinations (rando) or the train-present byte
    // Burrow and Swim are separate items, but the game switches modes mid-burrow on a path the SetBurrowGround gate never sees (#163). Polls the boundary each
    // tick and undoes a crossing into an unowned mode: drown in deep water without Swim, surface on land without Burrow.
    void enforce_burrow_tick(void *player);
    void on_world_destroy(); // drop the burrow arm: a room load ends a burrow without an emerge commit

  private:
    std::function<bool(std::int64_t)> is_granted_;
    AbilityGate gate_;
    BurrowBoundaryGate burrow_;
    TicketMachineGate ticket_machine_;
    void *last_player_{nullptr}; // burrow arm is per-player; a changed/null pointer disarms
    bool enforce_{false};
    bool train_rando_active_{false};
    std::uint32_t train_mask_{0};
    bool ticket_machine_suppress_{false}; // false: a live check priced by the seed
    bool warned_seed_withheld_{false};
    std::uint32_t ticket_machine_seed_{0};
};

} // namespace mth
