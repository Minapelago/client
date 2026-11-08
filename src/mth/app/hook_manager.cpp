#include "mth/app/hook_manager.hpp"

#include <cstdint>

#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/ap/ap_state.hpp"
#include "mth/core/ap/rando_bridge.hpp"
#include "mth/core/rules/session_policy.hpp"
#include "mth/features/ability_hooks.hpp"
#include "mth/features/death_hooks.hpp"
#include "mth/features/fountain_lamp_hooks.hpp"
#include "mth/features/item_granter.hpp"
#include "mth/features/levelcap_hooks.hpp"
#include "mth/features/location_hooks.hpp"
#include "mth/features/lock_hooks.hpp"
#include "mth/features/menus/pawn_shop_hooks.hpp"
#include "mth/features/menus/status_menu_caps.hpp"
#include "mth/features/menus/title_gate.hpp"
#include "mth/features/mirror_switches.hpp"
#include "mth/features/modifier_hooks.hpp"
#include "mth/features/save_pollers.hpp"
#include "mth/features/save_takeover.hpp"
#include "mth/features/scene_gates/intro_chest_gate.hpp"
#include "mth/features/scene_gates/sewer_cat_gate.hpp"
#include "mth/features/trap_hooks.hpp"
#include "mth/features/weapon_grants.hpp"
#include "pal/pal_game.hpp"
#include "pal/pal_log.hpp"

