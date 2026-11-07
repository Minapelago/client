#include "mth/features/save_pollers.hpp"

#include <bit>
#include <cstdio>

#include "mod/mod_api.hpp"
#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/ap/ap_state.hpp"
#include "mth/core/ap/rando_bridge.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/core/rules/goal.hpp"
#include "mth/core/rules/kear_completion.hpp"
#include "pal/pal_game.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"

namespace mth
{

GoalTracker::GoalTracker(RandoBridge &bridge) : bridge_(bridge)
{
}

void GoalTracker::evaluate(const ApState &state)
{
    if (disabled_)
        return;
    void *slot = pal::active_save_slot();
    if (slot == nullptr)
        return;

    const unsigned char clear_flag = pal::at<unsigned char>(slot, layout::kSaveGameClearOff);
    if (!game_clear_flag_in_domain(clear_flag))
    {
        disabled_ = true;
        pal::logf(pal::LogLevel::Error, "goal: kSaveGameClearOff read=0x%x is not a boolean; offset may have shifted, goal tracking DISABLED", clear_flag);
        return;
    }

    config_ = state.config().goal_config;
    gens_needed_ = state.config().goal_generators;
    bosses_needed_ = state.config().goal_bosses;
    game_cleared_ = clear_flag != 0;
    broken_mask_ = state.config().broken_generator_mask;
    gens_done_ = generators_done(pal::at<std::uint64_t>(slot, layout::kSaveGeneratorBitsOff), broken_mask_);
    bosses_done_ = std::popcount(mod::player_bosses_defeated());

    if (goal_met(config_, gens_needed_, bosses_needed_, game_cleared_, gens_done_, bosses_done_))
        bridge_.send_goal(); // one-shot + authenticated-gated in the bridge
}

std::vector<std::string> GoalTracker::status_lines() const
{
    char mask[24] = {};
    std::snprintf(mask, sizeof(mask), "0x%llx", static_cast<unsigned long long>(broken_mask_));
    return {"goal: config=" + std::to_string(config_) + " cleared=" + std::to_string(game_cleared_) + " generators=" + std::to_string(gens_done_) + "/" +
            std::to_string(gens_needed_) + " (broken=" + mask + ") bosses=" + std::to_string(bosses_done_) + "/" + std::to_string(bosses_needed_)};
}

BossTracker::BossTracker(RandoBridge &bridge) : bridge_(bridge)
{
}

void BossTracker::prime(const void *slot, const char *why, std::uint64_t mask)
{
    prev_slot_ = slot;
    prev_mask_ = mask;
    primed_ = true;
    pal::logf(pal::LogLevel::Debug, "boss: baseline primed (%s) slot=%p mask=0x%llx", why, slot, static_cast<unsigned long long>(mask));
}

void BossTracker::on_save_loaded()
{
    // Defer to the next poll rather than reading here: this runs inside the game's activation call.
    primed_ = false;
    pal::logf(pal::LogLevel::Debug, "boss: save activated; baseline re-taken on the next tick");
}

void BossTracker::poll()
{
    // PlayerGetBossesDefeated reads *(g_saveManager+0x18)+0x280 unguarded and faults with no slot bound. No gameplay happens without a slot, so dropping the
    // baseline loses no kill.
    const void *slot = pal::active_save_slot();
    if (slot == nullptr)
    {
        primed_ = false;
        return;
    }

    const std::uint64_t mask = mod::player_bosses_defeated();
    if (!primed_)
    {
        prime(slot, "slot bound", mask);
        return;
    }
    // The title screen binds its own slot before the run's, so a baseline is never diffed across slots; a fresh save's pre-set bits would read as kills.
    if (slot != prev_slot_)
    {
        prime(slot, "slot changed", mask);
        return;
    }
    if (mask == prev_mask_)
        return;
    // Normal play only ORs bits in, so lost bits mean the contents were replaced under the same pointer (takeover write, NG+ clear): re-baseline.
    if ((prev_mask_ & ~mask) != 0)
    {
        prime(slot, "mask lost bits; save changed", mask);
        return;
    }

    const std::uint64_t risen = mask & ~prev_mask_;
    // No tick kills three bosses, so a rise that wide is save data arriving under a primed slot.
    if (std::popcount(risen) >= 3)
    {
        prime(slot, "wide rise; save data loaded", mask);
        return;
    }
    prev_mask_ = mask;
    for (int idx = 0; idx <= kMaxBossIndex; ++idx)
    {
        if ((risen & (1ULL << idx)) == 0)
            continue;
        const int loc_slot = boss_location_slot(idx);
        pal::logf(pal::LogLevel::Info, "outbound: boss defeated index=%d -> loc slot=%d (defeat bit)", idx, loc_slot);
        if (bridge_.is_ap_location(loc_slot))
            bridge_.on_location_collected(loc_slot);
        else
            pal::logf(pal::LogLevel::Debug, "boss: slot=%d not a valid AP location (apworld may not define this boss)", loc_slot);
    }
    // Bits above kMaxBossIndex are not boss indices; log once so a layout shift is visible.
    const std::uint64_t stray = risen & ~((kMaxBossIndex >= 63) ? ~0ULL : ((1ULL << (kMaxBossIndex + 1)) - 1));
    if (stray != 0)
        pal::logf(pal::LogLevel::Warn, "boss: defeat bits set outside the boss index range: 0x%llx", static_cast<unsigned long long>(stray));
}

void latch_kear_completion(const ApState &state)
{
    if (!state.authenticated())
        return;
    // No latch for a seed without the completion location.
    if (!state.is_valid_location(ap_loc_id(kKearCompletionLocIdx)))
        return;
    void *slot = pal::active_save_slot();
    if (slot == nullptr)
        return; // title/menus: nothing durable to write through

    auto &flags = pal::at<std::uint64_t>(slot, layout::kSaveKeyMiserFlagOff);
    const std::uint64_t bit = std::uint64_t{1} << layout::kSaveKeyMiserTradeBit;
    if ((flags & bit) != 0)
        return; // already traded, natively or by an earlier tick

    if (!have_all_kears(state.config().kear_mode, state.received_items()))
        return;

    flags |= bit;
    pal::logf(pal::LogLevel::Info, "kear: all kears held; latched the KeyMiser trade flag (the reward spawns on the next entry to the Kear Institute)");
}

} // namespace mth
