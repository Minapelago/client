#pragma once

namespace mth::player_tracker
{

// Serves the live Player* and a once-per-tick position cache. Position comes from mod::player_position, which checks PlayerGetWorld first and so fails cleanly
// where PlayerGetPos3 alone would fault (the pre-World::Update spawn window). The Player* is the game's live-player pointer where the build exposes it, else
// the last note_player capture. Game thread only.

[[nodiscard]] void *player();
[[nodiscard]] bool position(float out[3]); // false until the first successful sample
void sample_position();                    // per-tick; the only writer of the position cache
void note_player(void *player);            // refresh the fallback capture from another game-thread hook
void invalidate_player();                  // drop the fallback capture on world teardown, which frees its object

} // namespace mth::player_tracker
