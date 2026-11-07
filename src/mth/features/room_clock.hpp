#pragma once

#include "mod/mod_api.hpp"

namespace mth
{

// Whether the world's gameplay queues ran this tick. A paused world (any menu) runs only its pause queue; a paused game skips the world queue and stalls the
// room clock. Without the room-clock API every tick reads as advancing. One instance per owner, since the previous reading is per caller.
struct RoomClock
{
    enum class Tick
    {
        Paused,   // the world is paused
        Stalled,  // unpaused, but the room clock did not move: a transition or a death sequence
        Advanced, // gameplay ran
    };

    Tick sample()
    {
        if (mod::world_is_paused())
            return Tick::Paused;
        if (!mod::room_time_api_available())
            return Tick::Advanced;
        const float now = mod::room_time();
        const bool moved = now != last_;
        last_ = now;
        return moved ? Tick::Advanced : Tick::Stalled;
    }

    float last_{0.0f}; // previous room clock reading
};

} // namespace mth
