#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "mocks/fake_mod_api.hpp"
#include "mod/mod_api.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/core/game/game_state_ids.hpp"
#include "mth/features/death_hooks.hpp"

// Covers the deathlink state machine given a correct offset, not the offset. FakePlayer writes the guard byte at kPlayerDeathGuardOff, the constant
// is_dying() reads, so drift on a game rebuild leaves these cases green; confirm that offset by playing.

namespace
{
// A Player buffer big enough for the death-guard byte; get_player() hands out its base.
struct FakePlayer
{
    std::vector<unsigned char> bytes;
    FakePlayer() : bytes(mth::layout::kPlayerDeathGuardOff + 16, 0)
    {
    }
    void set_dying(bool d)
    {
        bytes[static_cast<std::size_t>(mth::layout::kPlayerDeathGuardOff)] = d ? 1 : 0;
    }
    void *base()
    {
        return bytes.data();
    }

    void set_state(int s)
    {
        std::memcpy(bytes.data() + mth::layout::kPlayerStateOff, &s, sizeof(s));
    }
};

// Enough frozen polls to show a deferral is not a one-tick effect; nothing ages while frozen, so the count is arbitrary.
constexpr int kFrozenPolls = 4;

// Stably alive with no sparks: the state an inbound death may be applied from.
void settle(mth::DeathHooks &hooks, FakePlayer &player)
{
    mth::test::recorder().health = 1.0f;
    mth::test::recorder().spark = 0;
    player.set_dying(false);
    for (int i = 0; i < mth::DeathBroadcastGate::kStableAliveTicks; ++i)
        hooks.poll();
}
} // namespace

// Player::DropDeathSpark zeroes the live spark before the poll sees the death edge, so DeathHooks must snapshot spark while alive and gate on that.
TEST_CASE("deathlink: a death with sparks banked while alive is not broadcast", "[deathlink][sparkless]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    int broadcasts = 0;
    mth::DeathHooks hooks([&](const std::string &) { ++broadcasts; }, [&] { return player.base(); });

    // Alive with 3 sparks banked: DeathHooks snapshots the pre-death spark here.
    mth::test::recorder().health = 1.0f;
    mth::test::recorder().spark = 3;
    player.set_dying(false);
    hooks.poll();
    REQUIRE(broadcasts == 0);

    // Death: the game has already dropped every spark (live spark now 0) and health is 0.
    mth::test::recorder().health = 0.0f;
    mth::test::recorder().spark = 0;
    player.set_dying(true);
    hooks.poll();

    REQUIRE(broadcasts == 0);

    mod::set_api(nullptr);
}

TEST_CASE("deathlink: an inbound death during an area enter waits for it to finish", "[deathlink]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    mth::DeathHooks hooks([](const std::string &) {}, [&] { return player.base(); });

    player.set_state(7);
    settle(hooks, player);
    hooks.kill();
    REQUIRE(mth::test::recorder().deaths == 0);

    player.set_state(0);
    hooks.poll();
    REQUIRE(mth::test::recorder().deaths == 1);

    mod::set_api(nullptr);
}

TEST_CASE("deathlink: a long area change does not expore a latched inbound death", "[deathlink]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    mth::DeathHooks hooks([](const std::string &) {}, [&] { return player.base(); });

    player.set_state(7);
    settle(hooks, player);
    hooks.kill();
    for (int i = 0; i < mth::DeathHooks::kPendingInboundDeathTicks + 1; ++i)
        hooks.poll();
    REQUIRE(mth::test::recorder().deaths == 0);

    player.set_state(0);
    hooks.poll();
    REQUIRE(mth::test::recorder().deaths == 1);

    mod::set_api(nullptr);
}

