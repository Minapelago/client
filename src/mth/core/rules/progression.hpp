#pragma once

#include <optional>
#include <string>

#include "mth/core/ap/ap_ids.hpp" // kStatCount
#include "mth/core/ap/ap_state.hpp"

namespace mth
{

// slot_data "max_stat_level": per-stat level ceiling, bounded to the game-supported [10, 99]. Absent
// slot_data already arrives as 99 (the game's absolute max). Pure.
[[nodiscard]] constexpr int clamp_max_stat_level(int v) noexcept
{
    return v < 10 ? 10 : (v > 99 ? 99 : v);
}

// Ceiling fed to StatCapState::enforced_cap: real stats (0..<kStatCount) take the slot_data max_stat_level,
// replacing the game's native cap; bone bank / out-of-range keep vanilla_cap. Pure.
[[nodiscard]] constexpr int stat_cap_ceiling(int stat, int max_stat_level, int vanilla_cap) noexcept
{
    return (stat >= 0 && stat < kStatCount) ? max_stat_level : vanilla_cap;
}

// Windows bone-up gate: present the cursor-selected at-cap real stat as maxed to the inlined LevelUpMenu buy-gate, only in the interactive state,
// so the entry gate sees the real level and the menu opens when every stat is capped. Only the selected stat: the commit handler re-applies the
// defense level (SaveSlot+0x178) to combat and the bank/level rows skip the buy-gate, so faking an unselected stat pins combat defense to the
// sentinel. Pure.
[[nodiscard]] constexpr bool boneup_fake_capped_stat(bool menu_interactive, bool stat_is_selected, int real_level, int cap) noexcept
{
    return menu_interactive && stat_is_selected && real_level >= cap;
}

// The buy-gate compares the raw level (`raw < cap`) but the bone-up menu renders raw+1, so an
// enforced cap of N tops out at a displayed level of N+1. Vanilla cross-check: the native cap is 9
// and the level AnimNumber clamps at 10.
[[nodiscard]] constexpr int boneup_display_cap(int enforced_cap) noexcept
{
    return enforced_cap + 1;
}

// Description text with " (N)" appended to its first line. An existing numeric parenthesised suffix is replaced, so re-applying every frame is
// stable and a cap change while the menu is open takes effect. Empty text is left alone.
[[nodiscard]] std::string boneup_with_cap_suffix(const std::string &text, int display_cap);

// Pause-panel label with "/N" appended to its first line, N clamped to two digits. The level widget wraps at a width "LVL 14 (14)" already
// exceeds, and a wrapped block anchors on its last line, so overflow renders above the panel box; "/N" keeps a two-digit level and cap inside.
// Stale suffixes are replaced.
[[nodiscard]] std::string status_panel_with_cap_suffix(const std::string &text, int display_cap);

// twin: mth/features/levelcap_hooks.hpp enforces this in-game.
// Per-stat level-cap policy, derived from received AP "cap up" items. Pure.
// The game's per-stat buy-gate is `current_level < cap`; we feed it min(vanilla_cap, granted-count),
// so with 0 cap-ups a stat is frozen at its starting level and each cap-up unlocks one more level.
class StatCapState
{
  public:
    // Recompute per-stat granted counts from the AP received-items list. Idempotent; safe every tick.
    void recompute(const ApState &state);

    // Offline test seam: set the granted counts directly (bypasses AP).
    void set_counts(int attack, int defense, int sidearm);

    // The cap to enforce for `stat` given the active save's vanilla cap, in game units and never above it. stat outside [0,3) -> vanilla_cap.
    [[nodiscard]] int enforced_cap(int stat, int vanilla_cap) const;

    // Raw granted count for a stat (diagnostics). stat out of [0,3) -> 0.
    [[nodiscard]] int granted(int stat) const;

  private:
    int counts_[3]{0, 0, 0};
};

// twin: mth/features/upgrades.hpp (apply_upgrades pushes counts() to the save).
// Counts received capacity-upgrade items (68..72) per type, clamped to each cap. apply_upgrades
// pushes counts() to the save; dirty() gates re-applying so the owner only writes on change.
class UpgradeState
{
  public:
    void recompute(const ApState &state); // refresh clamped per-type counts from received items

    [[nodiscard]] const int *counts() const // [kUpgradeCount], index = upgrade_index()
    {
        return counts_;
    }
    [[nodiscard]] bool dirty() const // counts changed since the last mark_applied()
    {
        return dirty_;
    }
    void mark_applied(); // counts have been pushed to the game; clears dirty until they change again

    // Re-arms a re-apply without a count change: the kit suppressor zeroed the upgrade bitfields (on a SaveSlot::Clear, which also fires on
    // profile-menu / save-load), so counts() must be re-asserted. Sticky until mark_applied lands the re-apply.
    void force_dirty()
    {
        dirty_ = true;
    }

  private:
    int counts_[kUpgradeCount]{};
    int applied_[kUpgradeCount]{}; // last counts pushed to the game; 0 = nothing applied (matches a fresh save)
    bool dirty_{false};
};

// slot_data "wallet_cap": the bone wallet is capped by the number of received wallet items
// (kProgWalletId). Base 750, +500 per item, and uncapped once kWalletFreeAt items are held.
inline constexpr int kWalletBase = 750;
inline constexpr int kWalletStep = 500;
inline constexpr int kWalletFreeAt = 8;

// Per-item cap: nullopt once count >= kWalletFreeAt (no cap), else kWalletBase + kWalletStep*count. Pure.
[[nodiscard]] constexpr std::optional<int> wallet_cap_for(int count) noexcept
{
    if (count >= kWalletFreeAt)
        return std::nullopt;
    if (count < 0)
        count = 0;
    return kWalletBase + kWalletStep * count;
}

// twin: enforce_wallet_cap (features/upgrades.cpp) clamps live bones to this each frame.
// The bone cap the received wallet items allow, or nullopt for "uncapped".
[[nodiscard]] inline std::optional<int> wallet_cap(const ApState &state)
{
    int count = 0;
    for (const auto &it : state.received_items())
        if (is_wallet_item(it.item_id))
            ++count;
    return wallet_cap_for(count);
}

} // namespace mth
