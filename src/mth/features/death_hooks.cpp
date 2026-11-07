#include "mth/features/death_hooks.hpp"

#include <utility>

#include "mod/mod_api.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/core/game/game_state_ids.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"

namespace
{

// Player+0x1380: the once-per-death guard byte set when a death begins (0 = alive), the signal the deathlink edge keys on.
[[nodiscard]] bool is_dying(void *player)
{
    return player != nullptr && *reinterpret_cast<unsigned char *>(static_cast<char *>(player) + mth::layout::kPlayerDeathGuardOff) != 0;
}

// ycWorld+0x1758 is the world's AreaManager. Player::InitDeath dereferences player->entity->world->area without the null check other engine sites apply, so a
// death requested in an area-less world faults.
[[nodiscard]] bool world_area_missing()
{
    // Null means no live world or an API that cannot supply one. Neither refuses a deathlink, and the caller's null-player check covers transitions.
    void *world = mod::player_world();
    if (world == nullptr)
        return false;
    if (!pal::pointer_looks_valid(world))
        return true; // unverifiable world: do not request a death in it
    return *reinterpret_cast<void **>(static_cast<char *>(world) + mth::layout::kWorldAreaManagerOff) == nullptr;
}

[[nodiscard]] bool in_area_change(void *player)
{
    const int s = *reinterpret_cast<int *>(static_cast<char *>(player) + mth::layout::kPlayerStateOff);
    return s == -1 || (s >= 1 && s <= 0x18) || s == 0x4a || s == 0x4b || s == 0x74;
}

} // namespace