TEST_CASE("deathlink: a genuine sparkless death is broadcast", "[deathlink][sparkless]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    int broadcasts = 0;
    mth::DeathHooks hooks([&](const std::string &) { ++broadcasts; }, [&] { return player.base(); });

    // Alive but already at 0 sparks.
    mth::test::recorder().health = 1.0f;
    mth::test::recorder().spark = 0;
    player.set_dying(false);
    hooks.poll();
    REQUIRE(broadcasts == 0);

    // The death edge alone is not enough (a Proto Spark save produces it too), so the broadcast waits for the death screen.
    mth::test::recorder().health = 0.0f;
    mth::test::recorder().spark = 0;
    player.set_dying(true);
    hooks.poll();
    REQUIRE(broadcasts == 0);

    mth::test::recorder().game_state = mth::kGameStateDeath;
    hooks.poll();

    REQUIRE(broadcasts == 1);

    mod::set_api(nullptr);
}

// A Proto Spark save runs the whole death sequence and sets the death-guard byte, then revives the player in place without reaching the death screen.
TEST_CASE("deathlink: a death the player is revived from is not broadcast", "[deathlink][sparkless]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    int broadcasts = 0;
    mth::DeathHooks hooks([&](const std::string &) { ++broadcasts; }, [&] { return player.base(); });

    // Alive in a room, no sparks banked: a death here would otherwise broadcast.
    mth::test::recorder().game_state = mth::kGameStateFirstWorld;
    mth::test::recorder().health = 1.0f;
    mth::test::recorder().spark = 0;
    player.set_dying(false);
    hooks.poll();

    // The death sequence runs, and the game never leaves the room for the death screen.
    mth::test::recorder().health = 0.0f;
    player.set_dying(true);
    for (int i = 0; i < mth::DeathHooks::kDeathConfirmTicks; ++i)
        hooks.poll();
    REQUIRE(broadcasts == 0);

    // Revived in place, and still nothing may fire once the held death has lapsed.
    mth::test::recorder().health = 1.0f;
    player.set_dying(false);
    for (int i = 0; i < mth::DeathBroadcastGate::kStableAliveTicks * 2; ++i)
        hooks.poll();

    REQUIRE(broadcasts == 0);

    mod::set_api(nullptr);
}

TEST_CASE("deathlink: the broadcast carries a detail for the outbound cause", "[deathlink][sparkless]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    std::string detail;
    mth::DeathHooks hooks([&](const std::string &d) { detail = d; }, [&] { return player.base(); });

    mth::test::recorder().health = 1.0f;
    mth::test::recorder().spark = 0;
    player.set_dying(false);
    hooks.poll();

    mth::test::recorder().health = 0.0f;
    player.set_dying(true);
    hooks.poll();
    mth::test::recorder().game_state = mth::kGameStateDeath;
    hooks.poll();

    // ApLink prefixes the slot name, so the detail is the predicate alone, never empty.
    REQUIRE_FALSE(detail.empty());

    mod::set_api(nullptr);
}

// Echo storm (#125): after an inbound PlayerDie, health and the guard byte read alive for ~0.7s. Those polls must not count as a settled respawn,
// or they lift the suppression armed for this death and it is broadcast back.
TEST_CASE("deathlink: a death taken from an inbound deathlink is never echoed back", "[deathlink][echo]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    int broadcasts = 0;
    mth::DeathHooks hooks([&](const std::string &) { ++broadcasts; }, [&] { return player.base(); });

    // Settle: stably alive, no sparks (so any death of ours would otherwise broadcast).
    mth::test::recorder().health = 1.0f;
    mth::test::recorder().spark = 0;
    player.set_dying(false);
    for (int i = 0; i < mth::DeathBroadcastGate::kStableAliveTicks; ++i)
        hooks.poll();

    hooks.kill(); // inbound deathlink -> applied from a settled state
    REQUIRE(mth::test::recorder().deaths == 1);

    // ~0.7s of alive reads is twice as many polls at 120 Hz FixedUpdate as at 60 Hz; drive the higher tick count.
    for (int i = 0; i < mth::ticks_for_seconds(0.7); ++i)
        hooks.poll();

    // Now the death actually registers.
    mth::test::recorder().health = 0.0f;
    player.set_dying(true);
    hooks.poll();

    REQUIRE(broadcasts == 0);

    mod::set_api(nullptr);
}

