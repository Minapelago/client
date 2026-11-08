#include "mth/features/upgrades.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

#include "mod/mod_api.hpp"
#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/game/game_symbols.hpp"
#include "pal/pal_game.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"
#include "pal/pal_module.hpp"
#include "pal/scoped_hook.hpp"

// Capacity-upgrade grants and the new-file starting-kit suppression. Every SaveSlot/Player/CombatCore offset below matches on both builds; the g_saveManager
// deref is behind pal::active_save_slot.

// ---- capacity upgrades ----
// Per-upgrade SaveSlot field (Magic, Health, Spark, Vial, Trinket); popcount = capacity, and UpdateStats recomputes the live maxima from them. Build-specific:
// re-verify on the shipping build. The Vial entry (kVialUpgradeIndex) is a placeholder and never written; vials go through the mod API (see App).
namespace
{
constexpr std::ptrdiff_t kUpgradeFieldOff[5] = {0x170, 0x130, 0x54, 0x18c, 0x950};

// Live resource-pool fields, so a capacity grant keeps the missing amount constant (build 9b29bd0d). CombatCore = *(Player+0x130); see re-note
// 2026-06-24-resource-current-max-fields. Trinket has no depleting pool.
constexpr std::ptrdiff_t kCombatCoreOff = 0x130; // Player -> CombatCore*
constexpr std::ptrdiff_t kHpCurOff = 0x1e0;      // CombatCore, float
constexpr std::ptrdiff_t kHpMaxOff = 0x1e8;      // CombatCore, float
// Joules (magic), pinned by the game's accessors: PlayerGetJoules/PlayerSetJoules use Player+0x117c and UpdateStats writes the max at +0x1180. The adjacent
// 0x1174/0x1178 are the backup and in-flight sidearm itemTypes; clamping those empties the backup slot on the next WriteSave (r148851).
constexpr std::ptrdiff_t kMagicCurOff = 0x117c; // Player, int
constexpr std::ptrdiff_t kMagicMaxOff = 0x1180; // Player, int
constexpr std::ptrdiff_t kSparkCurOff = 0x50;   // SaveSlot, int
constexpr std::ptrdiff_t kSparkMaxOff = 0x230;  // CombatCore, int
// Vials are not written here: their SaveSlot bitfield offset drifts between builds (#97), so App uses the offset-free mod::set_player_max_vials.

bool g_up_layout_ok = true; // cleared permanently if an upgrade field reads out of its plausible domain

// ---- new-file starting-kit suppression. MSVC's SaveSlot layout matches Linux on depot_1875582 (verified against Windows SetItemCollected's case map). ----
constexpr std::ptrdiff_t kSparkUpgOff = 0x54;    // Spark_Upgrade   (itemType 0x46)
constexpr std::ptrdiff_t kHealthUpgOff = 0x130;  // Health_Upgrade  (itemType 0x45) bitfield (0xff = 8)
constexpr std::ptrdiff_t kMagicUpgOff = 0x170;   // Magic_Upgrade   (itemType 0x44)
constexpr std::ptrdiff_t kTrinketUpgOff = 0x950; // Trinket_Upgrade (itemType 0x48)
// Vials are not zeroed here (offset drifts, #97). App::enforce_vial_capacity re-asserts the AP vial count via the mod API each tick, which also survives the
// run re-seeding the base (#171).

std::function<bool()> g_kit_suppress = nullptr;
[[clang::no_destroy]] pal::ScopedHook g_kit_hook;
void (*g_orig_save_slot_clear)(void *, bool) = nullptr;
void repl_save_slot_clear(void *self, bool arg)
{
    if (g_orig_save_slot_clear)
        g_orig_save_slot_clear(self, arg);
    if (self == nullptr || !g_kit_suppress || !g_kit_suppress())
        return;
    pal::logf(pal::LogLevel::Info, "newfile-kit: zeroing default upgrades (health=%#x magic=%#x spark=%#x trinket=%#x)",
              pal::at<std::uint32_t>(self, kHealthUpgOff), pal::at<std::uint32_t>(self, kMagicUpgOff), pal::at<std::uint32_t>(self, kSparkUpgOff),
              pal::at<std::uint32_t>(self, kTrinketUpgOff));
    pal::at<std::uint32_t>(self, kHealthUpgOff) = 0;
    pal::at<std::uint32_t>(self, kMagicUpgOff) = 0;
    pal::at<std::uint32_t>(self, kSparkUpgOff) = 0;
    pal::at<std::uint32_t>(self, kTrinketUpgOff) = 0;
}

} // namespace

