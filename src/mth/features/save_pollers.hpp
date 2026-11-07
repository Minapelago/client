#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Per-tick SaveSlot pollers. None of them installs a detour, so both platforms share them unchanged.
namespace mth
{
class RandoBridge;
class ApState;

// twin: mth/core/rules/goal.hpp (pure goal_met()).
// Polls the active SaveSlot each tick and fires the AP goal when the slot_data-configured condition is met:
// finish = the game-clear flag; generators/bosses = popcount of the matching SaveSlot bitfield >= threshold.
// Read-only; send_goal() is one-shot in the bridge.
class GoalTracker
{
  public:
    explicit GoalTracker(RandoBridge &bridge);

    void evaluate(const ApState &state); // game thread, per tick while authenticated

    [[nodiscard]] std::vector<std::string> status_lines() const;

  private:
    RandoBridge &bridge_;
    bool disabled_{false}; // latched by an implausible game-clear read; never polls again

    // Last poll, retained for status_lines().
    int config_{0};
    int gens_done_{0};
    int gens_needed_{99};
    std::uint64_t broken_mask_{0};
    int bosses_done_{0};
    int bosses_needed_{99};
    bool game_cleared_{false};
};

// Outbound boss-defeat checks by polling the SaveSlot+0x280 defeat bitmask instead of detouring the
// death funnels. The bit index is the BossComponent index, so bit i maps straight to the same AP
// location the funnels used. Read-only.
// Only live kills count. The baseline is re-taken on every real save activation, because the game
// keeps ONE working-slot buffer: loading a save refills it in place, so neither the slot pointer nor
// a live Player distinguishes "the run's save arrived" from "a boss just died". Loading a save with
// bosses already down otherwise reported every one of them as a fresh kill.
class BossTracker
{
  public:
    explicit BossTracker(RandoBridge &bridge);

    void poll();           // game thread, per tick
    void on_save_loaded(); // game thread; a save was just activated, so re-baseline off its contents

  private:
    void prime(const void *slot, const char *why, std::uint64_t mask);

    RandoBridge &bridge_;
    const void *prev_slot_{nullptr};
    std::uint64_t prev_mask_{0};
    bool primed_{false};
};

// twin: mth/core/rules/kear_completion.hpp (pure have_all_kears()).
// Latches the KeyMiser trade bit once the run holds every kear its mode defines, so the game's own
// KeyReward path spawns the location-150 pickup and the normal pickup hook reports the check (#174).
// Writes one durable save bit. Game thread, per tick while authenticated.
void latch_kear_completion(const ApState &state);

} // namespace mth
