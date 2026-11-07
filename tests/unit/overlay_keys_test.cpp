#include <catch2/catch_test_macros.hpp>

#include "pal/overlay_keys.hpp"

namespace
{
constexpr int kF1 = 100;
constexpr int kBackquote = 500;

int console_key(const char *name)
{
    return pal::parse_console_key<int>(name, kF1, kBackquote);
}
} // namespace

TEST_CASE("console key parses F1..F12 and backquote names", "[overlay_keys]")
{
    REQUIRE(console_key("F1") == kF1);
    REQUIRE(console_key("F9") == kF1 + 8);
    REQUIRE(console_key("F10") == kF1 + 9);
    REQUIRE(console_key("F12") == kF1 + 11);
    REQUIRE(console_key("BACKQUOTE") == kBackquote);
    REQUIRE(console_key("GRAVE") == kBackquote);
    REQUIRE(console_key("`") == kBackquote);
}

TEST_CASE("console key falls back to F1 on anything else", "[overlay_keys]")
{
    for (const char *bad : {"", "F", "F0", "F01", "F13", "F+1", "F-1", "F1x", "f1", "F 1", "q"})
        REQUIRE(console_key(bad) == kF1);
    REQUIRE(console_key(nullptr) == kF1);
}

TEST_CASE("login key moves to F2 when it collides with the console key", "[overlay_keys]")
{
    REQUIRE(pal::parse_login_key<int>(nullptr, kF1, kF1, kBackquote) == kF1 + 1);
    REQUIRE(pal::parse_login_key<int>("F5", kF1, kF1, kBackquote) == kF1 + 4);
    REQUIRE(pal::parse_login_key<int>("F5", kF1 + 4, kF1, kBackquote) == kF1 + 1);
}
