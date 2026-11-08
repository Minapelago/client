// Console-only App commands. DevConsole posts them to commands_, so they run on the game thread.

#include <cstdint>
#include <string>
#include <vector>

#include "mod/mod_api.hpp"
#include "mth/app/app.hpp"
#include "mth/app/hook_manager.hpp"
#include "mth/core/ap/ability_ids.hpp"
#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/ap/ap_link.hpp"
#include "mth/features/fountain_lamp_hooks.hpp"
#include "mth/features/mirror_switches.hpp"
#include "pal/pal_log.hpp"

namespace mth
{

std::vector<std::string> App::status_lines() const
{
    std::vector<std::string> out;
    out.push_back(std::string("connected: ") + (link_->is_connected() ? "yes" : "no"));
    out.push_back("ap status: " + state_.status());
    out.push_back("player slot: " + std::to_string(state_.player_slot()));
    out.push_back("received items: " + std::to_string(state_.received_items().size()));
    if (const auto removed = state_.removed_locations().size(); removed != 0)
        out.push_back("removed locations: " + std::to_string(removed));
    if (session_)
    {
        std::string line = "ap state: " + std::to_string(session_->save.checked().size()) + " checked";
        line += bundle_store_.state_staged() ? " (uncommitted since the last game save)" : " (committed)";
        out.push_back(line);
    }

    // Firing is the only evidence a named hook exists on this build; anything still listed after real play is a dead path.
    if (const auto unfired = mod::unfired_hooks(); !unfired.empty())
    {
        std::string line = "mod hooks NEVER FIRED:";
        for (const char *name : unfired)
            line += std::string(" ") + name;
        out.push_back(line);
    }

    hooks_->append_status_lines(out);
    return out;
}

std::vector<std::string> App::item_lines() const
{
    std::vector<std::string> out;
    for (const auto &it : state_.received_items())
        out.push_back("item_id=" + std::to_string(it.item_id) + " index=" + std::to_string(it.index) + " from=" + std::to_string(it.player_from));
    if (out.empty())
        out.push_back("(no items received yet)");
    return out;
}

void App::give_item(std::int64_t ap_item_id)
{
    // Non-vanilla ids and capacity upgrades go through a received-item handler; inject into the socket's list so the dev path matches a real receipt.
    if (!is_vanilla_game_item(ap_item_id) || is_capacity_upgrade_item(ap_item_id))
    {
        state_.inject_received_item(ap_item_id);
        pal::logf(pal::LogLevel::Info, "console: giveapitem %lld -> injected as received item (applied by its segment handler)",
                  static_cast<long long>(ap_item_id));
        return;
    }

    const int item_type = mth::game_item_type(ap_item_id);
    // Dev-console path: nothing durable to record, so it carries no receipt.
    if (granter_->grant(item_type, kNoReceipt))
        pal::logf(pal::LogLevel::Info, "console: giveapitem %lld (type=%d) queued", static_cast<long long>(ap_item_id), item_type);
    else
        pal::logf(pal::LogLevel::Warn, "console: giveapitem %lld not ready (collect any pickup first to capture player + position)",
                  static_cast<long long>(ap_item_id));
}

void App::remove_lock(int slot)
{
    hooks_->lock_hooks_.locks().set_removed(slot);
    pal::logf(pal::LogLevel::Info, "console: removelock %d (live if spawned; opens on entry otherwise)", slot);
}

void App::set_modifier(int idx, bool on)
{
    policy_.console_modifiers = true;
    hooks_->modifier_hooks_.set_live(idx, on);
    pal::logf(pal::LogLevel::Info, "console: modifier %d %s", idx, on ? "on" : "off");
}

void App::fire_trap(int modifier_index, float seconds)
{
    // arm() warns about NotReady only once per index, and the console never retries, so report every attempt here.
    const TrapArm r = hooks_->traps_.arm(modifier_index, seconds);
    const char *what = r == TrapArm::Armed      ? "armed"
                       : r == TrapArm::Skipped  ? "skipped, the modifier is already on"
                       : r == TrapArm::NotReady ? "not ready and not retried; run it again once in a room"
                                                : "unavailable on this build";
    pal::logf(pal::LogLevel::Info, "console: trap %d %s", modifier_index, what);
}

void App::probe_switches()
{
    // The walk rides tick_mirror_switches, which runs later in this tick and logs what it finds.
    request_switch_probe();
}

void App::set_mirror_switch_override(bool on)
{
    set_mirror_switches_override_flag(on);
    if (on)
        pal::logf(pal::LogLevel::Warn, "console: mirror-switch override on; it ignores the AP session and the bound save, and the clamp is a durable write");
    else
        pal::logf(pal::LogLevel::Info, "console: mirror-switch override off");
}

void App::lock_modifiers(bool armed)
{
    policy_.console_modifiers = true;
    hooks_->modifier_hooks_.set_armed(armed);
    pal::logf(pal::LogLevel::Info, "console: modifiers %s", armed ? "locked" : "unlocked");
}

void App::set_stat_caps(int attack, int defense, int sidearm)
{
    policy_.forced_caps = true;
    hooks_->level_cap_hooks_.set_counts(attack, defense, sidearm);
    pal::logf(pal::LogLevel::Info, "console: stat caps attack=%d defense=%d sidearm=%d", attack, defense, sidearm);
}

void App::set_ability_randomized(Ability a, bool on)
{
    policy_.console_abilities = true;
    hooks_->ability_hooks_.set_randomized(a, on);
    pal::logf(pal::LogLevel::Info, "console: ability %d randomized %s", static_cast<int>(a), on ? "on" : "off");
}

void App::enable_deathlink(bool on)
{
    link_->enable_deathlink(on);
    pal::logf(pal::LogLevel::Info, "console: deathlink %s", on ? "enabled" : "disabled");
}

void App::set_lit_lamps(std::uint32_t lamp_mask)
{
    set_fountain_console_mask(lamp_mask);
    pal::logf(pal::LogLevel::Info, "console: fountain lamp override mask=0x%x", lamp_mask);
}

void App::save_test(const std::string &op)
{
    if (!mod::save_api_available())
    {
        pal::logf(pal::LogLevel::Warn, "savetest: modding save API unavailable (revision=%u)", mod::game_revision());
        return;
    }

    if (op == "dump")
    {
        const std::string blob = mod::active_save_slot_contents();
        pal::logf(pal::LogLevel::Info, "savetest: slot=%d bytes=%zu head=%.32s", mod::active_save_slot(), blob.size(), blob.c_str());
    }
    else if (op == "write")
    {
        // Capture then restore the same blob; a surviving session shows the serialize/deserialize pair round-trips.
        const std::string blob = mod::active_save_slot_contents();
        if (blob.empty())
        {
            // "" means the capture failed, and writing it would empty the live save slot.
            pal::logf(pal::LogLevel::Warn, "savetest: capture returned empty blob; skipping write to avoid clobbering the active save slot");
            return;
        }
        const bool ok = mod::set_active_save_slot_contents(blob.c_str());
        mod::player_restore_from_save();
        pal::logf(pal::LogLevel::Info, "savetest: round-trip set=%s bytes=%zu", ok ? "ok" : "FAILED", blob.size());
    }
    else if (op == "noflush")
    {
        // With writes suppressed the game must stop touching saveData.yc; confirm via its mtime.
        mod::set_save_write_enabled(false);
        pal::logf(pal::LogLevel::Info, "savetest: save writes disabled (enabled=%d)", mod::save_write_enabled() ? 1 : 0);
    }
    else if (op == "flush")
    {
        // Re-opens the one path from mod state to vanilla saveData.yc (and Steam Cloud); refused during a takeover.
        if (hooks_->save_takeover_.takeover_active())
        {
            pal::logf(pal::LogLevel::Warn, "savetest: refusing to enable save writes; a save takeover is active");
            return;
        }
        mod::set_save_write_enabled(true);
        pal::logf(pal::LogLevel::Info, "savetest: save writes enabled (enabled=%d)", mod::save_write_enabled() ? 1 : 0);
    }
    else
    {
        pal::logf(pal::LogLevel::Warn, "savetest: unknown op '%s'", op.c_str());
    }
}

} // namespace mth
