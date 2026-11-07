#pragma once

#include <functional>
#include <string>

#include "mth/core/rules/death_broadcast_gate.hpp"
#include "mth/features/room_clock.hpp"

namespace mth
{

// Deathlink via the native modding API: poll() edge-detects a local death (the Player death-guard byte) each tick and broadcasts it; kill() applies an inbound
// death via mod::player_die(). A one-shot suppress latch stops an applied death from echoing back.
class DeathHooks
{
  public:
    // Gameplay ticks an inbound death stays latched while it cannot be applied; both blockers (no player mid-transition, not settled after a local death) clear
    // in 1-4s. Counted in gameplay ticks so a menu freeze does not burn the window (#164).
    static constexpr int kPendingInboundDeathTicks = ticks_for_seconds(10.0); // 1200: 10s at 120 Hz, 20s at 60 Hz

    // Ticks a local death waits to reach the death screen before the broadcast is dropped; the sequence takes about 1.5s plus a screen transition. Wall ticks,
    // because the transition runs through a menu-like gamestate where the world's queues stall.
    static constexpr int kDeathConfirmTicks = ticks_for_seconds(6.0); // 720: 6s at 120 Hz, 12s at 60 Hz

    // on_local_death receives the outbound cause detail (the predicate only; ApLink prefixes our slot name).
    DeathHooks(std::function<void(const std::string &)> on_local_death, std::function<void *()> get_player);
    ~DeathHooks();
    DeathHooks(const DeathHooks &) = delete;
    DeathHooks &operator=(const DeathHooks &) = delete;

    void poll();  // game-thread, per-tick: detect a fresh local death edge and broadcast it
    void kill();  // game-thread: apply an inbound death, or latch it for retry if it cannot land yet
    void reset(); // drop the latched inbound and held outbound deaths of the ending session

  private:
    bool gameplay_advanced();       // did the world's gameplay queues run this tick
    bool try_apply_inbound_death(); // false only when the block is transient and the death should be retried
    void drive_pending_death(bool advanced);
    void drive_pending_broadcast(); // hold a local death until the death screen says it landed

    DeathBroadcastGate gate_;
    std::function<void(const std::string &)> on_local_death_;
    std::function<void *()> get_player_;
    int last_alive_spark_{0}; // spark sampled on the last alive tick; the death drop zeroes the live value before the edge
    RoomClock room_clock_;
    bool room_clock_stalled_{false}; // last tick froze with the world unpaused (a transition)
    int pending_kill_ticks_{0};      // gameplay ticks left to land a received death that could not be applied yet
    int pending_broadcast_ticks_{0}; // ticks left for our own death to reach the death screen before we drop it
};

} // namespace mth