TEST_CASE("deathlink: respawn re-arms; a mid-death guard-byte pulse does not overwrite the held spark", "[deathlink][sparkless]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    int broadcasts = 0;
    mth::DeathHooks hooks([&](const std::string &) { ++broadcasts; }, [&] { return player.base(); });

    // Alive with sparks, snapshot taken.
    mth::test::recorder().health = 1.0f;
    mth::test::recorder().spark = 2;
    player.set_dying(false);
    hooks.poll();

    // Death edge: broadcast suppressed (had sparks).
    mth::test::recorder().health = 0.0f;
    mth::test::recorder().spark = 0;
    player.set_dying(true);
    hooks.poll();
    REQUIRE(broadcasts == 0);

    // The guard byte pulses to 0 mid-death while still dead. That must not be sampled as an alive snapshot or re-arm the gate.
    player.set_dying(false);
    hooks.poll();
    REQUIRE(broadcasts == 0);

    mod::set_api(nullptr);
}

// Two bounces 142ms apart in the field both issued PlayerDie into one death sequence, because no advancing poll ran between them. PlayerDie
// reaches Player::DeathEvent through the vtable and skips the damage pipeline, so the second call re-runs the teardown (suspected cause of the
// camera losing the player).
TEST_CASE("deathlink: a second inbound death arriving during the first is deferred", "[deathlink][echo]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    mth::DeathHooks hooks([](const std::string &) {}, [&] { return player.base(); });
    settle(hooks, player);

    hooks.kill(); // first bounce -> applied from a settled state
    REQUIRE(mth::test::recorder().deaths == 1);

    // Only frozen polls follow. The player reads alive throughout and the freeze is a paused world, so the in-flight death alone defers the second bounce.
    mth::test::recorder().paused = true;
    for (int i = 0; i < kFrozenPolls; ++i)
        hooks.poll();

    hooks.kill();
    REQUIRE(mth::test::recorder().deaths == 1);

    mod::set_api(nullptr);
}

// A paused world holds a queued death until the menu closes; a stalled room clock with the world unpaused is a screen transition, where PlayerDie
// lands unpredictably.
TEST_CASE("deathlink: an inbound death during a screen transition is deferred", "[deathlink][echo]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    mth::DeathHooks hooks([](const std::string &) {}, [&] { return player.base(); });
    settle(hooks, player);

    // The room clock stalls while WorldIsPaused stays false: a transition, not a menu.
    mth::test::recorder().game_paused = true;
    hooks.poll();

    hooks.kill();
    REQUIRE(mth::test::recorder().deaths == 0);

    mod::set_api(nullptr);
}

TEST_CASE("deathlink: an inbound death during a menu is still applied", "[deathlink][echo]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    mth::DeathHooks hooks([](const std::string &) {}, [&] { return player.base(); });
    settle(hooks, player);

    // A paused world queues the death until the menu closes, so it keeps applying.
    mth::test::recorder().paused = true;
    hooks.poll();

    hooks.kill();
    REQUIRE(mth::test::recorder().deaths == 1);

    mod::set_api(nullptr);
}