namespace mth
{

HookManager::HookManager(RandoBridge &rando, const std::map<int, ScoutInfo> &scout, ApState &state, ApSaveBundleStore &bundle,
                         std::function<void(const std::string &)> send_death, std::function<void *()> get_player)
    : rando_(rando), get_player_(get_player), location_hooks_(rando, &scout), boss_tracker_(rando), goal_tracker_(rando),
      death_hooks_(std::move(send_death), std::move(get_player)), ability_hooks_([&state](std::int64_t id) { return state.has_received(id); }),
      sewer_cat_gate_([this, &state] { return vendor_locked(state); }),
      // Save load is the vendor lockout's only reset point. boss_tracker_ and vendor_lockout_ are declared earlier, so they outlive the hook.
      modifier_hooks_(
          [this]
          {
              boss_tracker_.on_save_loaded();
              vendor_lockout_.store(false, std::memory_order_relaxed);
          }),
      status_menu_caps_(level_cap_hooks_), save_takeover_(bundle, [&state] { return std::make_pair(state.seed(), std::to_string(state.player_slot())); })
{
    install_pawn_shop_hook([this, &state] { return vendor_locked(state); });
    install_fountain_lamp_hook();
    // The title gate owns the only StartGame detour, which calls into the takeover.
    install_title_gate([&state] { return state.phase() == ConnectionPhase::Connected; }, [this] { return save_takeover_.begin(); });
    location_hooks_.set_player_getter(get_player_); // kear key edits mirror Player+0x11b0 (#130)
}

// Pawnty and Panino sell outside AP logic, so they stay shut for the rest of the save once a session is seen. SewerCatGate::tick calls this every drain,
// which also arms the latch for Pawnty before the player reaches him.
bool HookManager::vendor_locked(const ApState &state)
{
    if (state.phase() == ConnectionPhase::Connected)
        vendor_lockout_.store(true, std::memory_order_relaxed);
    return vendor_lockout_.load(std::memory_order_relaxed);
}

void HookManager::tick(ApState &state, const SessionPolicy &policy, int save_game_slot)
{
    const bool authed = state.authenticated();
    // Enforce (seed + lockdown) only in an AP session, offline test mode, or once the console
    // drove modifiers; ap_scoped (authed only) restricts the seed to the captured AP-game slot.
    modifier_hooks_.set_enforce_live(authed || policy.console_modifiers);
    modifier_hooks_.set_ap_scoped(authed);
    modifier_hooks_.set_ossex_start(state.config().ossex_start); // force Landing Done when slot_data requests it
    modifier_hooks_.drain_live();

    level_cap_hooks_.set_enforce_live(authed || policy.forced_caps);
    if (!policy.forced_caps)
        level_cap_hooks_.recompute(state);

    // Neutralize the world-kear collect grant in every AP mode; pin usable keys to 0 only in the AP-item modes (vanilla mode credits received Universal
    // Kears, see InboundGranter).
    location_hooks_.set_kear_gating(authed, authed && state.kear_keys_suppressed());
    location_hooks_.reconcile_kear_keys(); // re-cancel AP kears that a reload restored as usable keys (suppress modes)
    location_hooks_.enforce_native_bits(); // native collected-bit for server-collected durable-bit chests (Collect/coop)

    // slot_data lamps, 0 when not authed. The detour ORs the sticky console override over them.
    set_fountain_slot_mask(authed ? state.config().lit_generator_lamp_mask : 0);

    // Unconditional: the bridge persists and queues checks while disconnected, so a kill during an outage is still recorded.
    boss_tracker_.poll();

    if (authed)
    {
        goal_tracker_.evaluate(state); // poll SaveSlot; fires the AP goal when the slot_data condition is met
        latch_kear_completion(state);  // latches the KeyMiser trade flag once every kear is held (#174)
    }

    if (authed)
    {
        ability_hooks_.set_randomized(Ability::Burrow, state.config().burrow_rando);
        ability_hooks_.set_randomized(Ability::Swim, state.config().swim_rando);
        ability_hooks_.set_randomized(Ability::RopeClimb, state.config().rope_rando);
        ability_hooks_.set_randomized(Ability::BouncePuff, state.config().puff_rando);
        ability_hooks_.set_randomized(Ability::BounceSpring, state.config().spring_rando);
        ability_hooks_.set_randomized(Ability::Carry, state.config().carry_rando);
    }
    // offline: leave randomized as the `ability` console command set it.

    // The station's donation machine (#162). As a location it completes normally and OnPickupDone swaps the pass for the check, so it only needs a cheaper
    // goal. Otherwise the machine is made inert and the OnPickup backstop arms behind it.
    const bool train_gated = authed && state.config().train_rando;
    // Asks the bridge so a seed that prunes the donation location is seen here.
    const bool machine_is_check = rando_.is_ap_location(kTrainPassLocIdx);
    // Train fast travel (train_rando): gate each destination on its AP ticket so visiting a station does not unlock it (#98).
    ability_hooks_.set_train_gate(train_gated, state);
    ability_hooks_.set_ticket_machine(machine_is_check, rando_.is_checked(kTrainPassLocIdx), ticket_machine_seed(state.config().train_pass_cost));
    set_train_pass_machine_blocked(train_gated && !machine_is_check);
    const bool armed = authed || policy.console_abilities;
    const bool slot_ok = !authed ? true : (save_game_slot >= 0 && modifier_hooks_.captured_ap_slot() == save_game_slot);
    ability_hooks_.set_enforce(armed && slot_ok);
    // Both write durable save fields, so both need authed too: slot_ok alone is true offline.
    clear_starter_weapon_swap(authed, slot_ok);
    intro_chest_gate_.enforce_weapon_grants(state, authed, slot_ok);
    // Mirrors End shortcut switches (#28). Durable too, so it takes slot_ok; it reads authed off the state.
    tick_mirror_switches(state, slot_ok);

    lock_hooks_.seed_kear_blocks(state);

    death_hooks_.poll(); // edge-detect a local death for deathlink (send_death gates on deathlink enabled)

    save_takeover_.tick(); // self-gates on its own step; no-ops in the terminal ones
}

void HookManager::clear_session_state()
{
    // LockHooks::seed_kear_blocks() only adds, so the previous connection's kear-block removals would keep reaching the next game's unlock bitfield.
    lock_hooks_.locks().clear();
    modifier_hooks_.set_ap_slot(-1); // unknown again: the next AP game captures its own slot
    traps_.clear();                  // a running trap must not leak into the next seed
    // A death latched before the seed change belongs to the old seed, even if the new ApConnected lands in a later tick.
    death_hooks_.reset();
}

void HookManager::drain()
{
    lock_hooks_.seed_removed_locks();
    lock_hooks_.sweep(); // open already-spawned KeyBlock / KeyBlockChain locks and kear-locked chests
    ability_hooks_.enforce_train_tick();
    ability_hooks_.enforce_burrow_tick(get_player_ ? get_player_() : nullptr);
    sewer_cat_gate_.tick();   // self-gated on the vendor lockout; walks nothing when it is clear
    intro_chest_gate_.tick(); // self-gated on the armed flag; walks nothing when it is clear
}

void HookManager::on_world_update_end(void *world)
{
    save_takeover_.on_world_update_end(world);
    status_menu_caps_.on_world_update_end(world); // self-throttled; walks nothing on most ticks
}

void HookManager::on_world_destroy()
{
    location_hooks_.reset_native_bits(); // a save reload clears s_rItemCollection; re-apply on the next load
    ability_hooks_.on_world_destroy();
    lock_hooks_.on_world_destroy(); // the tracked chests died with the world
    sewer_cat_gate_.on_world_destroy();
    intro_chest_gate_.on_world_destroy();
    status_menu_caps_.on_world_destroy();
    mirror_switches_on_world_destroy(); // the managed switches died with the world
}

void HookManager::append_status_lines(std::vector<std::string> &out) const
{
    for (const auto &l : traps_.status_lines())
        out.push_back(l);
    for (const auto &l : modifier_hooks_.status_lines())
        out.push_back(l);
    for (const auto &l : level_cap_hooks_.status_lines())
        out.push_back(l);
    for (const auto &l : goal_tracker_.status_lines())
        out.push_back(l);
    for (const auto &l : save_takeover_.status_lines())
        out.push_back(l);
}

} // namespace mth
