#include "mth/app/app.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "mod/mod_api.hpp"
#include "mth/app/gate_probes.hpp"
#include "mth/app/hook_manager.hpp"
#include "mth/core/ap/ap_coordinator.hpp"
#include "mth/core/ap/ap_link.hpp"
#include "mth/core/ap/area_reporter.hpp"
#include "mth/core/ap/broadcast.hpp"
#include "mth/core/ap/rando_bridge.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/core/game/game_symbols.hpp"
#include "mth/features/player_tracker.hpp"
#include "mth/features/room_tracker.hpp"
#include "mth/features/save_takeover.hpp"
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

mth::App *g_app = nullptr;

void (*g_orig_game_fixed_update)() = nullptr;

void repl_game_fixed_update()
{
    if (g_orig_game_fixed_update)
        g_orig_game_fixed_update();
    if (mth::App *app = g_app)
        app->drive_tick();
}

// The native World hooks also fire for the popup, dialogue, prompt and overlay Worlds. The consumers past the liveness note count polls or walk menus
// once per call, so they run only for the gamestate World. "WorldUpdate" delivers the spawn window at the top of World::Update, so there is no original
// to forward.
void world_update_notify(void *world, float step_seconds)
{
    mth::App *app = g_app;
    if (app == nullptr)
        return;
    // Liveness first: any World's callback proves the native mod-hook path works, a gamestate World proves the flag still reads, and the gate verdict
    // waits on both.
    const bool gamestate = mth::layout::is_gamestate_world(world);
    app->gate_note_worldupdate(gamestate);
    if (gamestate)
        app->on_world_update(world, step_seconds);
}

void world_update_end_notify(void *world)
{
    if (!mth::layout::is_gamestate_world(world))
        return;
    if (mth::App *app = g_app)
        app->on_world_update_end(world);
}

// An overlay ending or a system prompt closing destroys its own World while the gamestate World and its entities live on.
void world_destroy_notify(void *world)
{
    if (!mth::layout::is_gamestate_world(world))
        return;
    if (mth::App *app = g_app)
        app->on_world_destroy();
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
    // installers call tables::repurpose_dummy_item(). A refusal blocks connecting; `gate enforce off` overrides it when a probe itself is wrong.
    gate_.begin(run_static_gate_probes(game_rev));

    link_ = std::make_unique<mth::net::ApLink>();
    coordinator_ = std::make_unique<ApCoordinator>(
        *link_, state_,
        // Runs on the game thread (coordinator tick), same as the consumer in drive_tick().
        [this](const std::string &source, const std::string &cause)
        {
            connection_.pending_death_text = deathlink_banner_text(source, cause);
            connection_.pending_inbound_death = true;
        },
        // on_broadcast: hand PrintJSON banners to the render thread.
        [this](const std::vector<mth::BannerSegment> &segs) { banner_queue_.push(segs); },
        // on_scout: fill the scout map on the game thread (coordinator tick runs there).
        [this](const std::vector<mth::ScoutInfo> &locs)
        {
            for (const auto &s : locs)
                connection_.scouts[s.collection_slot] = s;
        },
        // on_session_reset: game thread (coordinator tick), on a fresh ApConnected or ApDisconnected. connection_ is game-thread-only, so it is reset
        // here; disconnect() runs on the render thread. Also covers connecting to a new server without disconnecting.
        [this]
        {
            // Runs before any SaveSlot write this tick: the game's save path uploads to Steam Cloud, so a leaked write reaches the player's cloud save.
            mod::set_save_write_enabled(false);
            pal::logf(pal::LogLevel::Info, "save: vanilla save writes suppressed (save api=%s, enabled=%d)", mod::save_api_available() ? "ok" : "MISSING",
                      mod::save_write_enabled() ? 1 : 0);
            // A disconnect skips on_session_end, so a deathlink latched for retry would otherwise land after the player left the multiworld.
            connection_ = Connection{};
            hooks_->death_hooks_.reset();
        },
        // on_session_end: a different (seed, slot) authenticated. It arrives ahead of that connection's ApConnected, the safe point to drop the old session.
        [this] { clear_session_state(); },
        // on_event: ghost players follow connects, disconnects and datastore replies.
        [this](const ApEvent &ev) { hooks_->ghost_players_.on_ap_event(ev); });
    area_reporter_ = std::make_unique<AreaReporter>(*link_);
    rando_ = std::make_unique<RandoBridge>(*link_, state_);
    granter_.emplace([this](int loc) { return rando_->is_ap_location(loc); }, [this](int loc) { rando_->on_location_collected(loc); });
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

    hooks_ = std::make_unique<HookManager>(
        *rando_, connection_.scouts, state_, bundle_store_, *link_, [this](const std::string &detail) { link_->send_death(detail); }, &player_tracker::player);
    {
        overlay_root_ = std::make_unique<OverlayRoot>(*this, banner_queue_);
        pal::overlay_start(overlay_root_.get());
        pal::logf(pal::LogLevel::Info, "overlay: dev console attached"); // overlay logs the resolved toggle key
    }

    // Tick sources last. No barrier: the ctor and the trampolines share the game thread.
    g_app = this;
    fixed_update_hook_ = pal::ScopedHook(sym::game_fixed_update, &repl_game_fixed_update, &g_orig_game_fixed_update, "Game::FixedUpdate");
    mod::install_world_update_hook(&world_update_notify);
    mod::install_world_update_end_hook(&world_update_end_notify);
    mod::install_world_destroy_hook(&world_destroy_notify);
    pal::logf(pal::LogLevel::Info, "app: tick hooks installed, idling");
}

