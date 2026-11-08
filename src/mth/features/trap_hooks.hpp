#pragma once

#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mth/core/rules/trap_state.hpp"
#include "mth/features/room_clock.hpp"

namespace mth
{

// Runs the modifier traps: owns the countdowns and does every game-facing write, so TrapState stays pure. The bit goes into the runtime cheat mask only, so it
// applies the same frame and never reaches the save file or its achievement byte.
class TrapHooks
{
  public:
    TrapHooks();

    TrapHooks(const TrapHooks &) = delete;
    TrapHooks &operator=(const TrapHooks &) = delete;

    // Game thread. Forces the modifier on and starts its countdown. A positive seconds_override
    // replaces the table's duration; the dev console uses it to test short traps.
    TrapArm arm(int modifier_index, float seconds_override = 0.0f);

    // Any thread. Defers arm() to the next tick so an off-thread caller cannot mutate the countdown lists mid-walk; the outcome is logged.
    void queue_arm(int modifier_index, float seconds_override = 0.0f);

    // Game thread, once per frame. Ages the running traps, lifts what ran out, re-asserts the rest.
    void tick();

    // Lifts every running trap. Session change and teardown call it so no modifier is stranded on.
    void clear();

    // Render thread. Reads the snapshot tick() publishes; state_ itself stays game-thread-only.
    [[nodiscard]] std::vector<std::string> status_lines() const;

  private:
    [[nodiscard]] bool gameplay_advanced();
    [[nodiscard]] float take_delta();
    void drain_pending_arm();
    void advance_frame();
    void publish_active();
    void retry_pending_off();

    TrapState state_;
    RoomClock room_clock_;
    bool timing_source_logged_ = false;
    // Indices whose lift-write failed (set_runtime_modifier returned false, "not right now"). TrapState has dropped them; tick() retries.
    std::vector<int> pending_off_;
    // Indices arm() has warned about (NotReady), tracked per index since traps can stall concurrently. Erased once that index's write lands, so a later stall
    // warns again.
    std::set<int> notready_warned_;

    // Guards only the two members the render thread touches; the countdown lists stay game-thread-only.
    mutable std::mutex mtx_;
    std::vector<std::pair<int, float>> pending_arm_; // (modifier index, seconds override) queued for the game thread
    std::vector<ActiveTrap> snapshot_;               // what was running at the end of the last tick
};

} // namespace mth
