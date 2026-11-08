#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "mth/core/ap/ability_ids.hpp"
#include "mth/core/ap/ap_state.hpp"
#include "mth/core/ap/broadcast.hpp"
#include "mth/core/ap/connect_resend_gate.hpp"
#include "mth/core/ap/inbound_granter.hpp"
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
class TrapHooks;

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

// Snapshot of the AP safety gate, copied by value because the UI reads it on the render thread while the game thread writes the gate.
struct GateStatus
{
    GateVerdict verdict{GateVerdict::Pending};
    bool enforcing{false};
    std::string reason; // empty unless refused
};

// Composition root. Logger and hook engine are PAL globals; App owns everything else.
class App
{
  public:
    App();
    ~App() = delete; // leaked on purpose, see entry.cpp

    App(const App &) = delete;
    App &operator=(const App &) = delete;

    void drive_tick();                     // called each fixed update
    void drain_grants();                   // called from the World::Update pre-hook
    void on_world_update_end(void *world); // called after World::Update
    void on_world_destroy();               // called on World teardown; drops the cached Player*
    void gate_note_worldupdate();          // called on the native WorldUpdate; proves the mod-hook path is live

    // Console and login-window commands. Called on the overlay render thread; must not block.
    void connect(const std::string &server, const std::string &slot, const std::string &password);
    void disconnect();
    [[nodiscard]] ConnectionStatus connection_status() const;
    [[nodiscard]] SavedLogin saved_login() const;
    [[nodiscard]] std::vector<std::string> status_lines() const;
    [[nodiscard]] std::vector<std::string> item_lines() const;
    [[nodiscard]] GateStatus gate_status() const;
    void set_gate_enforcing(bool on);                              // dev override of the default-on enforcement
    void give_item(std::int64_t ap_item_id);                       // manual test path; bypasses dedup
    void remove_lock(int slot);                                    // pre-open/remove a KeyBlock by slot (AP runtime path)
    void set_modifier(int idx, bool on);                           // live toggle a continuous modifier
    void lock_modifiers(bool armed);                               // arm/disarm gameplay-modifier lockdown
    void set_stat_caps(int attack, int defense, int sidearm);      // force per-stat level caps (offline test)
    void set_ability_randomized(Ability ability, bool randomized); // offline test: mark randomized + arm enforcement
    void enable_deathlink(bool on);                                // enable/disable deathlink
    void set_lit_lamps(std::uint32_t lamp_mask);                   // offline test: force Ossex fountain lamps lit (bit i = lamp i)
    void save_test(const std::string &op);                         // save-takeover validation; dev only
    // offline test: queue one AP trap for the next tick; seconds <= 0 uses the trap table's duration. The outcome goes to the log.
    void fire_trap(int modifier_index, float seconds);
    // dev probe: log every rainbow switch in the player's room (#28). Results go to the log because the walk runs on the game thread.
    void probe_switches();
    // offline test: drive the Mirrors End switches with no AP session. It skips the bound-save check on the durable write, so use a scratch save.
    void set_mirror_switch_override(bool on);

  private:
    void remember_successful_login(); // persist the attempted target once the server authenticates
    // Drops everything the previous AP session accumulated. Game thread, on the link's ApSessionEnded (a different seed/slot authenticated).
    void clear_session_state();
    void ensure_inbound_ready(); // lazily builds save_state_ + inbound_ once connected
    // Merges ApState's server-reported checked locations (Collect / coop) into the save-state checked set once inbound is ready. Never re-sends; persists
    // once per pass. Called each drive_tick.
    void reconcile_server_checked();
    void enforce_vial_capacity(); // re-assert the AP vial count through the mod-API accessors (#171)
    void enforce_wallet_cap();    // clamp live bones to the AP wallet cap (#112); no-op unless authed + wallet_cap
    ApState state_;
    // Declared before hooks_ and save_state_, which hold references into it.
    ApSaveBundleStore bundle_store_;
    // AP session. banner_queue_ is the net->render mailbox the coordinator pushes to and the overlay draws from, so it outlives both.
    BannerQueue banner_queue_;
    std::unique_ptr<IApLink> link_;
    std::unique_ptr<ApCoordinator> coordinator_;
    std::unique_ptr<AreaReporter> area_reporter_;
    std::unique_ptr<RandoBridge> rando_; // net<->hooks bridge for outbound checks
    // Game::FixedUpdate detour, the tick source; the World::Update/WorldUpdateEnd/WorldDestroy events come from native mod hooks.
    pal::ScopedHook fixed_update_hook_;
    std::unique_ptr<HookManager> hooks_;
    std::optional<ApSaveState> save_state_;
    // Received-item grant path, built once connected and dropped at a session boundary. inbound_ references granter_ and save_state_, so it dies first.
    std::optional<ItemGranter> granter_;
    std::optional<InboundGranter> inbound_;
    std::unique_ptr<TrapHooks> traps_;
    std::atomic<bool> pending_inbound_death_{false};
    // Attribution for the death pending_inbound_death_ arms, written and read in the same game-thread tick, so no lock. Read only when the flag is set.
    std::string pending_death_text_;
    // AP safety gate. Static inputs settle in the ctor; liveness arrives on the first native WorldUpdate (title screen). gate_verdict_ is atomic for the
    // overlay's per-frame read on the render thread; gate_reason_ is copied out under its own lock.
    GateInputs gate_inputs_;
    GateLatch gate_latch_;
    std::atomic<GateVerdict> gate_verdict_{GateVerdict::Pending};
    std::atomic<bool> gate_enforcing_{true}; // `gate enforce off` clears it
    mutable std::mutex gate_reason_mutex_;
    std::string gate_reason_;
    void gate_tick(); // game thread: advance the liveness deadline, latch, log transitions
    bool first_tick_logged_{false};
    SessionPolicy policy_;
    UpgradeState upgrades_;
    ConnectResendGate resend_gate_; // fires flush() once per (re)connection
    // Last connect target, auto-filled into the login window next launch. connect() stashes the attempt on the render thread and the game thread commits
    // it once the server authenticates, so the mutex covers the prefs and the pending pair.
    mutable std::mutex login_mutex_;
    LoginPrefs login_prefs_;
    std::string pending_server_;
    std::string pending_slot_;
    std::map<int, mth::ScoutInfo> scouts_; // game thread only, by collection slot
    std::unique_ptr<OverlayRoot> overlay_root_;
};

} // namespace mth
