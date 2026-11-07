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
#include "mth/features/weapon_grants.hpp"
#include "pal/pal_game.hpp"
#include "pal/pal_log.hpp"

namespace mth
{

HookManager::HookManager(RandoBridge &rando, const std::map<int, ScoutInfo> &scout, ApState &state, ApSaveBundleStore &bundle,
                         std::function<void(const std::string &)> send_death, std::function<void *()> get_player)
    : rando_(rando), get_player_(get_player), location_hooks_(rando, &scout), boss_tracker_(rando), goal_tracker_(rando),
      death_hooks_(std::move(send_death), std::move(get_player)), ability_hooks_([&state](std::int64_t id) { return state.has_received(id); }),
      sewer_cat_gate_([this, &state] { return vendor_locked(state); }), status_menu_caps_(level_cap_hooks_),
      save_takeover_(bundle, [&state] { return std::make_pair(state.seed(), std::to_string(state.player_slot())); })
{
    install_pawn_shop_hook([this, &state] { return vendor_locked(state); });
    install_fountain_lamp_hook();
    // The title gate owns the only StartGame detour, which calls into the takeover; the destructor
    // removes it before the takeover member is destroyed.
    install_title_gate([&state] { return state.phase() == ConnectionPhase::Connected; }, [this] { return save_takeover_.begin(); });
    // Loading a save is the vendor lockout's only reset point, which scopes the latch to a save rather
    // than to the process.
    set_save_loaded(
        [this]
        {
            boss_tracker_.on_save_loaded();
            vendor_lockout_.store(false, std::memory_order_relaxed);
        });
    location_hooks_.set_player_getter(get_player_); // kear key edits mirror Player+0x11b0 (#130)
}

HookManager::~HookManager()
{
    // Unreached while the entry point leaks App (see entry.cpp). These detours and the save-loaded
    // callback reach members through this, so they go before any member is destroyed. App removes the
    // tick hooks before this runs.
    remove_title_gate();
    remove_pawn_shop_hook();
    remove_fountain_lamp_hook();
    set_save_loaded(nullptr);
}

// Pawnty and Panino both sell outside AP logic, so they stay shut for the rest of the save once a
// session has been seen; a mid-run disconnect must not hand the exploit back. SewerCatGate::tick
// calls this every drain, which is what arms the latch for Pawnty too: his own detour does not run
// until the player reaches him.
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

    // Neutralize the world-kear collect grant in every AP mode; pin usable keys to 0 only in the AP-item
    // modes (vanilla mode credits received Universal Kears instead - see App/InboundGranter).
    location_hooks_.set_kear_gating(authed, authed && state.kear_keys_suppressed());
    location_hooks_.reconcile_kear_keys(); // re-cancel AP kears that a reload restored as usable keys (suppress modes)
    location_hooks_.enforce_native_bits(); // native collected-bit for server-collected durable-bit chests (Collect/coop)

    // slot_data lamps, 0 when not authed. The detour ORs the sticky console override over them.
    set_fountain_slot_mask(authed ? state.config().lit_generator_lamp_mask : 0);

    // Unconditional: the bridge persists and queues checks while disconnected, so a kill during an
    // outage is still recorded rather than lost.
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

    // The station's donation machine (#162). When the seed carries it as a location the vanilla path is
    // right - it completes, OnPickupDone suppresses the pass and sends the check - so all it needs is a
    // cheaper goal. When the seed does not, nothing would refuse the grant, so the machine is made inert
    // and the OnPickup backstop arms behind it.
    const bool train_gated = authed && state.config().train_rando;
    // Through the bridge, not ApState, so a seed that prunes the donation location is seen here too.
    const bool machine_is_check = rando_.is_ap_location(kTrainPassLocIdx);
    // Train fast-travel (train_rando): gate each destination on its AP ticket rather than the whole-train
    // ability, so visiting a station no longer unlocks it (#98).
    ability_hooks_.set_train_gate(train_gated, state);
    ability_hooks_.set_ticket_machine(machine_is_check, rando_.is_checked(kTrainPassLocIdx), ticket_machine_seed(state.config().train_pass_cost));
    set_train_pass_machine_blocked(train_gated && !machine_is_check);
    const bool armed = authed || policy.console_abilities;
    const bool slot_ok = !authed ? true : (save_game_slot >= 0 && modifier_hooks_.captured_ap_slot() == save_game_slot);
    ability_hooks_.set_enforce(armed && slot_ok);
    // Both calls write durable save fields, so both take authed as well: slot_ok on its own is true while
    // offline and is not the bound-save test.
    clear_starter_weapon_swap(authed, slot_ok);
    intro_chest_gate_.enforce_weapon_grants(state, authed, slot_ok);
    // Mirrors End shortcut switches (#28). Durable like the two above, so it takes slot_ok for the same
    // reason; it reads authed off the state itself.
    tick_mirror_switches(state, slot_ok);

    lock_hooks_.seed_kear_blocks(state);

    death_hooks_.poll(); // edge-detect a local death for deathlink (send_death gates on deathlink enabled)

    save_takeover_.tick(); // self-gates on its own step; no-ops in the terminal ones
}

void HookManager::clear_session_state()
{
    // LockHooks::seed_kear_blocks() only ever adds, so the previous connection's kear-block removals
    // would otherwise keep being written into the next AP game's unlock bitfield.
    lock_hooks_.locks().clear();
    modifier_hooks_.set_ap_slot(-1); // unknown again: the next AP game captures its own slot
    death_hooks_.reset();            // a death latched for retry belongs to the connection being dropped
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