// The world pause flag covers only menus that pause the world. A game-level pause skips the whole update queue, which the flag cannot see, but it
// stalls the room clock.
TEST_CASE("deathlink: a death queued behind a game-level pause is not echoed", "[deathlink][echo]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    int broadcasts = 0;
    mth::DeathHooks hooks([&](const std::string &) { ++broadcasts; }, [&] { return player.base(); });

    mth::test::recorder().health = 1.0f;
    mth::test::recorder().spark = 0;
    player.set_dying(false);
    for (int i = 0; i < mth::DeathBroadcastGate::kStableAliveTicks; ++i)
        hooks.poll();

    hooks.kill();
    REQUIRE(mth::test::recorder().deaths == 1);

    // Longer than both gate timers together, so a paused tick that aged either would have lifted the suppression by the time the death lands.
    mth::test::recorder().game_paused = true; // world never learns it is paused; the room clock stops
    for (int i = 0; i < mth::DeathBroadcastGate::kInboundDeathGraceTicks + mth::DeathBroadcastGate::kStableAliveTicks; ++i)
        hooks.poll();
    mth::test::recorder().game_paused = false;

    mth::test::recorder().health = 0.0f;
    player.set_dying(true);
    hooks.poll();

    REQUIRE(broadcasts == 0);

    mod::set_api(nullptr);
}

// A bounce that arrives while we are already dying is served by that death; a second death after the respawn would read as a bug.
TEST_CASE("deathlink: a bounce arriving during our own death does not kill us again", "[deathlink][retry]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    int broadcasts = 0;
    mth::DeathHooks hooks([&](const std::string &) { ++broadcasts; }, [&] { return player.base(); });

    // Settle, then die locally: the gate is no longer stably alive.
    mth::test::recorder().health = 1.0f;
    mth::test::recorder().spark = 0;
    player.set_dying(false);
    for (int i = 0; i < mth::DeathBroadcastGate::kStableAliveTicks; ++i)
        hooks.poll();
    mth::test::recorder().health = 0.0f;
    player.set_dying(true);
    hooks.poll();
    mth::test::recorder().game_state = mth::kGameStateDeath;
    hooks.poll();
    REQUIRE(broadcasts == 1);

    hooks.kill(); // the bounce lands while we are dying
    REQUIRE(mth::test::recorder().deaths == 0);

    // Respawn and play well past a full grace + settle: nothing may be waiting to fire.
    mth::test::recorder().health = 1.0f;
    player.set_dying(false);
    for (int i = 0; i < (mth::DeathBroadcastGate::kInboundDeathGraceTicks + mth::DeathBroadcastGate::kStableAliveTicks) * 2; ++i)
        hooks.poll();

    REQUIRE(mth::test::recorder().deaths == 0);

    mod::set_api(nullptr);
}

// #164: a bounce during a world/screen transition, when no Player is captured.
TEST_CASE("deathlink: an inbound death arriving during a transition is applied when the player returns", "[deathlink][retry]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    void *live = nullptr; // mid-transition: no player captured
    int broadcasts = 0;
    mth::DeathHooks hooks([&](const std::string &) { ++broadcasts; }, [&] { return live; });

    hooks.kill();
    REQUIRE(mth::test::recorder().deaths == 0);

    mth::test::recorder().health = 1.0f;
    mth::test::recorder().spark = 0;
    player.set_dying(false);
    live = player.base(); // the new room's player exists
    for (int i = 0; i < mth::DeathBroadcastGate::kInboundDeathGraceTicks + mth::DeathBroadcastGate::kStableAliveTicks + 2; ++i)
        hooks.poll();

    REQUIRE(mth::test::recorder().deaths == 1);

    mod::set_api(nullptr);
}

// The latch is bounded: a bounce that never finds a settled player is dropped, not held to kill the player much later.
TEST_CASE("deathlink: a latched inbound death expires if the player never settles", "[deathlink][retry]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    void *live = nullptr;
    int broadcasts = 0;
    mth::DeathHooks hooks([&](const std::string &) { ++broadcasts; }, [&] { return live; });

    hooks.kill();
    for (int i = 0; i < mth::DeathHooks::kPendingInboundDeathTicks + 1; ++i)
        hooks.poll(); // still no player: the window burns down

    mth::test::recorder().health = 1.0f;
    mth::test::recorder().spark = 0;
    player.set_dying(false);
    live = player.base();
    for (int i = 0; i < mth::DeathBroadcastGate::kInboundDeathGraceTicks + mth::DeathBroadcastGate::kStableAliveTicks + 2; ++i)
        hooks.poll();

    REQUIRE(mth::test::recorder().deaths == 0);

    mod::set_api(nullptr);
}

