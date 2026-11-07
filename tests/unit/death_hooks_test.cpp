#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "mocks/fake_game.hpp"
#include "mth/core/rules/death_broadcast_gate.hpp"
#include "mth/features/death_hooks.hpp"

// The fake writes the guard byte at kPlayerDeathGuardOff, the offset DeathHooks reads, so drift on a game rebuild leaves these green; confirm it in game.

namespace
{
using Log = std::vector<std::string>;

const Log kDeathRun{"died", "death screen", "respawned"};
const Log kBroadcastDeathRun{"died", "death screen", "deathlink sent", "respawned"};

// A player settled in a room with DeathHooks polling every tick; an outbound deathlink lands in the game's log.
struct Deathlink
{
    mth::test::FakeGame game;
    mth::DeathHooks hooks{[this](const std::string &cause) { game.note(cause.empty() ? "deathlink sent without a cause" : "deathlink sent"); },
                          [this] { return game.player(); }};

    Deathlink()
    {
        game.on_tick = [this] { hooks.poll(); };
        game.play_for(1.0);
    }
    void receive_deathlink()
    {
        hooks.kill();
    }
};
} // namespace

TEST_CASE("deathlink: a sparkless death is broadcast once, at the death screen, with a cause", "[deathlink][outbound]")
{
    Deathlink d;
    d.game.die_locally();
    d.game.play_for(1.0);
    REQUIRE(d.game.log == Log{"died"});

    d.game.play_for(10.0);
    REQUIRE(d.game.log == kBroadcastDeathRun);
}

// Player::DropDeathSpark zeroes the live spark before the death registers, so the spark count has to come from while the player was alive.
TEST_CASE("deathlink: a death with sparks banked is not broadcast, and the next sparkless one is", "[deathlink][outbound]")
{
    Deathlink d;
    d.game.sparks = 3;
    d.game.play_for(0.5);
    d.game.die_locally();
    d.game.play_for(10.0);
    REQUIRE(d.game.log == kDeathRun);

    d.game.die_locally();
    d.game.play_for(10.0);
    REQUIRE(d.game.log == Log{"died", "death screen", "respawned", "died", "death screen", "deathlink sent", "respawned"});
}

// A Proto Spark runs the whole death sequence, guard byte included, then revives in place without reaching the death screen.
TEST_CASE("deathlink: a death the player is revived from is not broadcast", "[deathlink][outbound]")
{
    Deathlink d;
    d.game.proto_spark = true;
    d.game.die_locally();
    d.game.play_for(20.0);
    REQUIRE(d.game.log == Log{"died", "revived"});
}

// Echo storm (#125): the player reads alive for a while after PlayerDie, and those ticks must not lift the suppression armed for this death.
TEST_CASE("deathlink: a death taken from an inbound deathlink is not echoed back", "[deathlink][echo]")
{
    Deathlink d;
    d.receive_deathlink();
    d.game.play_for(10.0);
    REQUIRE(d.game.log == kDeathRun);
}

// The world pause flag only covers menus; a game-level pause stops the room clock without it. Neither may age the echo suppression.
TEST_CASE("deathlink: an inbound death held behind a pause lands when play resumes, without an echo", "[deathlink][echo]")
{
    Deathlink d;
    SECTION("a menu")
    {
        d.game.open_menu();
        d.game.play_for(0.5);
        d.receive_deathlink();
        d.game.play_for(30.0);
        REQUIRE(d.game.log.empty());
        d.game.close_menu();
    }
    SECTION("a game-level pause")
    {
        d.receive_deathlink();
        d.game.pause_game();
        d.game.play_for(30.0);
        REQUIRE(d.game.log.empty());
        d.game.resume_game();
    }
    d.game.play_for(10.0);
    REQUIRE(d.game.log == kDeathRun);
}

// Two bounces 142ms apart in the field both reached PlayerDie before the first death registered, and the second re-ran the death teardown.
TEST_CASE("deathlink: a second bounce while the first death is still landing is served by it", "[deathlink][echo]")
{
    Deathlink d;
    d.receive_deathlink();
    d.game.open_menu();
    d.game.play_for(0.1);
    d.receive_deathlink();
    d.game.close_menu();
    d.game.play_for(30.0);
    REQUIRE(d.game.log == kDeathRun);
}

TEST_CASE("deathlink: an inbound death that never lands stops suppressing our own deaths", "[deathlink][echo]")
{
    Deathlink d;
    d.game.invulnerable = true;
    d.receive_deathlink();
    d.game.play_for(10.0);
    REQUIRE(d.game.log.empty());

    d.game.invulnerable = false;
    d.game.die_locally();
    d.game.play_for(10.0);
    REQUIRE(d.game.log == kBroadcastDeathRun);
}

// A single alive poll is transition flicker (#125); only a settled alive stretch re-arms the broadcast.
TEST_CASE("deathlink: one alive poll after a death does not re-arm the broadcast", "[deathlink][echo]")
{
    mth::DeathBroadcastGate gate;
    for (int i = 0; i < 1000; ++i)
        (void)gate.observe(false, true, true);
    REQUIRE(gate.observe(true, false, true));
    (void)gate.observe(false, true, true);
    REQUIRE_FALSE(gate.observe(true, false, true));
}