namespace mth
{

DeathHooks::DeathHooks(std::function<void(const std::string &)> on_local_death, std::function<void *()> get_player)
    : on_local_death_(std::move(on_local_death)), get_player_(std::move(get_player))
{
}

DeathHooks::~DeathHooks() = default;

// PlayerDie only queues damage for the gameplay queues, so a death cannot land on a tick where they did not run (see RoomClock). A paused world holds the death
// until the menu closes, but a stalled room clock (transition or death sequence) lands it at an unpredictable point, so the kind of freeze is recorded.
bool DeathHooks::gameplay_advanced()
{
    const RoomClock::Tick t = room_clock_.sample();
    room_clock_stalled_ = t == RoomClock::Tick::Stalled;
    return t == RoomClock::Tick::Advanced;
}

void DeathHooks::poll()
{
    const bool advanced = gameplay_advanced(); // sampled every tick so the room clock reading stays fresh
    drive_pending_death(advanced);
    drive_pending_broadcast(); // before the null-player return: the death screen has no player
    void *p = get_player_ ? get_player_() : nullptr;
    if (p == nullptr)
    {
        // No player (transition): reset the settled-respawn debounce so an inbound PlayerDie waits after the player reappears.
        gate_.observe(false, false, advanced);
        return;
    }
    const bool dying = is_dying(p);
    // The guard byte pulses through the death sequence, so health > 0 is the stable alive signal for re-arming. Falls back to the guard byte without the health
    // API.
    const bool alive = mod::health_api_available() ? (mod::player_health() > 0.0f) : !dying;

    // Player::DropDeathSpark zeroes the live spark mid-death, before this poll sees the edge, so sample it while alive. Keyed on `alive` because the guard byte
    // pulses mid-death at 0 health and must not resample. No spark API holds 0.
    if (alive)
        last_alive_spark_ = mod::spark_api_available() ? mod::player_spark() : 0;

    const bool fresh_local_death = gate_.observe(dying, alive, advanced);
    if (gate_.take_suppressed_death())
        pal::logf(pal::LogLevel::Info, "deathlink: local death suppressed (echo of a received deathlink)");
    if (fresh_local_death && on_local_death_)
    {
        // Only broadcast a sparkless demise (0 sparks at death, per the pre-death snapshot).
        if (last_alive_spark_ > 0)
        {
            pal::logf(pal::LogLevel::Info, "deathlink: local death suppressed (had %d spark(s); not sparkless)", last_alive_spark_);
            return;
        }
        pending_broadcast_ticks_ = kDeathConfirmTicks;
    }
}

// Holds a sparkless death until the death screen confirms it. A Proto Spark save runs the same death sequence and only revives in place at the tail, so nothing
// readable at the death edge separates the two without a raw Player offset.
void DeathHooks::drive_pending_broadcast()
{
    if (pending_broadcast_ticks_ <= 0)
        return;
    const int gs = mod::current_game_state();
    if (gs == kGameStateDeath || gs < 0) // no gamestate to read means no confirmation is ever coming: send it
    {
        pending_broadcast_ticks_ = 0;
        pal::logf(pal::LogLevel::Info, "deathlink: sparkless local death confirmed -> broadcasting");
        // One detail for every death until the death path can distinguish them.
        on_local_death_("had a skill issue");
        return;
    }
    if (--pending_broadcast_ticks_ == 0)
        pal::logf(pal::LogLevel::Info, "deathlink: local death not broadcast (revived without reaching the death screen)");
}

// false only for a block that clears on its own (no player yet, or not settled), so the caller retries. A missing PlayerDie API or an area-less world will not
// clear, so both count as handled.
bool DeathHooks::try_apply_inbound_death()
{
    void *p = get_player_ ? get_player_() : nullptr;
    if (p == nullptr)
        return false; // world/screen transition: the next room's player is moments away
    // Apply PlayerDie only from a settled state: during the Underlab->overworld transition it softlocks, while dying it no-ops, and with a stalled room clock
    // it lands at an unpredictable point. A paused world (menu) holds the death safely (#125).
    if (!gate_.stably_alive() || room_clock_stalled_ || in_area_change(p))
        return false;

    // The ending sequence leaves a settled player in a World with no area, and PlayerDie there faults a tick later in Player::InitDeath. Checked after the
    // guards above so the transition window (area teardown to AreaManagerNewArea) still retries. Dropped because the run is over by the credits and a held
    // bounce would land in the post-credits handoff. An in-run pause keeps the area, so a menu deathlink still queues (#125).
    if (world_area_missing())
    {
        pal::logf(pal::LogLevel::Warn, "deathlink: inbound death dropped (world has no area bound; gamestate %d)", mod::current_game_state());
        return true;
    }
    if (!mod::player_die())
    {
        pal::logf(pal::LogLevel::Warn, "deathlink: inbound death not applied (PlayerDie API unavailable)");
        return true;
    }
    // Arm the grace when the death goes out: the alive polls that follow belong to this request and must not settle us before it lands.
    gate_.note_inbound_death_applied();
    pal::logf(pal::LogLevel::Info, "deathlink: applying inbound death (PlayerDie)");
    return true;
}

// Retries a latched inbound death and ages its window on gameplay ticks only, so a menu pause does not spend it (#164).
void DeathHooks::drive_pending_death(bool advanced)
{
    if (pending_kill_ticks_ <= 0)
        return;
    // A death registered after the latch serves it. kill() checks this too, but the guard byte takes ~0.7s to appear; without this the latch outlives the
    // respawn and kills twice for one exchange.
    if (gate_.death_in_progress())
    {
        pal::logf(pal::LogLevel::Info, "deathlink: latched inbound death served by the death already in progress");
        pending_kill_ticks_ = 0;
        return;
    }
    if (try_apply_inbound_death())
    {
        pending_kill_ticks_ = 0;
        return;
    }
    void *p = get_player_ ? get_player_() : nullptr;
    if (advanced && !(p && in_area_change(p)) && --pending_kill_ticks_ == 0)
        pal::logf(pal::LogLevel::Warn, "deathlink: inbound death dropped (player in a cutscene or other area-change state)");
}

void DeathHooks::kill()
{
    // Suppress our outbound as soon as a bounce arrives, even if applying it is deferred; this breaks the multiworld echo storm (#125).
    gate_.note_inbound_death_received();
    if (gate_.death_in_progress())
    {
        pal::logf(pal::LogLevel::Info, "deathlink: inbound death served by the death already in progress");
        return;
    }
    if (try_apply_inbound_death())
    {
        pending_kill_ticks_ = 0; // a queued-but-unlanded death is the game's to finish
        return;
    }
    // One latch however many bounces arrive, so a storm cannot chain-kill through respawns. The window runs from the first deferral, so a steady stream cannot
    // extend it (#164).
    if (pending_kill_ticks_ > 0)
    {
        pal::logf(pal::LogLevel::Debug, "deathlink: inbound death arrived while one is already pending (coalesced)");
        return;
    }
    const bool no_player = (get_player_ ? get_player_() : nullptr) == nullptr;
    pal::logf(pal::LogLevel::Info, "deathlink: inbound death deferred (%s), retrying",
              no_player             ? "no player captured"
              : room_clock_stalled_ ? "room clock stalled: mid-transition"
                                    : "player not settled");
    pending_kill_ticks_ = kPendingInboundDeathTicks;
}

void DeathHooks::reset()
{
    pending_kill_ticks_ = 0;
    pending_broadcast_ticks_ = 0;
}

} // namespace mth
