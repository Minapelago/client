#include "mth/features/player_tracker.hpp"

#include <cmath>

#include "mod/mod_api.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"

namespace
{

void *g_player = nullptr;
float g_last_pos[3]{};
bool g_have_pos = false;

} // namespace

namespace mth::player_tracker
{

void sample_position()
{
    float p[3];
    if (mod::player_position(p) && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]))
    {
        g_last_pos[0] = p[0];
        g_last_pos[1] = p[1];
        g_last_pos[2] = p[2];
        g_have_pos = true;
    }
}

void *player()
{
    // Prefers the game's live-Player pointer, which Player::~Player nulls. The note_player capture is cleared only by World::Destroy, and a Player can be freed
    // before or without one, leaving a canonical-looking pointer to recycled heap (faulted on the Coltrane rest ride, #157). Null means no live player this
    // tick; callers retry.
    void *p = mod::player_component_available() ? mod::player_component() : g_player;

    // A non-null, non-canonical value means a hook or the API resolved wrong: return null (every caller null-checks) and warn once.
    if (p != nullptr && !pal::pointer_looks_valid(p))
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            pal::logf(pal::LogLevel::Warn, "player pointer looks invalid (%p); write paths (upgrades/deathlink/abilities) disabled this session", p);
        }
        return nullptr;
    }
    return p;
}

bool position(float out[3])
{
    if (!g_have_pos)
        return false;
    out[0] = g_last_pos[0];
    out[1] = g_last_pos[1];
    out[2] = g_last_pos[2];
    return true;
}

void note_player(void *player)
{
    if (player != nullptr)
        g_player = player;
}

void invalidate_player()
{
    // World teardown frees the Player, and the next tick would write through a cached pointer (apply_upgrades, Player::UpdateStats). Position is per-player and
    // recaptured on the next sample, so it goes too.
    g_player = nullptr;
    g_have_pos = false;
}

} // namespace mth::player_tracker