// Re-arming needs alive and not dying; health reading alive with the guard byte set does not count.
TEST_CASE("deathlink: the broadcast never re-arms while the guard byte is set", "[deathlink][echo]")
{
    mth::DeathBroadcastGate gate;
    for (int i = 0; i < 1000; ++i)
        (void)gate.observe(false, true, true);
    REQUIRE(gate.observe(true, false, true));
    for (int i = 0; i < 1000; ++i)
        REQUIRE_FALSE(gate.observe(true, true, true));
}

// A busy multiworld: bounces arrive every second from mid-transition on. Each death runs to the respawn before the next, and none is echoed.
TEST_CASE("deathlink: a steady stream of bounces keeps landing deaths and never echoes one", "[deathlink][storm]")
{
    Deathlink d;
    d.game.despawn_player();
    d.receive_deathlink();
    d.game.play_for(0.5);
    d.game.spawn_player();
    for (int second = 0; second < 60; ++second)
    {
        d.game.play_for(1.0);
        d.receive_deathlink();
    }
    d.game.play_for(10.0);

    const long deaths = d.game.count("died");
    REQUIRE(deaths >= 2);
    Log expected;
    for (long i = 0; i < deaths; ++i)
        expected.insert(expected.end(), kDeathRun.begin(), kDeathRun.end());
    REQUIRE(d.game.log == expected);
}

TEST_CASE("deathlink: several bounces before the player is back apply as one death", "[deathlink][storm]")
{
    Deathlink d;
    d.game.despawn_player();
    d.receive_deathlink();
    d.game.play_for(0.25);
    d.receive_deathlink();
    d.game.play_for(0.25);
    d.receive_deathlink();
    d.game.spawn_player();
    d.game.play_for(30.0);
    REQUIRE(d.game.log == kDeathRun);
}

// #164: a bounce during a world or screen transition, while no Player exists.
TEST_CASE("deathlink: a bounce with no player lands soon after the player returns", "[deathlink][retry]")
{
    Deathlink d;
    d.game.despawn_player();
    d.receive_deathlink();
    d.game.play_for(0.5);
    REQUIRE(d.game.log.empty());

    d.game.spawn_player();
    d.game.play_for(2.0);
    REQUIRE(d.game.log == Log{"died"});
}

// The retry window counts gameplay time, so a menu does not spend it; a bounce that never finds a settled player is dropped.
TEST_CASE("deathlink: a held bounce is dropped after a long absence, counting gameplay time only", "[deathlink][retry]")
{
    Deathlink d;
    d.game.despawn_player();
    d.receive_deathlink();
    SECTION("the player stays away")
    {
        d.game.play_for(60.0);
        d.game.spawn_player();
        d.game.play_for(10.0);
        REQUIRE(d.game.log.empty());
    }
    SECTION("the room clock stops while the player is away")
    {
        d.game.open_menu();
        d.game.play_for(60.0);
        d.game.close_menu();
        d.game.spawn_player();
        d.game.play_for(10.0);
        REQUIRE(d.game.log == kDeathRun);
    }
}

// PlayerDie softlocks the game in an area-change state, so the bounce waits however long the cutscene runs.
TEST_CASE("deathlink: a bounce during a cutscene waits for it to end", "[deathlink][retry]")
{
    Deathlink d;
    d.game.enter_cutscene();
    d.receive_deathlink();
    d.game.play_for(30.0);
    REQUIRE(d.game.log.empty());

    d.game.leave_cutscene();
    d.game.play_for(10.0);
    REQUIRE(d.game.log == kDeathRun);
}

// The area is unbound between teardown and the next room's AreaManagerNewArea, a window that clears by itself.
TEST_CASE("deathlink: a bounce during a room change lands in the next room", "[deathlink][retry]")
{
    Deathlink d;
    d.game.begin_room_change();
    d.game.play_for(0.1);
    d.receive_deathlink();
    d.game.play_for(1.0);
    REQUIRE(d.game.log.empty());

    d.game.finish_room_change();
    d.game.play_for(10.0);
    REQUIRE(d.game.log == kDeathRun);
}

TEST_CASE("deathlink: a bounce during our own death does not kill us again", "[deathlink][retry]")
{
    Deathlink d;
    d.game.die_locally();
    d.game.play_for(0.5);
    d.receive_deathlink();
    d.game.play_for(30.0);
    REQUIRE(d.game.log == kBroadcastDeathRun);
}

// The ending leaves a settled Player in a World with no area, where PlayerDie faults in Player::InitDeath (crash 2026-08-23, during the credits). The
// bounce is dropped: held, it would land in the post-credits handoff.
TEST_CASE("deathlink: a bounce during the credits is dropped", "[deathlink][ending]")
{
    Deathlink d;
    d.game.enter_credits();
    d.receive_deathlink();
    d.game.play_for(1.0);
    d.game.leave_credits();
    d.game.play_for(30.0);
    REQUIRE(d.game.log.empty());
}

// A build without PlayerGetWorld cannot tell an area-less world from no world at all, and must not lose every deathlink for it.
TEST_CASE("deathlink: the credits check lets deaths through on a build with no world accessor", "[deathlink][ending]")
{
    Deathlink d;
    d.game.api.PlayerGetWorld = nullptr;
    d.receive_deathlink();
    d.game.play_for(10.0);
    REQUIRE(d.game.log == kDeathRun);
}