// One pending death however many bounces arrive; a multiworld storm must not chain-kill through several respawns.
TEST_CASE("deathlink: several inbound deaths arriving before the player settles apply once", "[deathlink][retry]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    void *live = nullptr;
    int broadcasts = 0;
    mth::DeathHooks hooks([&](const std::string &) { ++broadcasts; }, [&] { return live; });

    // Three bounces spread across the unsettled window, not stacked on one tick (a counter would queue all three).
    hooks.kill();
    for (int i = 0; i < mth::DeathBroadcastGate::kStableAliveTicks; ++i)
        hooks.poll();
    hooks.kill();
    for (int i = 0; i < mth::DeathBroadcastGate::kStableAliveTicks; ++i)
        hooks.poll();
    hooks.kill();

    mth::test::recorder().health = 1.0f;
    mth::test::recorder().spark = 0;
    player.set_dying(false);
    live = player.base();
    // Long enough that a second queued death would have its own full grace + settle to land in.
    for (int i = 0; i < (mth::DeathBroadcastGate::kInboundDeathGraceTicks + mth::DeathBroadcastGate::kStableAliveTicks) * 3; ++i)
        hooks.poll();

    REQUIRE(mth::test::recorder().deaths == 1);

    mod::set_api(nullptr);
}

// The retry window counts gameplay ticks: a menu holds the game still and must not burn the window.
TEST_CASE("deathlink: a menu does not burn the pending-death retry window", "[deathlink][retry]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    void *live = nullptr;
    int broadcasts = 0;
    mth::DeathHooks hooks([&](const std::string &) { ++broadcasts; }, [&] { return live; });

    hooks.kill();

    mth::test::recorder().paused = true; // menu open far longer than the retry window
    for (int i = 0; i < mth::DeathHooks::kPendingInboundDeathTicks * 2; ++i)
        hooks.poll();
    mth::test::recorder().paused = false;

    mth::test::recorder().health = 1.0f;
    mth::test::recorder().spark = 0;
    player.set_dying(false);
    live = player.base();
    for (int i = 0; i < mth::DeathBroadcastGate::kInboundDeathGraceTicks + mth::DeathBroadcastGate::kStableAliveTicks + 2; ++i)
        hooks.poll();

    REQUIRE(mth::test::recorder().deaths == 1);

    mod::set_api(nullptr);
}

// The inbound grace covers a PlayerDie already in flight. Arming it at receive time would hold the alive streak at zero for its whole length and
// delay a latched death even though the player settled immediately; it must land off the ordinary settle debounce.
TEST_CASE("deathlink: a latched death lands as soon as the player settles, not a grace later", "[deathlink][retry]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    void *live = nullptr;
    int broadcasts = 0;
    mth::DeathHooks hooks([&](const std::string &) { ++broadcasts; }, [&] { return live; });

    hooks.kill(); // no player yet: latched

    mth::test::recorder().health = 1.0f;
    mth::test::recorder().spark = 0;
    player.set_dying(false);
    live = player.base();
    for (int i = 0; i < mth::DeathBroadcastGate::kStableAliveTicks + 3; ++i)
        hooks.poll();

    REQUIRE(mth::test::recorder().deaths == 1);

    mod::set_api(nullptr);
}

// A busy multiworld: bounces keep arriving while one is latched. An arrival must not restart the settling clock, or the latched death never lands
// and the retry window never expires.
TEST_CASE("deathlink: a steady stream of bounces still lands a death", "[deathlink][retry]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    void *live = nullptr;
    int broadcasts = 0;
    mth::DeathHooks hooks([&](const std::string &) { ++broadcasts; }, [&] { return live; });

    hooks.kill(); // first bounce arrives mid-transition

    mth::test::recorder().health = 1.0f;
    mth::test::recorder().spark = 0;
    player.set_dying(false);
    live = player.base(); // player available and alive from here on

    for (int i = 0; i < mth::ticks_for_seconds(10.0) && mth::test::recorder().deaths == 0; ++i)
    {
        if (i % mth::ticks_for_seconds(1.0) == 0)
            hooks.kill(); // another player dies every second
        hooks.poll();
    }

    REQUIRE(mth::test::recorder().deaths >= 1);

    mod::set_api(nullptr);
}

