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

// Last connection target that authenticated successfully; empty if there is none.
// Password is never persisted.
struct SavedLogin
{
    std::string server;
    std::string slot;
};

// Snapshot of the AP safety gate, copied by value: the UI reads it from the render thread
// while the gate is written on the game thread.
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
    ~App();

    App(const App &) = delete;
    App &operator=(const App &) = delete;

    // False until the ctor finishes wiring every member. The tick trampolines check this before forwarding
    // any game-thread event, so a hook that goes live mid-construction can't reach a half-built App.
    [[nodiscard]] bool ready() const noexcept
    {
        return ready_.load();
    }

    void drive_tick();                     // called each fixed update (only once ready())
    void drain_grants();                   // called from the World::Update pre-hook (only once ready())
    void on_world_update_end(void *world); // called after World::Update (only once ready())
    void on_world_destroy();               // called on World teardown (only once ready()); drops the cached Player*
    void gate_note_worldupdate();          // called on the native WorldUpdate; proves the mod-hook path is live

    // Console and login-window commands. Called on the overlay render thread; must not block.
    void connect(const std::string &server, const std::string &slot, const std::string &password);
    void disconnect();
    [[nodiscard]] ConnectionStatus connection_status() const;
    [[nodiscard]] SavedLogin saved_login() const;
    [[nodiscard]] std::vector<std::string> status_lines() const;
    [[nodiscard]] std::vector<std::string> item_lines() const;
    [[nodiscard]] GateStatus gate_status() const;
    void set_gate_enforcing(bool on);                              // dev override: enforcement is on by default, so `off` is the escape hatch
    void give_item(std::int64_t ap_item_id);                       // manual test path; bypasses dedup
    void remove_lock(int slot);                                    // pre-open/remove a KeyBlock by slot (AP runtime path)
    void set_modifier(int idx, bool on);                           // live toggle a continuous modifier
    void lock_modifiers(bool armed);                               // arm/disarm gameplay-modifier lockdown
    void set_stat_caps(int attack, int defense, int sidearm);      // force per-stat level caps (offline test)
    void set_ability_randomized(Ability ability, bool randomized); // offline test: mark randomized + arm enforcement
    void enable_deathlink(bool on);                                // enable/disable deathlink
    void set_lit_lamps(std::uint32_t lamp_mask);                   // offline test: force Ossex fountain lamps lit (bit i = lamp i)
    void save_test(const std::string &op);                         // save-takeover validation; dev only
    // offline test: queue one AP trap for the next game tick. seconds <= 0 uses the trap table's own
    // duration, and the outcome goes to the log rather than coming back here.
    void fire_trap(int modifier_index, float seconds);
    // dev probe: log every rainbow switch in the room the player is standing in (#28). The findings go
    // to the log rather than coming back here, because the walk runs on the game thread.
    void probe_switches();
    // offline test: drive the Mirrors End switches with no AP session. Bypasses the bound-save gate that
    // the durable write otherwise waits for, so it is a scratch-save tool.
    void set_mirror_switch_override(bool on);

  private:
    void remember_successful_login(); // persist the attempted target once the server authenticates
    // Drop everything the previous AP session accumulated, so the next one behaves like the first since
    // launch. Game thread; fires only on the link's ApSessionEnded (a different seed/slot authenticated).
    void clear_session_state();
    void ensure_inbound_ready(); // lazily builds save_state_ + inbound_ once connected
    // Drain ApState's server-reported checked locations into the save-state checked set (Collect / coop),
    // once inbound is ready. Never re-sends; persists once per pass. Called each drive_tick.
    void reconcile_server_checked();
    void enforce_vial_capacity(); // re-assert the AP vial count through the offset-free mod-API accessors (#171)
    void enforce_wallet_cap();    // clamp live bones to the AP wallet cap (#112); no-op unless authed + wallet_cap
    // Destruction order: overlay first, then the tick hooks, hooks_, inbound_/granter_, then the AP
    // session members rando_ -> area_reporter_ -> coordinator_ -> link_ (stops the net thread last).
    ApState state_;
    // Ahead of hooks_ and save_state_: both hold references into it, and members are destroyed in
    // reverse declaration order.
    ApSaveBundleStore bundle_store_;
    // AP session. banner_queue_ is the net->render mailbox: the coordinator pushes to it and the overlay
    // draws from it, so it outlives both coordinator_ and overlay_root_.
    BannerQueue banner_queue_;
    std::unique_ptr<IApLink> link_;
    std::unique_ptr<ApCoordinator> coordinator_;
    std::unique_ptr<AreaReporter> area_reporter_;
    std::unique_ptr<RandoBridge> rando_; // net<->hooks bridge for outbound checks
    // Game::FixedUpdate detour, the tick source. The World::Update/WorldUpdateEnd/WorldDestroy events come
    // from native mod hooks installed next to it.
    pal::ScopedHook fixed_update_hook_;
    std::unique_ptr<HookManager> hooks_;
    std::optional<ApSaveState> save_state_;
    // Received-item grant path. inbound_ references granter_ and save_state_, so it goes first; it is
    // built lazily once connected and dropped at a session boundary.
    std::optional<ItemGranter> granter_;
    std::optional<InboundGranter> inbound_;
    std::unique_ptr<TrapHooks> traps_;
    std::atomic<bool> pending_inbound_death_{false};
    // Attribution line for the death pending_inbound_death_ arms. Written by the coordinator drain and read
    // later in the same game-thread tick, so it needs no lock; it is deliberately not cleared alongside the
    // flag, because it is only ever read when the flag was set, and the next deathlink overwrites it.
    std::string pending_death_text_;
    // Gates the tick entry points until construction finishes. The game-thread tick hooks go live mid-ctor
    // (Game::FixedUpdate is detoured before hooks_ is even assigned), so on a fast-initializing host
    // the first tick can land on a half-built App and deref a null member. Release on the last ctor line,
    // acquire at the top of each tick; cleared first in the dtor so teardown can't be ticked either.
    std::atomic<bool> ready_{false};
    // AP safety gate. Static inputs are settled in the ctor; liveness arrives on the first native
    // WorldUpdate, which fires at the title screen. verdict_ is atomic because the overlay reads it
    // every frame on the render thread; the reason string is copied out under its own lock.
    GateInputs gate_inputs_;
    GateLatch gate_latch_;
    std::atomic<GateVerdict> gate_verdict_{GateVerdict::Pending};
    std::atomic<bool> gate_enforcing_{true}; // `gate enforce off` is the escape hatch
    mutable std::mutex gate_reason_mutex_;
    std::string gate_reason_;
    void gate_tick(); // game thread: advance the liveness deadline, latch, log transitions
    bool first_tick_logged_{false};
    SessionPolicy policy_;
    UpgradeState upgrades_;
    ConnectResendGate resend_gate_; // fires flush() once per (re)connection
    // Last-used connect target, auto-filled into the login window next launch. connect() runs on the
    // overlay render thread and only stashes what was attempted; the commit happens on the game thread
    // once the server authenticates, so the mutex covers both the prefs and the pending pair.
    mutable std::mutex login_mutex_;
    LoginPrefs login_prefs_;
    std::string pending_server_;
    std::string pending_slot_;
    std::map<int, mth::ScoutInfo> scouts_; // game thread only, by collection slot
    std::unique_ptr<OverlayRoot> overlay_root_;
};

} // namespace mth
