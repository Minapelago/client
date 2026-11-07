#include "mth/app/app.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "mod/mod_api.hpp"
#include "mth/app/gate_probes.hpp"
#include "mth/app/hook_manager.hpp"
#include "mth/core/ap/ability_ids.hpp"
#include "mth/core/ap/ap_coordinator.hpp"
#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/ap/ap_link.hpp"
#include "mth/core/ap/area_reporter.hpp"
#include "mth/core/ap/broadcast.hpp"
#include "mth/core/ap/rando_bridge.hpp"
#include "mth/core/game/game_symbols.hpp"
#include "mth/features/fountain_lamp_hooks.hpp"
#include "mth/features/mirror_switches.hpp"
#include "mth/features/player_tracker.hpp"
#include "mth/features/room_tracker.hpp"
#include "mth/features/save_takeover.hpp"
#include "mth/features/trap_hooks.hpp"
#include "mth/features/upgrades.hpp"
#include "mth/net/ap_link_apclient.hpp"
#include "mth/ui/overlay_root.hpp"
#include "mth_version.h"
#include "pal/pal_hook.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_module.hpp"
#include "pal/pal_overlay.hpp"

namespace
{

mth::App *g_app = nullptr; // set by the ctor before the tick hooks go live

void (*g_orig_game_fixed_update)() = nullptr;

// Gates every game-thread event on a fully built App, since the tick hooks go live mid-ctor; ready() is the release/acquire barrier.
bool app_ready()
{
    return g_app != nullptr && g_app->ready();
}

void repl_game_fixed_update()
{
    if (g_orig_game_fixed_update)
        g_orig_game_fixed_update();
    if (app_ready())
        g_app->drive_tick();
}

// The native "WorldUpdate" hook delivers the spawn window at the top of World::Update, so there is no original to forward.
void world_update_notify()
{
    if (!app_ready())
        return;
    // Liveness first: this callback firing proves the native mod-hook path works, and the gate verdict waits on it.
    g_app->gate_note_worldupdate();
    g_app->drain_grants();
}

void world_update_end_notify(void *world)
{
    if (app_ready())
        g_app->on_world_update_end(world);
}

// Delivered by the native "WorldDestroy" hook, so there is no original to forward.
void world_destroy_notify()
{
    if (app_ready())
        g_app->on_world_destroy();
}

} // namespace