namespace mth
{

bool apply_upgrades(const int *counts, void *player)
{
    // Diagnostic (#46): called every tick while dirty, so it logs only when the outcome changes, naming which guard drops the new-file start-inventory grant.
    static int s_last_outcome = -1;
    auto trace = [&](int outcome, const char *what, void *slot)
    {
        if (outcome == s_last_outcome)
            return;
        s_last_outcome = outcome;
        pal::logf(pal::LogLevel::Debug, "upgrades: apply -> %s (player=%p slot=%p counts=[%d,%d,%d,%d,%d])", what, player, slot, counts[0], counts[1],
                  counts[2], counts[3], counts[4]);
    };
    if (player == nullptr)
    {
        trace(2, "skip: player null", nullptr);
        return false;
    }
    if (!g_up_layout_ok)
    {
        trace(3, "skip: layout disabled", nullptr);
        return false;
    }
    // Checked before any write: the owned-bit fields do nothing until UpdateStats recomputes the maxima, so without it the grant would be half-applied.
    if (!mod::player_stats_api_available())
    {
        trace(6, "skip: PlayerUpdateStats unavailable", nullptr);
        return false;
    }
    void *slot = pal::active_save_slot();
    if (slot == nullptr)
    {
        trace(4, "skip: active SaveSlot* null", slot);
        return false;
    }
    // CombatCore is reached through the Player, so a non-canonical read means the Player is not one (#157 faulted here walking a freed Player). UpdateStats and
    // the magic-pool writes use the same pointer, so bail and leave the counts dirty for a retry. A null CombatCore is handled by the pool restores.
    void *cc = pal::at<void *>(player, kCombatCoreOff);
    if (cc != nullptr && !pal::pointer_looks_valid(cc))
    {
        trace(5, "skip: CombatCore* invalid", slot);
        return false;
    }

    // UpdateStats raises the max but never refills current, so each pool keeps its pre-grant missing amount (new_current = new_max - old_missing). A resend
    // leaves max unchanged; a fresh AP file starts full.
    const float hp_missing = cc != nullptr ? pal::at<float>(cc, kHpMaxOff) - pal::at<float>(cc, kHpCurOff) : 0.0f;
    const int magic_missing = pal::at<int>(player, kMagicMaxOff) - pal::at<int>(player, kMagicCurOff);
    const int spark_missing = cc != nullptr ? pal::at<int>(cc, kSparkMaxOff) - pal::at<int>(slot, kSparkCurOff) : 0;

    for (int i = 0; i < 5; ++i)
    {
        if (i == kVialUpgradeIndex)
            continue; // vials go through the offset-free mod API
        auto &fieldv = pal::at<std::uint32_t>(slot, kUpgradeFieldOff[i]);
        if (!upgrade_field_in_domain(i, fieldv))
        {
            g_up_layout_ok = false; // short-circuits every later call
            pal::logf(pal::LogLevel::Warn, "upgrades: kUpgradeFieldOff[%d] read=0x%x exceeds cap %d; offset may have shifted, upgrade writes DISABLED", i,
                      fieldv, kUpgradeCaps[i]);
            return false;
        }
        fieldv = upgrade_field_value(i, counts[i], fieldv);
    }
    mod::player_update_stats(); // recompute live maxima from the owned-bit fields; acts on the live player

    if (cc != nullptr)
    {
        const float hp_max = pal::at<float>(cc, kHpMaxOff);
        pal::at<float>(cc, kHpCurOff) = std::clamp(hp_max - hp_missing, 0.0f, hp_max);
        const int spark_max = pal::at<int>(cc, kSparkMaxOff);
        pal::at<int>(slot, kSparkCurOff) = std::clamp(spark_max - spark_missing, 0, spark_max);
    }
    const int magic_max = pal::at<int>(player, kMagicMaxOff);
    pal::at<int>(player, kMagicCurOff) = std::clamp(magic_max - magic_missing, 0, magic_max);

    trace(0, "applied", slot);
    return true;
}

bool install_newfile_kit_suppressor(std::function<bool()> should_suppress)
{
    const std::uintptr_t addr = pal::resolve_game_symbol(sym::save_slot_clear);
    if (addr == 0)
    {
        pal::logf(pal::LogLevel::Warn, "newfile-kit: SaveSlot::Clear not resolved; starting-kit suppression disabled");
        return false;
    }
    g_kit_suppress = std::move(should_suppress);
    g_kit_hook = pal::ScopedHook(addr, &repl_save_slot_clear, &g_orig_save_slot_clear);
    if (!g_kit_hook.installed())
    {
        pal::logf(pal::LogLevel::Error, "newfile-kit: failed to hook SaveSlot::Clear");
        g_kit_suppress = nullptr;
        return false;
    }
    pal::logf(pal::LogLevel::Info, "newfile-kit: hooked SaveSlot::Clear at %p", g_kit_hook.target());
    return true;
}

} // namespace mth