void App::drive_tick()
{
    if (gate_.tick())
        drop_refused_session();
    commands_.drain();
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
    hooks_->tick(state_, policy_, session_ ? session_->save.game_slot() : -1);
    tick_upgrades(upgrades_, state_);
    // Vials sit outside the dirty gate: the game re-seeds their bitfield mid-run, so the AP count is re-asserted every tick (#171).
    enforce_vial_capacity(upgrades_, state_);
    enforce_wallet_cap(state_);
    if (std::exchange(connection_.pending_inbound_death, false))
    {
        // kill() logs whether the death landed. The banner is best effort and can name a sender for a death kill() declined.
        hooks_->death_hooks_.kill();
        // Salmon is the AP palette's trap color; to the receiver a deathlink is a trap.
        banner_queue_.push({{connection_.pending_death_text, banner_color("", "salmon", 0, 0, false)}});
    }
    ensure_inbound_ready();
    reconcile_server_checked();
    if (session_ && session_->resend.fire(link_->is_connected()))
    {
        rando_->flush(); // (re)connect: resend the full persisted checked set; server dedups
        pal::logf(pal::LogLevel::Info, "outbound: (re)connect -> flushed checked-set");
    }
    hooks_->traps_.tick();
    if (!session_)
        return;
    session_->inbound.tick();
    // Persist a freshly captured AP-game slot so it's known on the next load/session.
    const int s = hooks_->modifier_hooks_.captured_ap_slot();
    if (s >= 0 && s != session_->save.game_slot())
    {
        session_->save.set_game_slot(s);
        session_->save.stage();
        pal::logf(pal::LogLevel::Info, "modifiers: staged AP-game slot %d", s);
    }
}

void App::on_world_update(void *world, float step_seconds)
{
    // The WorldUpdateEnd poll is a step late for ghost players, which need this step's room.
    room_tracker::poll();
    hooks_->drain(world, step_seconds);
    granter_->drain();
}

void App::gate_note_worldupdate(bool gamestate_world)
{
    if (gate_.note_worldupdate(gamestate_world))
        drop_refused_session();
}

void App::drop_refused_session()
{
    // Liveness can settle after connect() let a session through, so a refusal drops it.
    pal::logf(pal::LogLevel::Error, "gate: dropping the AP session that was opened before the verdict settled");
    link_->disconnect();
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
    hooks_->on_world_destroy();
}

// A different (seed, slot) authenticated, and the mod owns one save per (seed, slot), so start clean. ApSessionEnded lands ahead of the new ApConnected,
// so that connection's items and checks apply on top. A reconnect to the same seed and slot never reaches here (#152). connection_ is left to
// on_session_reset, which runs on the old connection's ApDisconnected and again on this one's ApConnected.
void App::clear_session_state()
{
    rando_->reset_session(); // releases the save; must precede session_.reset()
    // Queued grants belong to the old save and must not reach the next one (#175).
    granter_->discard_pending();
    session_.reset();
    state_.reset_session();
    hooks_->clear_session_state();
    // Reset with the received stream so UpgradeState::applied_ returns to zero too and the next recompute cannot write a downgrade into the live save.
    upgrades_ = UpgradeState{};
    pal::logf(pal::LogLevel::Info, "session: cleared previous AP connection state");
}

void App::ensure_inbound_ready()
{
    if (session_ || !state_.authenticated())
        return;
    remember_successful_login(); // first tick after authentication: this target worked
    const std::string seed = state_.seed();
    const std::string slot = std::to_string(state_.player_slot());
    const std::string key = bundle_store_.path_for(seed, slot).string();
    session_.emplace([this, seed, slot] { return bundle_store_.load_state(seed, slot); },
                     [this, seed, slot](std::string_view text) { bundle_store_.stage_state(seed, slot, text); }, *granter_, state_,
                     [this] { return hooks_->location_hooks_.credit_kear_key(player_tracker::player()); }, // vanilla-kear key grant (#130)
                     [this](int modifier_index) { return hooks_->traps_.arm(modifier_index); });
    pal::logf(pal::LogLevel::Info, "inbound: state loaded (%s); granter live", key.c_str());
    hooks_->modifier_hooks_.set_ap_slot(session_->save.game_slot()); // restore the AP-game slot (skip capture if known)
    rando_->attach_save_state(session_->save);
    pal::logf(pal::LogLevel::Info, "outbound: bridge attached to %s", key.c_str());
}

void App::reconcile_server_checked()
{
    if (!session_) // ids remain pending in ApState until the save attaches
        return;
    const auto ids = state_.take_server_checked_pending();
    bool changed = false;
    for (std::int64_t id : ids)
        if (id >= 0 && id <= std::numeric_limits<int>::max())
            changed |= rando_->reconcile_server_checked(static_cast<int>(id));
    if (changed)
        session_->save.stage(); // one stage per reconcile pass (batched)
}

void App::connect(const std::string &server, const std::string &slot, const std::string &password)
{
    // ApState is game-thread data and this is the render thread, so the UI reads the reason via gate_.status().
    if (const GateStatus gate = gate_.status(); should_refuse_connect(gate.enforcing, gate.verdict))
    {
        pal::logf(pal::LogLevel::Error, "gate: refusing connect - %s", gate.reason.c_str());
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
    // Render thread. The game-thread-only connection_ is reset in on_session_reset (see ctor).
    link_->disconnect();
}

ConnectionStatus App::connection_status() const
{
    return ConnectionStatus{state_.phase(), state_.detail()};
}

} // namespace mth
