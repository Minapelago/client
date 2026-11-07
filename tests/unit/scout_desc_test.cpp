#include <string>

#include <catch2/catch_test_macros.hpp>

#include "mth/core/ap/ap_events.hpp"

TEST_CASE("format_scout_desc: the other-player form names the recipient and their game", "[scout]")
{
    const std::string d = mth::format_scout_desc(mth::ScoutInfo{1, "Sword", "Alice", "Some Game", 0u, false});
    REQUIRE(d.find("Alice") != std::string::npos);
    REQUIRE(d.find("Some Game") != std::string::npos);
}

TEST_CASE("format_scout_desc: the self form names neither the alias nor the game", "[scout]")
{
    // Our own slot still carries an alias and a game, so a self-scout that fell through to the
    // other-player branch would advertise "for Mina (Mina the Hollower)" in the shop.
    const std::string d = mth::format_scout_desc(mth::ScoutInfo{1, "Sword", "Mina", "Mina the Hollower", 0u, true});
    REQUIRE(d.find("Mina") == std::string::npos);
    REQUIRE(d.find("Mina the Hollower") == std::string::npos);
}
