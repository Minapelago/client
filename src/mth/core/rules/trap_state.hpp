#pragma once

#include <vector>

namespace mth
{

// What arming a trap did. Armed and Skipped consume the receipt, NotReady retries it next tick without blocking later receipts, and Unavailable
// consumes it with a warning.
enum class TrapArm
{
    Armed,       // the modifier bit is set and the countdown is running
    NotReady,    // the manager is bound but the write was refused this frame; retry
    Unavailable, // this build can never do it; give up on the receipt
    Skipped,     // the player already had that modifier on, so the trap would be a no-op
};

// One running trap: the modifier index it forces on, and the gameplay seconds left before it lifts.
struct ActiveTrap
{
    int index{0};
    float remaining{0.0f};
};

// Countdowns for the modifier traps currently forced on. Pure: it takes a delta and hands back the
// indices whose time ran out, so the owner does every game-facing write. Traps stack, one timer per
// index, in arming order.
class TrapState
{
  public:
    // Starts a trap, or extends one already running; a re-arm never shortens a live trap. Ignores a non-positive duration.
    void arm(int index, float seconds);

    // Ages every trap by dt gameplay seconds and returns the expired indices in arming order. A non-positive dt (paused or loading frame) ages nothing.
    std::vector<int> advance(float dt);

    // The running traps in arming order, with the seconds each has left.
    [[nodiscard]] const std::vector<ActiveTrap> &traps() const
    {
        return traps_;
    }

    // Empties the state and returns what was running, so the owner can lift those modifiers.
    [[nodiscard]] std::vector<int> clear();

    [[nodiscard]] bool empty() const
    {
        return traps_.empty();
    }

  private:
    std::vector<ActiveTrap> traps_;
};

} // namespace mth
