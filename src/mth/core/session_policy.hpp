#pragma once

namespace mth
{

// Which enforcement features the dev console drove this session. Each latches for the session and
// makes its feature enforce without an AP session; while authenticated everything enforces anyway.
// Vanilla play (nothing set, not authed) is never affected.
struct SessionPolicy
{
    bool console_modifiers{false};
    bool forced_caps{false}; // console caps: fixed caps, skip AP recompute
    bool console_abilities{false};
};

} // namespace mth