// The window runs from the first deferral; re-arming it on each bounce would push the deadline out indefinitely.
TEST_CASE("deathlink: a stream of bounces does not extend the retry window", "[deathlink][retry]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    void *live = nullptr;
    int broadcasts = 0;
    mth::DeathHooks hooks([&](const std::string &) { ++broadcasts; }, [&] { return live; });

    const int window = mth::DeathHooks::kPendingInboundDeathTicks;
    hooks.kill();
    for (int i = 0; i < window; ++i)
    {
        if (i < window / 2 && i % mth::ticks_for_seconds(0.5) == 0)
            hooks.kill(); // more bounces, but only through the first half of the window
        hooks.poll();     // no player the whole time
    }

    mth::test::recorder().health = 1.0f;
    mth::test::recorder().spark = 0;
    player.set_dying(false);
    live = player.base();
    for (int i = 0; i < mth::DeathBroadcastGate::kInboundDeathGraceTicks + mth::DeathBroadcastGate::kStableAliveTicks + 2; ++i)
        hooks.poll();

    REQUIRE(mth::test::recorder().deaths == 0);

    mod::set_api(nullptr);
}

// A bounce that applies immediately must consume the one already latched, or the latched one fires again after the respawn.
TEST_CASE("deathlink: a bounce that applies at once clears an already-latched death", "[deathlink][retry]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    int broadcasts = 0;
    void *live = nullptr;
    mth::DeathHooks hooks([&](const std::string &) { ++broadcasts; }, [&] { return live; });

    mth::test::recorder().health = 1.0f;
    mth::test::recorder().spark = 0;
    player.set_dying(false);
    live = player.base();
    for (int i = 0; i < mth::DeathBroadcastGate::kStableAliveTicks; ++i)
        hooks.poll();

    live = nullptr; // one tick of transition
    hooks.kill();   // bounce A: latched
    REQUIRE(mth::test::recorder().deaths == 0);

    live = player.base();
    hooks.kill(); // bounce B: still settled, so it applies right away
    REQUIRE(mth::test::recorder().deaths == 1);

    for (int i = 0; i < (mth::DeathBroadcastGate::kInboundDeathGraceTicks + mth::DeathBroadcastGate::kStableAliveTicks) * 2; ++i)
        hooks.poll();

    REQUIRE(mth::test::recorder().deaths == 1);

    mod::set_api(nullptr);
}

// The apply path unsettles us at once, so a rapid second bounce is latched, and the death underway serves it: the latch must be cancelled.
// kill() checks death_in_progress(), but the guard byte takes ~0.7s to appear, so the bounce is latched while it is still false.
TEST_CASE("deathlink: a death in progress cancels a latched bounce instead of killing again", "[deathlink][retry]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    mth::DeathHooks hooks([](const std::string &) {}, [&] { return player.base(); });
    settle(hooks, player);

    hooks.kill(); // first bounce -> applied from a settled state, which unsettles us at once
    REQUIRE(mth::test::recorder().deaths == 1);

    // A paused world: frozen ticks age nothing and we are no longer settled, so the second bounce is latched for retry.
    mth::test::recorder().paused = true;
    hooks.poll();
    hooks.kill();
    REQUIRE(mth::test::recorder().deaths == 1);

    // The first PlayerDie now registers: from here the latched bounce is served by the death in progress.
    mth::test::recorder().paused = false;
    mth::test::recorder().health = 0.0f;
    player.set_dying(true);
    hooks.poll();

    // Respawn and play well past a full grace + settle. The latch must have been cancelled, not delayed.
    mth::test::recorder().health = 1.0f;
    player.set_dying(false);
    for (int i = 0; i < (mth::DeathBroadcastGate::kInboundDeathGraceTicks + mth::DeathBroadcastGate::kStableAliveTicks) * 2; ++i)
        hooks.poll();

    REQUIRE(mth::test::recorder().deaths == 1);

    mod::set_api(nullptr);
}

