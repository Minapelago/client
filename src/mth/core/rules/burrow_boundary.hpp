#pragma once

namespace mth
{

// What crossing the land/water boundary in an ungranted mode costs the player.
enum class BoundaryAction
{
    None,
    FallIn, // hand the player to the game's own on-foot deep-water fall: shore respawn plus pit damage,
            // and the game decides whether that is fatal
    Emerge, // surface, which needs no ability
};

// twin: src/pal/*/game_*.cpp repl_burrow_ground / repl_burrow_jump (the arm/disarm sites).
// Player::SetBurrowGround is classify-and-commit, so once burrowed the game flips between ground-burrow and water-swim through a path that gate
// never sees, and owning one of Burrow/Swim grants the other (#163). Watching the boundary avoids the burrow-mode field, whose offset drifts.
//
// arm(deep) on a commit the gate allowed, disarm() on the emerge commit. Callers must also feed
// observe_burrowing() every tick, because most ways a burrow ends never reach the emerge commit.
class BurrowBoundaryGate
{
  public:
    // Consecutive agreeing polls before a changed water reading counts. The raw discriminator is an exact ground-bitmask compare that also goes
    // false under covering geometry, so it flickers along shorelines where the game's debounced test does not; one spurious reading would punish a
    // player walking a beach.
    static constexpr int kWaterConfirmTicks = 10;

    // Consecutive non-burrow readings before the arm drops. A burrow requested by another entity's state machine (a room transition) applies a
    // frame late, and disarming on that frame would switch the gate off for the rest of that burrow. Damage and death hold the reading, so they
    // still disarm.
    static constexpr int kBurrowConfirmGraceTicks = 2;

    void arm(bool deep)
    {
        state_ = deep ? State::Swim : State::Burrow;
        last_deep_ = deep;
        confirmed_ = kWaterConfirmTicks; // the commit itself is a confirmed reading
        not_burrowing_ = 0;
    }

    void disarm()
    {
        state_ = State::None;
    }

    [[nodiscard]] bool armed() const
    {
        return state_ != State::None;
    }

    // Live "is the player still in the burrow state" reading, polled every tick.
    void observe_burrowing(bool burrowing)
    {
        if (burrowing)
        {
            not_burrowing_ = 0;
        }
        else if (++not_burrowing_ >= kBurrowConfirmGraceTicks)
        {
            disarm();
        }
    }

    // No water reading this tick (null WaterListener during a room transition). Restart the confirmation so a partial streak can't bridge the gap.
    void reading_unavailable()
    {
        confirmed_ = 0;
    }

    // What to do about a crossing into an ungranted mode. Returning one consumes the arm, so the fall or emerge applies once.
    BoundaryAction observe(bool deep, bool burrow_blocked, bool swim_blocked)
    {
        if (state_ == State::None)
            return BoundaryAction::None;
        if (deep != last_deep_)
        {
            last_deep_ = deep;
            confirmed_ = 1;
        }
        else if (confirmed_ < kWaterConfirmTicks)
        {
            ++confirmed_;
        }
        if (confirmed_ < kWaterConfirmTicks)
            return BoundaryAction::None;

        if (state_ == State::Burrow && deep)
        {
            if (swim_blocked)
            {
                state_ = State::None;
                return BoundaryAction::FallIn;
            }
            state_ = State::Swim; // owns Swim, so an ordinary crossing, but the mode did change
        }
        else if (state_ == State::Swim && !deep)
        {
            if (burrow_blocked)
            {
                state_ = State::None;
                return BoundaryAction::Emerge;
            }
            state_ = State::Burrow;
        }
        return BoundaryAction::None;
    }

  private:
    enum class State
    {
        None,
        Burrow,
        Swim
    };
    State state_ = State::None;
    bool last_deep_ = false; // most recent raw water reading
    int confirmed_ = 0;      // consecutive polls agreeing with last_deep_
    int not_burrowing_ = 0;  // consecutive polls reading not-in-burrow-state
};

} // namespace mth
