#pragma once

namespace mth
{

// TitleScreen cursor correction while "Start Game" (index 0) is gated off. The game's own wrap only
// fires at the boundaries (raw index 3 or -1); moving Up from index 1 lands on 0 directly with no
// wrap, so a blind "snap to 1" would refuse that motion and freeze the cursor. `previous` is the
// index before the game's update, `current` is after.
[[nodiscard]] constexpr int skip_gated_option(int previous, int current) noexcept
{
    if (current != 0)
        return current;
    // current == 0 either by moving Up from 1 (continue upward, to 2) or wrapping Down from 2
    // (continue downward, to 1); previous == 0 (no movement) also lands on 1.
    return previous == 1 ? 2 : 1;
}
// Non-zero indices pass through; Up from 1 lands on 0 and continues to 2; Down from 2 wraps to 0 and
// continues to 1; no movement while on 0 lands on 1.
static_assert(skip_gated_option(0, 1) == 1 && skip_gated_option(0, 2) == 2 && skip_gated_option(2, 1) == 1);
static_assert(skip_gated_option(1, 0) == 2 && skip_gated_option(2, 0) == 1 && skip_gated_option(0, 0) == 1);

} // namespace mth
