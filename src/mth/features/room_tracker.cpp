#include "mth/features/room_tracker.hpp"

#include <cstdint>

#include "mod/mod_api.hpp"
#include "mth/core/game/game_state_ids.hpp"
#include "pal/pal_log.hpp"

namespace
{

std::uint32_t g_screen = 0;
bool g_have_screen = false;

} // namespace

namespace mth::room_tracker
{

void poll()
{
    // Menu worlds tick World::Update too and GetRoomIndex keeps returning the last room, so the gamestate tells a room from a menu. AreaReporter relies on an
    // absent screen outside rooms: it early-returns without clearing what it last sent.
    const int gs = mod::current_game_state();
    if (!is_gameplay_game_state(gs))
        return;

    // RoomManager::BeginRoomTransition zeroes the room timer, then recomputes the index from the camera's bound list and stores 0 on no match (0 is also a real
    // room). Every transition state keeps the timer frozen, so a positive timer means the room is settled. Fails open without the entry. Known hole:
    // Player::UpdateResurrectur rewrites the index without zeroing the timer.
    if (!settled())
        return;

    const int room = mod::room_index();
    if (room < 0)
        return;

    const std::uint32_t screen = (static_cast<std::uint32_t>(gs) << 16) | (static_cast<std::uint32_t>(room) & 0xFFFFu);
    if (g_have_screen && screen == g_screen)
        return;
    g_screen = screen;
    g_have_screen = true;
    pal::logf(pal::LogLevel::Info, "area: gamestate %d room %d -> screen 0x%x", gs, room, screen);
}

bool settled()
{
    return !mod::room_time_api_available() || mod::room_time() > 0.0f;
}

bool current_screen(std::uint32_t *out)
{
    if (!g_have_screen)
        return false;
    *out = g_screen;
    return true;
}

} // namespace mth::room_tracker
