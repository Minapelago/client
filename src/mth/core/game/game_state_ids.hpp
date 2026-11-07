#pragma once

#include "MinaModEnums.h"

namespace mth
{

// Gamestate ids from the pinned mod ABI's GAMESTATE_* defines. The enum is flat but ordered: world/room states sit in [kGameStateFirstWorld,
// kGameStateLastWorld], and everything from the title screen up is a menu, overlay, cinematic or panorama. Menu worlds still tick World::Update
// and GetRoomIndex keeps the last room, so this range is the only "in a room" test.
inline constexpr int kGameStateFirstWorld = GAMESTATE_INTROBEACH;
inline constexpr int kGameStateLastWorld = GAMESTATE_GYM_WORLDLOADTEST2;
inline constexpr int kGameStateTitleScreen = GAMESTATE_TITLE_SCREEN;
inline constexpr int kGameStateProfileSelect = GAMESTATE_PROFILE_SELECT_MENU;
// The Mirrors End mirror hub: its own gamestate (separate from the Orrery) and small enough to gate the shortcut switches on. The apworld lists
// six rooms in it, all but one Mirrors End proper.
inline constexpr int kGameStateMirrorHub = GAMESTATE_ASTRAL_ORRERY_MIRROR_HUB;
// The mirror hub's switch room, the only room the switch gate applies to.
inline constexpr int kGameStateMirrorHubPathToAO = 0;
// The death screen, reached only when a death sequence runs to its end. A Proto Spark save revives in place first, so this separates a death
// that stuck from a cushioned one.
inline constexpr int kGameStateDeath = GAMESTATE_DEATH;

// current_game_state() reports -1 when the API is unavailable, which lands outside the range.
[[nodiscard]] inline constexpr bool is_gameplay_game_state(int gs) noexcept
{
    return gs >= kGameStateFirstWorld && gs <= kGameStateLastWorld;
}
static_assert(is_gameplay_game_state(kGameStateFirstWorld) && is_gameplay_game_state(kGameStateLastWorld));
static_assert(is_gameplay_game_state((kGameStateFirstWorld + kGameStateLastWorld) / 2));
static_assert(!is_gameplay_game_state(kGameStateFirstWorld - 1) && !is_gameplay_game_state(kGameStateLastWorld + 1));
static_assert(!is_gameplay_game_state(kGameStateTitleScreen) && !is_gameplay_game_state(kGameStateProfileSelect) && !is_gameplay_game_state(-1));

} // namespace mth
