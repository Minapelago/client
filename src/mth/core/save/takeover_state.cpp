#include "mth/core/save/takeover_state.hpp"

namespace mth
{

bool takeover_settled(TakeoverStep step) noexcept
{
    return step == TakeoverStep::Idle || step == TakeoverStep::Running || step == TakeoverStep::Failed;
}

TakeoverStep next_takeover_step(TakeoverStep current, const TakeoverInputs &in)
{
    if (takeover_settled(current))
        return current;

    // modding API inert; never drop the player into a vanilla slot at any point in the sequence
    if (!in.save_api_ready)
        return TakeoverStep::Failed;

    const bool timed_out = in.frames_in_step > kMaxFramesPerStep;

    // No default case: a new step must be handled here or the build fails.
    switch (current)
    {
    case TakeoverStep::AwaitingMenu:
        // Advanced by the profile-menu callback, which is where the live menu pointer is.
        return timed_out ? TakeoverStep::Failed : current;

    case TakeoverStep::Launching:
        if (in.gameplay_entered)
            return TakeoverStep::Running;
        return timed_out ? TakeoverStep::Failed : current;

    case TakeoverStep::Idle:
    case TakeoverStep::Running:
    case TakeoverStep::Failed:
        break; // settled, handled above
    }
    return TakeoverStep::Failed;
}

void InputBlockState::rearm() noexcept
{
    frames_ = 0;
}

bool InputBlockState::advance(TakeoverStep step, bool on_profile_select) noexcept
{
    bool wanted = false;
    // No default case: a new step must be handled here or the build fails.
    switch (step)
    {
    case TakeoverStep::AwaitingMenu:
    case TakeoverStep::Launching:
        wanted = true;
        break;

    // The bounce back to the title leaves the menu live for a few more frames, and a confirm landing
    // there opens exactly the vanilla file the takeover just refused to run.
    case TakeoverStep::Failed:
        wanted = on_profile_select;
        break;

    case TakeoverStep::Idle:
    case TakeoverStep::Running:
        break;
    }
    blocked_ = wanted && frames_ <= kMaxInputBlockFrames;
    // Only while blocking, so a tripped ceiling stops counting and cannot fall back under it.
    if (blocked_)
        ++frames_;
    return blocked_;
}

const char *takeover_step_name(TakeoverStep step)
{
    switch (step)
    {
    case TakeoverStep::Idle:
        return "Idle";
    case TakeoverStep::AwaitingMenu:
        return "AwaitingMenu";
    case TakeoverStep::Launching:
        return "Launching";
    case TakeoverStep::Running:
        return "Running";
    case TakeoverStep::Failed:
        return "Failed";
    }
    return "Unknown";
}

const char *flush_refusal(const FlushInputs &in) noexcept
{
    if (in.step != TakeoverStep::Running)
        return "no claim on the slot";
    // #152: outside gameplay the live slot is the cleared one, and committing it destroys the run.
    if (!in.in_gameplay)
        return "not in gameplay";
    // A reconnect since begin() must not file this blob under a session that is no longer live.
    if (in.current_seed != in.begin_seed || in.current_slot != in.begin_slot)
        return "AP identity changed since begin()";
    return nullptr;
}

} // namespace mth
