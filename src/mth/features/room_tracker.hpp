#pragma once

#include <cstdint>

namespace mth::room_tracker
{

// Caches the live screen identity (room index plus gamestate, since room indices repeat across gamestates), polled each World::Update end. Game thread only.

void poll(); // game thread, per World::Update end

// Packed screen id: gamestate in the high 16 bits, room index in the low 16. false until the first gameplay reading. Mid-transition readings are dropped
// because the game reports room 0 for both "no room matched" and the real room 0.
[[nodiscard]] bool current_screen(std::uint32_t *out);

} // namespace mth::room_tracker
