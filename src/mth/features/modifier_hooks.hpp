#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "mth/core/game/modifiers.hpp"

namespace mth
{

// Owns the modifier hooks and enforcement policy. Installs whenever modifiers are available; the hooks no-op while disarmed. Armed from construction so an AP
// session enforces the deny list; `modifiers unlock` overrides. Game thread only: the detours, the tick and the console all run there.
class ModifierHooks
{
  public:
    // `on_save_loaded` runs on the game thread when a save slot is activated by a real load (title/profile-menu re-activations skip it). It rides the
    // ActivateSaveSlot hook.
    explicit ModifierHooks(std::function<void()> on_save_loaded);
    ModifierHooks(const ModifierHooks &) = delete;
    ModifierHooks &operator=(const ModifierHooks &) = delete;

    // Console/AP seams.
    void set_armed(bool on);
    void set_live(int idx, bool on); // writes the save mask and rebuilds the runtime mirror now
    // Gates both seed and lockdown; off for vanilla play (not connected, not test mode).
    void set_enforce_live(bool on);
    // True (authed AP session): scope the seed to the AP game's slot, captured on first load.
    void set_ap_scoped(bool on);
    // slot_data seam: add/remove Landing Done from the AP force-on set (Warp Home is always forced).
    void set_ossex_start(bool on);
    // AP-game slot persistence, bridged to ApSaveState by App. A recorded slot skips capture and -1 captures again; reads back this session's slot.
    void set_ap_slot(int slot);
    [[nodiscard]] int captured_ap_slot() const;
    [[nodiscard]] std::vector<std::string> status_lines() const;

  private:
    void seed(int slot_index, std::uint32_t words[8]); // PAL seed callback
    bool block(int idx) const;                         // PAL lockdown callback

    std::set<int> force_on_{kCheatWarpHome, kCheatCheaperBoneUp, kCheatUnlockBoneUps}; // additive AP force-on (Warp Home always; Landing Done per ossex_start)
    bool armed_{true};
    int ap_slot_{-1};          // captured AP-game slot index (-1 = none yet)
    bool enforce_live_{false}; // AP session active (or offline test mode); gates seed + lockdown
    bool ap_scoped_{false};    // scope the seed to a single (AP) slot vs permissive test mode
};

} // namespace mth
