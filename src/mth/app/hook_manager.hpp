#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "mth/features/ability_hooks.hpp"
#include "mth/features/death_hooks.hpp"
#include "mth/features/levelcap_hooks.hpp"
#include "mth/features/location_hooks.hpp"
#include "mth/features/lock_hooks.hpp"
#include "mth/features/menus/status_menu_caps.hpp"
#include "mth/features/modifier_hooks.hpp"
#include "mth/features/save_pollers.hpp"
#include "mth/features/save_takeover.hpp"
#include "mth/features/scene_gates/intro_chest_gate.hpp"
#include "mth/features/scene_gates/sewer_cat_gate.hpp"

namespace mth
{

class RandoBridge;
class ApState;
struct SessionPolicy;
class ApSaveBundleStore;

// Owns the feature hooks and drives their slice of the per-frame tick.
class HookManager
{
  public:
    HookManager(RandoBridge &rando, const std::map<int, ScoutInfo> &scout, ApState &state, ApSaveBundleStore &bundle,
                std::function<void(const std::string &)> send_death, std::function<void *()> get_player);

    HookManager(const HookManager &) = delete;
    HookManager &operator=(const HookManager &) = delete;

    void tick(ApState &state, const SessionPolicy &policy, int save_game_slot); // -1 when no save state
    // Session boundary: drop the lock removals seeded from one connection's items and the captured AP-game slot, so neither reaches the next save.
    void clear_session_state();
    void drain();                          // World::Update pre-hook window
    void on_world_update_end(void *world); // menu-world scene walk for the save takeover
    void on_world_destroy();               // re-arm native collected-bit enforcement (save reload clears it)
    void append_status_lines(std::vector<std::string> &out) const;

  private:
    bool vendor_locked(const ApState &state); // arms vendor_lockout_ while connected, then reports it

    // Set the first time a save sees an AP session and cleared only on save load, so a mid-run disconnect cannot reopen the off-logic vendors.
    std::atomic<bool> vendor_lockout_{false};

    RandoBridge &rando_;                 // checked-location state; the donation machine reads it (#162)
    std::function<void *()> get_player_; // live Player* accessor for the burrow tick

  public:
    // App reaches the features directly for the console and session seams. A feature that references another is declared after it.
    LocationHooks location_hooks_;
    BossTracker boss_tracker_;
    GoalTracker goal_tracker_;
    LockHooks lock_hooks_;
    DeathHooks death_hooks_;
    AbilityHooks ability_hooks_;
    SewerCatGate sewer_cat_gate_; // reads vendor_lockout_
    IntroChestGate intro_chest_gate_;
    ModifierHooks modifier_hooks_; // its save-loaded callback reads boss_tracker_ and vendor_lockout_
    LevelCapHooks level_cap_hooks_;
    StatusMenuCaps status_menu_caps_; // holds level_cap_hooks_
    SaveTakeover save_takeover_;      // the title gate's StartGame detour calls it
};

} // namespace mth