namespace mth
{

App::App() : bundle_store_(mod_save_dir(), {pal::log_dir(), mod_save_dir()}), login_prefs_(pal::log_dir() / "login.prefs")
{
    pal::logf(pal::LogLevel::Info, "mth-apclient %.*s (%.*s) loaded", static_cast<int>(version::string.size()), version::string.data(),
              static_cast<int>(version::hash.size()), version::hash.data());

    const auto game = pal::game_module();
    const auto self = pal::self_module();

    pal::logf(pal::LogLevel::Info, "game base=0x%llx size=0x%zx path=%s", static_cast<unsigned long long>(game.base), game.size, game.path.c_str());
    pal::logf(pal::LogLevel::Info, "self base=0x%llx path=%s", static_cast<unsigned long long>(self.base), self.path.c_str());

    // Must precede any mod-API call, game_revision() included: until the range is published, usable() accepts any non-null entry.
    const pal::TextRange text = pal::game_text_range();
    pal::set_game_text_range(text);
    pal::logf(text.size != 0 ? pal::LogLevel::Info : pal::LogLevel::Warn, "pal: game .text range base=0x%llx size=0x%zx%s",
              static_cast<unsigned long long>(text.base), text.size, text.size != 0 ? "" : " (unresolved; mod-API pointer validation disabled)");

    const std::uint32_t game_rev = mod::game_revision();
    pal::logf(pal::LogLevel::Info, "game revision=r%u", game_rev);
    if (game_rev == 0)
        pal::logf(pal::LogLevel::Warn, "modding API reports revision 0: native mod hooks (WorldUpdate/IsItemCollected) will NOT fire on this "
                                       "build, so item grants and collection redirects are DISABLED. Ensure the game is the experimental-modding build.");

    pal::init_hook_engine();

    // AP safety gate, before anything touches game memory. Symbol resolution needs the hook engine, and the item-table probes must run before the feature
    // installers call tables::repurpose_dummy_item().
    gate_inputs_ = run_static_gate_probes(game_rev);
    // A refusal blocks connecting; `gate enforce off` overrides it when a probe itself is wrong.
    gate_tick();
    pal::logf(pal::LogLevel::Info, "gate: verdict=%s enforcing=%d (console: `gate enforce off` to override)", verdict_name(gate_verdict_.load()),
              gate_enforcing_.load() ? 1 : 0);

    link_ = std::make_unique<mth::net::ApLink>();
    coordinator_ = std::make_unique<ApCoordinator>(
        *link_, state_,
        // Runs on the game thread (coordinator tick), same as the consumer in tick().
        [this](const std::string &source, const std::string &cause)
        {
            pending_death_text_ = deathlink_banner_text(source, cause);
            pending_inbound_death_.store(true);
        },
        // on_broadcast: hand PrintJSON banners to the render thread.
        [this](const std::vector<mth::BannerSegment> &segs) { banner_queue_.push(segs); },
        // on_scout: fill the scout map on the game thread (coordinator tick runs there).
        [this](const std::vector<mth::ScoutInfo> &locs)
        {
            for (const auto &s : locs)
                scouts_[s.collection_slot] = s;
        },
        // on_session_reset: game thread (coordinator tick), on a fresh ApConnected or ApDisconnected. The scout map is game-thread-only, so it is
        // cleared here; disconnect() runs on the render thread. Also covers connecting to a new server without disconnecting.
        [this]
        {
            // Runs before any SaveSlot write this tick: the game's save path uploads to Steam Cloud, so a leaked write reaches the player's cloud save.
            mod::set_save_write_enabled(false);
            pal::logf(pal::LogLevel::Info, "save: vanilla save writes suppressed (save api=%s, enabled=%d)", mod::save_api_available() ? "ok" : "MISSING",
                      mod::save_write_enabled() ? 1 : 0);
            scouts_.clear();
            // A disconnect skips on_session_end, so a deathlink latched for retry would otherwise land after the player left the multiworld.
            pending_inbound_death_.store(false);
            hooks_->death_hooks_.reset();
        },
        // on_session_end: a different (seed, slot) authenticated. It arrives ahead of that connection's ApConnected, the safe point to drop the old session.
        [this] { clear_session_state(); });
    area_reporter_ = std::make_unique<AreaReporter>(*link_);
    rando_ = std::make_unique<RandoBridge>(*link_, state_);
    granter_.emplace([this](int loc) { return rando_->is_ap_location(loc); }, [this](int loc) { rando_->on_location_collected(loc); });
    traps_ = std::make_unique<TrapHooks>();
    // While authenticated, suppress the game's new-file starting kit (AP supplies it). SaveSlot::Clear also fires on profile menu and save load and can
    // zero an existing save's upgrade fields, so each suppression re-arms the upgrade re-apply.
    install_newfile_kit_suppressor(
        [this]
        {
            if (!state_.authenticated())
                return false;
            upgrades_.force_dirty();
            return true;
        });

    // Tick sources, then the feature hooks they tick into; ready_ holds the trampolines off until the ctor finishes.
    g_app = this;
    fixed_update_hook_ = pal::ScopedHook(sym::game_fixed_update, &repl_game_fixed_update, &g_orig_game_fixed_update, "Game::FixedUpdate");
    mod::install_world_update_hook(&world_update_notify);
    mod::install_world_update_end_hook(&world_update_end_notify);
    mod::install_world_destroy_hook(&world_destroy_notify);
    hooks_ = std::make_unique<HookManager>(
        *rando_, scouts_, state_, bundle_store_, [this](const std::string &detail) { link_->send_death(detail); }, &player_tracker::player);
    {
        overlay_root_ = std::make_unique<OverlayRoot>(*this, banner_queue_);
        pal::overlay_start(overlay_root_.get());
        pal::logf(pal::LogLevel::Info, "overlay: dev console attached"); // overlay logs the resolved toggle key
    }
    ready_.store(true); // everything wired; the tick hooks may already be firing
    pal::logf(pal::LogLevel::Info, "app: tick hooks installed, idling");
}

App::~App()
{
    ready_.store(false);   // stop ticking before any member is torn down
    pal::overlay_stop();   // removes render/input hooks and stops drawing
    overlay_root_.reset(); // then unregister the log observer
    remove_newfile_kit_suppressor();
    // Tick sources stop before the feature hooks they tick into. g_app is cleared before the detour goes,
    // so a FixedUpdate already inside the trampoline only forwards to the original.
    mod::remove_world_destroy_hook();
    mod::remove_world_update_end_hook();
    mod::remove_world_update_hook();
    g_app = nullptr;
    fixed_update_hook_.reset();
    hooks_.reset();
    inbound_.reset(); // references granter_; tear it down first
    granter_.reset();
    rando_.reset();
    area_reporter_.reset();
    coordinator_.reset();
    link_.reset(); // stops the net thread last
    pal::shutdown_hook_engine();
    pal::logf(pal::LogLevel::Info, "mth-apclient unloading");
}

void App::drive_tick()
{
    if (!gate_latch_.settled())
    {
        ++gate_inputs_.ticks_since_probe_installed;
        gate_tick();
    }
    if (!first_tick_logged_)
    {
        first_tick_logged_ = true;
        pal::logf(pal::LogLevel::Info, "tick: Game::FixedUpdate live; AP coordinator pumping");
    }
    player_tracker::sample_position(); // post-FixedUpdate, so grants drained from WorldUpdate read the prior frame
    std::optional<std::uint32_t> screen;
    std::uint32_t screen_id = 0;
    if (room_tracker::current_screen(&screen_id))
        screen = screen_id;
    coordinator_->tick();
    area_reporter_->tick(state_.authenticated(), screen);
    hooks_->tick(state_, policy_, save_state_ ? save_state_->game_slot() : -1);
    upgrades_.recompute(state_);
    // Diagnostic (#46): apply_upgrades runs only while dirty, so its own trace can stay silent; log the gating on change to tell "never tried" from "failed".
    {
        static int s_last_gate = -1;
        const int gate = !upgrades_.dirty() ? 0 : (player_tracker::player() == nullptr ? 1 : 2);
        if (gate != s_last_gate)
        {
            s_last_gate = gate;
            const int *c = upgrades_.counts();
            const char *w = gate == 0 ? "idle (not dirty)" : gate == 1 ? "dirty, player null" : "dirty, attempting apply";
            pal::logf(pal::LogLevel::Debug, "upgrades: drive_tick gate -> %s counts=[%d,%d,%d,%d,%d]", w, c[0], c[1], c[2], c[3], c[4]);
        }
    }
    if (upgrades_.dirty() && apply_upgrades(upgrades_.counts(), player_tracker::player()))
        upgrades_.mark_applied(); // applied to the save; retry next tick if player not ready yet
    // Vials sit outside the dirty gate: the game re-seeds their bitfield mid-run, so the AP count is re-asserted every tick (#171).
    enforce_vial_capacity();
    enforce_wallet_cap();
    if (pending_inbound_death_.exchange(false))
    {
        // kill() logs whether the death landed. The banner is best effort and can name a sender for a death kill() declined.
        hooks_->death_hooks_.kill();
        // Salmon is the AP palette's trap color; to the receiver a deathlink is a trap.
        banner_queue_.push({{pending_death_text_, banner_color("", "salmon", 0, 0, false)}});
    }
    ensure_inbound_ready();
    reconcile_server_checked();
    if (resend_gate_.fire(link_->is_connected(), inbound_.has_value()))
    {
        rando_->flush(); // (re)connect: resend the full persisted checked set; server dedups
        pal::logf(pal::LogLevel::Info, "outbound: (re)connect -> flushed checked-set");
    }
    traps_->tick();
    if (inbound_)
        inbound_->tick();
    // Persist a freshly captured AP-game slot so it's known on the next load/session.
    if (save_state_)
    {
        const int s = hooks_->modifier_hooks_.captured_ap_slot();
        if (s >= 0 && s != save_state_->game_slot())
        {
            save_state_->set_game_slot(s);
            save_state_->stage();
            pal::logf(pal::LogLevel::Info, "modifiers: staged AP-game slot %d", s);
        }
    }
}

void App::drain_grants()
{
    hooks_->drain();
    granter_->drain();
}

void App::enforce_vial_capacity()
{
    // Vial capacity is the popcount of a SaveSlot bitfield whose offset drifts between builds (#97), so it goes through the mod API. It is re-asserted
    // each tick (#171) because the healing-vial grant ORs the vanilla 3-bit base back in (Items::OnPickupDone -> Player::AddVialUpgrade), and an OR never
    // reduces it (#83). Idempotent once the count matches, so held flasks stay put.
    if (!state_.authenticated() || !mod::vial_api_available())
        return;
    const int want = upgrades_.counts()[kVialUpgradeIndex]; // clamped >= 0; the setter hangs on a negative
    const int old_max = mod::player_max_vials();
    const int old_held = mod::player_vials();
    mod::set_player_max_vials(want);
    // Re-read: the getter adds the trinket vial bonus the setter does not write, so comparing with `want` would fight an equipped trinket every tick.
    const int new_max = mod::player_max_vials();
    if (new_max == old_max)
        return;
    const int new_held = maintained_vial_held(old_max, old_held, new_max);
    mod::set_player_vials(new_held);
    // Unthrottled: a correction only happens when something re-seeded the bitfield, so the repeats are the signal (#171).
    pal::logf(pal::LogLevel::Info, "vials: capacity %d -> %d (AP count %d, held %d -> %d)", old_max, new_max, want, old_held, new_held);
}

void App::enforce_wallet_cap()
{
    // slot_data "wallet_cap": cap bones at 750 + 500 per received wallet item, uncapped at 8+. The game's cap field drifts between builds, so this clamps
    // current bones through the mod API each frame. Inert unless authenticated with the flag set.
    if (!state_.authenticated() || !state_.config().wallet_cap)
        return;
    const std::optional<int> cap = wallet_cap(state_);
    if (!cap || !mod::bones_api_available())
        return;
    if (mod::player_bones() > *cap)
        mod::set_player_bones(*cap);
}

void App::gate_note_worldupdate()
{
    if (gate_inputs_.worldupdate_observed)
        return;
    gate_inputs_.worldupdate_observed = true;
    pal::logf(pal::LogLevel::Info, "gate: WorldUpdate observed after %d tick(s); native mod hooks are live", gate_inputs_.ticks_since_probe_installed);
    gate_tick();
}

void App::gate_tick()
{
    const GateVerdict before = gate_latch_.verdict();
    const GateVerdict after = gate_latch_.update(gate_inputs_);
    if (after == before)
        return;

    gate_verdict_.store(after);
    std::string reason = refusal_reason(gate_inputs_);
    if (after == GateVerdict::Refused)
    {
        pal::logf(pal::LogLevel::Error, "gate: REFUSED - %s", reason.c_str());
        if (!gate_enforcing_.load())
            pal::logf(pal::LogLevel::Warn, "gate: enforcement is off, so AP behavior is NOT being blocked; run `gate enforce on` to enforce");
        else if (link_)
        {
            // Liveness can settle after connect() let a session through, so a refusal drops it. link_ is still null under the ctor's first gate_tick().
            pal::logf(pal::LogLevel::Error, "gate: dropping the AP session that was opened before the verdict settled");
            link_->disconnect();
        }
    }
    else if (after == GateVerdict::Clear)
    {
        pal::logf(pal::LogLevel::Info, "gate: CLEAR after %d tick(s); AP behavior permitted", gate_inputs_.ticks_since_probe_installed);
        if (!gate_inputs_.revision_known)
            pal::logf(pal::LogLevel::Warn, "gate: this game build is outside the validated revision range; proceeding because every probe passed");
        if (!gate_inputs_.mod_api_shape_ok)
            pal::logf(pal::LogLevel::Error, "gate: the game's mod API version differs from the one this build expects; entries may be misaligned");
    }
    {
        std::lock_guard<std::mutex> lk(gate_reason_mutex_);
        gate_reason_ = std::move(reason);
    }
}

void App::on_world_update_end(void *world)
{
    room_tracker::poll();
    hooks_->on_world_update_end(world);
}

void App::on_world_destroy()
{
    // The Player is freed with the world; drop the cached pointer so the next upgrade re-apply cannot write through it.
    player_tracker::invalidate_player();
    // A save reload wipes the mod's collected-bit writes from s_rItemCollection; re-arm so the next in-game tick re-applies them.
    hooks_->on_world_destroy();
}

// A different (seed, slot) authenticated, and the mod owns one save per (seed, slot), so start clean. ApSessionEnded lands ahead of the new ApConnected,
// so that connection's items and checks apply on top. A reconnect to the same seed and slot never reaches here (#152).
void App::clear_session_state()
{
    rando_->reset_session(); // releases the save; must precede save_state_.reset()
    // inbound_ also references the ApSaveState, so it goes before save_state_. Its queued grants belong to that save and must not reach the next one (#175).
    granter_->discard_pending();
    inbound_.reset();
    save_state_.reset();
    state_.reset_session();
    scouts_.clear();
    hooks_->clear_session_state();
    // Reset with the received stream so UpgradeState::applied_ returns to zero too and the next recompute cannot write a downgrade into the live save.
    upgrades_ = UpgradeState{};
    traps_->clear();                    // a running trap must not leak into the next seed
    resend_gate_ = ConnectResendGate{}; // re-arm so the next connection flushes its own checked-set
    pending_inbound_death_.store(false);
    pal::logf(pal::LogLevel::Info, "session: cleared previous AP connection state");
}

void App::ensure_inbound_ready()
{
    if (inbound_ || !state_.authenticated())
        return;
    remember_successful_login(); // first tick after authentication: this target worked
    const std::string seed = state_.seed();
    const std::string slot = std::to_string(state_.player_slot());
    const std::string key = bundle_store_.path_for(seed, slot).string();
    save_state_.emplace([this, seed, slot] { return bundle_store_.load_state(seed, slot); },
                        [this, seed, slot](std::string_view text) { bundle_store_.stage_state(seed, slot, text); });
    inbound_.emplace(
        *granter_, state_, *save_state_, [this] { return hooks_->location_hooks_.credit_kear_key(player_tracker::player()); }, // vanilla-kear key grant (#130)
        [this](int modifier_index) { return traps_->arm(modifier_index); });
    pal::logf(pal::LogLevel::Info, "inbound: state loaded (%s); granter live", key.c_str());
    hooks_->modifier_hooks_.set_ap_slot(save_state_->game_slot()); // restore the AP-game slot (skip capture if known)
    rando_->attach_save_state(*save_state_);
    pal::logf(pal::LogLevel::Info, "outbound: bridge attached to %s", key.c_str());
}

void App::reconcile_server_checked()
{
    if (!save_state_) // inbound not ready yet; ids remain pending in ApState until the save attaches
        return;
    const auto ids = state_.take_server_checked_pending();
    bool changed = false;
    for (std::int64_t id : ids)
        if (id >= 0 && id <= std::numeric_limits<int>::max())
            changed |= rando_->reconcile_server_checked(static_cast<int>(id));
    if (changed)
        save_state_->stage(); // one stage per reconcile pass (batched)
}

void App::connect(const std::string &server, const std::string &slot, const std::string &password)
{
    if (should_refuse_connect(gate_enforcing_.load(), gate_verdict_.load()))
    {
        std::string reason;
        {
            std::lock_guard<std::mutex> lk(gate_reason_mutex_);
            reason = gate_reason_;
        }
        // ApState is game-thread data and this is the render thread, so the UI reads the reason via gate_status().
        pal::logf(pal::LogLevel::Error, "gate: refusing connect - %s", reason.c_str());
        return;
    }
    {
        std::lock_guard<std::mutex> lk(login_mutex_);
        pending_server_ = server;
        pending_slot_ = slot;
    }
    link_->connect(server, slot, password);
}

SavedLogin App::saved_login() const
{
    std::lock_guard<std::mutex> lk(login_mutex_);
    return SavedLogin{login_prefs_.server(), login_prefs_.slot()};
}

// Commit the attempted target only once the server has authenticated, so a typo or a dead
// host never overwrites a known-good one.
void App::remember_successful_login()
{
    std::lock_guard<std::mutex> lk(login_mutex_);
    if (pending_server_.empty())
        return;
    if (pending_server_ == login_prefs_.server() && pending_slot_ == login_prefs_.slot())
        return;
    login_prefs_.set(pending_server_, pending_slot_);
    login_prefs_.save();
    pal::logf(pal::LogLevel::Info, "login: remembered %s / %s", pending_server_.c_str(), pending_slot_.c_str());
}

void App::disconnect()
{
    // Render thread. The game-thread-only scout map is cleared in on_session_reset (see ctor).
    link_->disconnect();
}

ConnectionStatus App::connection_status() const
{
    return ConnectionStatus{state_.phase(), state_.detail()};
}

std::vector<std::string> App::status_lines() const
{
    std::vector<std::string> out;
    out.push_back(std::string("connected: ") + (link_->is_connected() ? "yes" : "no"));
    out.push_back("ap status: " + state_.status());
    out.push_back("player slot: " + std::to_string(state_.player_slot()));
    out.push_back("received items: " + std::to_string(state_.received_items().size()));
    if (const auto removed = state_.removed_locations().size(); removed != 0)
        out.push_back("removed locations: " + std::to_string(removed));
    if (save_state_)
    {
        std::string line = "ap state: " + std::to_string(save_state_->checked().size()) + " checked";
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

    for (const auto &l : traps_->status_lines())
        out.push_back(l);
    hooks_->append_status_lines(out);
    return out;
}

GateStatus App::gate_status() const
{
    GateStatus s;
    s.verdict = gate_verdict_.load();
    s.enforcing = gate_enforcing_.load();
    {
        std::lock_guard<std::mutex> lk(gate_reason_mutex_);
        s.reason = gate_reason_;
    }
    return s;
}

void App::set_gate_enforcing(bool on)
{
    gate_enforcing_.store(on);
    pal::logf(pal::LogLevel::Info, "gate: enforcing=%d (verdict=%s)", on ? 1 : 0, verdict_name(gate_verdict_.load()));
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
    // Render thread: the game thread walks TrapHooks' countdown lists every tick, so queue the request. The arm logs its outcome.
    traps_->queue_arm(modifier_index, seconds);
    pal::logf(pal::LogLevel::Info, "console: trap %d queued for the next game tick", modifier_index);
}

void App::probe_switches()
{
    // Render thread: the walk calls into the game per node, so it runs on the next tick.
    request_switch_probe();
    pal::logf(pal::LogLevel::Info, "console: switch probe queued for the next game tick");
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