// The ending leaves a live Player in a World with no area bound. Player::InitDeath dereferences player->entity->world->area without a null check,
// so a PlayerDie there faults when the state machine flushes the death (crash 2026-08-23, 59th inbound deathlink, during the credits).
TEST_CASE("deathlink: an inbound death is not applied while the world has no area bound", "[deathlink][ending]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    mth::DeathHooks hooks([&](const std::string &) {}, [&] { return player.base(); });

    settle(hooks, player);
    mth::test::recorder().world_has_area = false; // a populated World, no area: the credits

    hooks.kill();

    REQUIRE(mth::test::recorder().deaths == 0);

    mod::set_api(nullptr);
}

// No area bound is terminal: a bounce held until an area exists again would land in the post-credits handoff. It is consumed on arrival.
TEST_CASE("deathlink: a bounce received with no area bound is dropped, not retried later", "[deathlink][ending]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    mth::DeathHooks hooks([&](const std::string &) {}, [&] { return player.base(); });

    settle(hooks, player);
    mth::test::recorder().world_has_area = false;
    hooks.kill();
    REQUIRE(mth::test::recorder().deaths == 0);

    // An area is bound again and play continues well past the retry window: nothing may still be waiting.
    mth::test::recorder().world_has_area = true;
    for (int i = 0; i < mth::DeathHooks::kPendingInboundDeathTicks + mth::DeathBroadcastGate::kStableAliveTicks + 2; ++i)
        hooks.poll();

    REQUIRE(mth::test::recorder().deaths == 0);

    mod::set_api(nullptr);
}

// A build whose API cannot supply a World reports no world, which must not cost every deathlink. "No accessor" and "no world right now" are
// indistinguishable here; this guards against an absent world refusing the kill.
TEST_CASE("deathlink: the area check fails open when the build has no world accessor", "[deathlink][ending]")
{
    mth::test::ScopedFakeApi scoped;
    scoped.api.PlayerGetWorld = nullptr; // older game build: the entry is absent

    FakePlayer player;
    mth::DeathHooks hooks([&](const std::string &) {}, [&] { return player.base(); });

    settle(hooks, player);
    hooks.kill();

    REQUIRE(mth::test::recorder().deaths == 1);

    mod::set_api(nullptr);
}

// Between area teardown and AreaManagerNewArea there is an area-less window that clears by itself. The terminal area check must come last, or every
// room change becomes a permanent silent drop.
TEST_CASE("deathlink: an area-less bounce mid-transition is latched and lands, not dropped", "[deathlink][ending]")
{
    mth::test::ScopedFakeApi scoped;

    FakePlayer player;
    mth::DeathHooks hooks([&](const std::string &) {}, [&] { return player.base(); });

    settle(hooks, player);

    // A room change: the area is torn down and the room clock stalls with the world unpaused.
    mth::test::recorder().game_paused = true;
    mth::test::recorder().world_has_area = false;
    hooks.poll();
    hooks.kill();
    REQUIRE(mth::test::recorder().deaths == 0);

    // The next room binds its area and the clock runs again; the latched bounce lands.
    mth::test::recorder().game_paused = false;
    mth::test::recorder().world_has_area = true;
    for (int i = 0; i < mth::DeathBroadcastGate::kStableAliveTicks + 2; ++i)
        hooks.poll();

    REQUIRE(mth::test::recorder().deaths == 1);

    mod::set_api(nullptr);
}
