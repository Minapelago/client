#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mth/core/ap/ability_ids.hpp"
#include "mth/core/ap/ap_state.hpp"
#include "mth/core/ap/broadcast.hpp"
#include "mth/core/ap/connect_resend_gate.hpp"
#include "mth/core/ap/inbound_granter.hpp"
#include "mth/core/console/command_queue.hpp"
#include "mth/core/console/login_prefs.hpp"
#include "mth/core/rules/ap_gate.hpp"
#include "mth/core/rules/progression.hpp"
#include "mth/core/rules/session_policy.hpp"
#include "mth/core/save/ap_save_bundle.hpp"
#include "mth/core/save/ap_save_state.hpp"
#include "mth/features/item_granter.hpp"
#include "pal/scoped_hook.hpp"

namespace mth
{

class HookManager;
class IApLink;
class ApCoordinator;
class AreaReporter;
class RandoBridge;
class OverlayRoot;

struct ConnectionStatus
{
    ConnectionPhase phase{ConnectionPhase::Disconnected};
    std::string detail; // error/status text for display
};

// Last connection target that authenticated; empty if none. The password is never persisted.
struct SavedLogin
{
    std::string server;
    std::string slot;
};

// Composition root. Logger and hook engine are PAL globals; App owns everything else.
class App
{
  public:
    // Runs on the game thread inside MinaMod_Init, before the first frame, so it must never wait for a tick or a World.
    App();
    ~App() = delete; // leaked on purpose, see entry.cpp

    App(const App &) = delete;
    App &operator=(const App &) = delete;

    void drive_tick();                                     // called each fixed update
    void on_world_update(void *world, float step_seconds); // called from the World::Update pre-hook; drains grants
    void on_world_update_end(void *world);                 // called after World::Update
    void on_world_destroy();                               // called on World teardown; drops the cached Player*
    // Called on every native WorldUpdate; proves the mod-hook path is live and, for a gamestate World, that the World flag still reads.
    void gate_note_worldupdate(bool gamestate_world);

    // Console and login-window commands. Called on the overlay render thread; must not block.
    void connect(const std::string &server, const std::string &slot, const std::string &password);
    void disconnect();
    [[nodiscard]] ConnectionStatus connection_status() const;
    [[nodiscard]] SavedLogin saved_login() const;

    // Console-only commands. Game thread: the console posts them to commands_.
    [[nodiscard]] std::vector<std::string> status_lines() const;
    [[nodiscard]] std::vector<std::string> item_lines() const;
    void give_item(std::int64_t ap_item_id);                       // manual test path; bypasses dedup
    void remove_lock(int slot);                                    // pre-open/remove a KeyBlock by slot (AP runtime path)
    void set_modifier(int idx, bool on);                           // live toggle a continuous modifier
    void lock_modifiers(bool armed);                               // arm/disarm gameplay-modifier lockdown
    void set_stat_caps(int attack, int defense, int sidearm);      // force per-stat level caps (offline test)
    void set_ability_randomized(Ability ability, bool randomized); // offline test: mark randomized + arm enforcement
    void enable_deathlink(bool on);                                // enable/disable deathlink
    void set_lit_lamps(std::uint32_t lamp_mask);                   // offline test: force Ossex fountain lamps lit (bit i = lamp i)
    void save_test(const std::string &op);                         // save-takeover validation; dev only
    // offline test: arm one AP trap; seconds <= 0 uses the trap table's duration. The outcome goes to the log.
    void fire_trap(int modifier_index, float seconds);
    // dev probe: log every rainbow switch in the player's room (#28) from this tick's mirror-switch pass.
    void probe_switches();
    // offline test: drive the Mirrors End switches with no AP session. It skips the bound-save check on the durable write, so use a scratch save.
    void set_mirror_switch_override(bool on);
    // dev: toggle the ghost echo of the local player, sent through the server.
    void toggle_ghost_echo();

    // The overlay reads status() every frame and `gate enforce` calls set_enforcing(), both on the render thread.
    SafetyGate gate_;
    // The dev console posts here from the render thread; drive_tick drains it on the game thread.
    CommandQueue commands_;

  private:
    // One AP (seed, slot). Built on the authenticating tick and dropped whole when a different seed or slot authenticates. inbound references save, so it
    // is declared after it.
    struct SeedSession
    {
        SeedSession(ApSaveState::LoadFn load, ApSaveState::StoreFn store, IItemGranter &granter, ApState &state, std::function<bool()> credit_kear_key,
                    std::function<TrapArm(int)> arm_trap)
            : save(std::move(load), std::move(store)), inbound(granter, state, save, std::move(credit_kear_key), std::move(arm_trap))
        {
        }

        ApSaveState save;
        InboundGranter inbound;
        ConnectResendGate resend; // fires flush() once per (re)connection
    };

    // One AP connection, replaced by assignment on every ApConnected and ApDisconnected. Game thread only. LocationHooks holds the address of scouts, so this
    // stays a plain member.
    struct Connection
    {
        std::map<int, ScoutInfo> scouts; // by collection slot
        bool pending_inbound_death{false};
        std::string pending_death_text; // attribution for pending_inbound_death; read only while it is set
    };

    void remember_successful_login(); // persist the attempted target once the server authenticates
    // Drops everything the previous AP session accumulated. Game thread, on the link's ApSessionEnded (a different seed/slot authenticated).
    void clear_session_state();
    void ensure_inbound_ready(); // builds session_ on the first authenticated tick
    // Merges ApState's server-reported checked locations (Collect / coop) into the save-state checked set once inbound is ready. Never re-sends; persists
    // once per pass. Called each drive_tick.
    void reconcile_server_checked();
    void drop_refused_session(); // the gate refused after connect() let a session through
    ApState state_;
    // Declared before hooks_ and session_, which hold references into it.
    ApSaveBundleStore bundle_store_;
    // AP session. banner_queue_ is the game->render mailbox: the coordinator and drive_tick push banners on the game thread and the overlay drains them
    // on the render thread, so it outlives both.
    BannerQueue banner_queue_;
    std::unique_ptr<IApLink> link_;
    std::unique_ptr<ApCoordinator> coordinator_;
    std::unique_ptr<AreaReporter> area_reporter_;
    std::unique_ptr<RandoBridge> rando_; // net<->hooks bridge for outbound checks
    // Game::FixedUpdate detour, the tick source; the World::Update/WorldUpdateEnd/WorldDestroy events come from native mod hooks.
    pal::ScopedHook fixed_update_hook_;
    std::unique_ptr<HookManager> hooks_;
    // Received-item grant path. session_->inbound references it, so it is declared first.
    std::optional<ItemGranter> granter_;
    std::optional<SeedSession> session_;
    Connection connection_;
    bool first_tick_logged_{false};
    SessionPolicy policy_;
    UpgradeState upgrades_;
    // Last connect target, auto-filled into the login window next launch. connect() stashes the attempt on the render thread and the game thread commits
    // it once the server authenticates, so the mutex covers the prefs and the pending pair.
    mutable std::mutex login_mutex_;
    LoginPrefs login_prefs_;
    std::string pending_server_;
    std::string pending_slot_;
    std::unique_ptr<OverlayRoot> overlay_root_;
};

} // namespace mth
