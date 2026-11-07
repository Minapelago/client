#pragma once

#include <string_view>

namespace mth
{

// Save-takeover progress. Every step waits on the game.
enum class TakeoverStep
{
    Idle,
    AwaitingMenu, // waiting for the profile-select menu to reach the state a file is confirmed from
    Launching,    // menu driven; the game is activating and starting the slot
    Running,
    Failed
};

struct TakeoverInputs
{
    bool save_api_ready{false};
    bool gameplay_entered{false};
    int frames_in_step{0};
};

// Any wait longer than this fails the takeover, so the player is never left on a dead menu.
inline constexpr int kMaxFramesPerStep = 600;

// Ceiling on how long game input stays swallowed, so a wedge the step timeouts miss still returns the controller.
inline constexpr int kMaxInputBlockFrames = 1800;

// Settled outcomes.
[[nodiscard]] bool takeover_settled(TakeoverStep step) noexcept;

TakeoverStep next_takeover_step(TakeoverStep current, const TakeoverInputs &in);

// Decides whether the game sees input, and owns the frame budget behind that decision. Once the ceiling trips, the block stays off until the next
// claim.
class InputBlockState
{
  public:
    // Fresh budget for a newly claimed launch.
    void rearm() noexcept;

    // Whether the game sees no input this frame. The profile-select menu is live briefly before the takeover drives it, long enough for a held
    // confirm to open a vanilla file. on_profile_select: the file-select menu is the live gamestate.
    [[nodiscard]] bool advance(TakeoverStep step, bool on_profile_select) noexcept;

    [[nodiscard]] bool blocked() const noexcept
    {
        return blocked_;
    }

  private:
    bool blocked_{false};
    int frames_{0};
};

const char *takeover_step_name(TakeoverStep step);

// What stands between a cleared title slot and the player's run, kept out of the feature so tests can compile it. Blob emptiness is not an
// input: reading the slot costs a full serialize, so the caller does that only once these pass.
struct FlushInputs
{
    TakeoverStep step{TakeoverStep::Idle};
    bool in_gameplay{false};
    std::string_view begin_seed;
    std::string_view begin_slot;
    std::string_view current_seed;
    std::string_view current_slot;
};

// Null to commit, otherwise the reason, for the log.
[[nodiscard]] const char *flush_refusal(const FlushInputs &in) noexcept;

} // namespace mth
