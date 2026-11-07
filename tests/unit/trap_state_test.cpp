#include <set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "mth/core/rules/trap_state.hpp"

namespace
{
std::set<int> indices(const mth::TrapState &s)
{
    std::set<int> out;
    for (const mth::ActiveTrap &t : s.traps())
        out.insert(t.index);
    return out;
}

std::set<int> as_set(const std::vector<int> &v)
{
    return {v.begin(), v.end()};
}
} // namespace

TEST_CASE("TrapState: an armed trap expires exactly once, after its duration of positive time", "[trap]")
{
    mth::TrapState s;
    s.arm(204, 30.0f);
    REQUIRE(indices(s) == std::set<int>{204});

    REQUIRE(s.advance(10.0f).empty());
    REQUIRE(s.advance(0.0f).empty());
    REQUIRE(s.advance(-5.0f).empty());
    REQUIRE(s.advance(9999.0f) == std::vector<int>{204});
    REQUIRE(s.empty());
    REQUIRE(s.advance(30.0f).empty());

    s.arm(204, 0.0f);
    s.arm(205, -1.0f);
    REQUIRE(s.empty());
}

TEST_CASE("TrapState: re-arming refreshes and never shortens", "[trap]")
{
    mth::TrapState s;
    s.arm(204, 30.0f);
    s.advance(25.0f);
    s.arm(204, 30.0f);
    REQUIRE(s.traps().size() == 1);
    REQUIRE(s.advance(10.0f).empty());

    s.arm(204, 2.0f);
    REQUIRE(s.advance(10.0f).empty());
}

TEST_CASE("TrapState: traps stack and expire independently", "[trap]")
{
    mth::TrapState s;
    s.arm(204, 10.0f);
    s.arm(197, 20.0f);
    s.arm(202, 20.0f);
    REQUIRE(indices(s) == std::set<int>{197, 202, 204});

    REQUIRE(s.advance(10.0f) == std::vector<int>{204});
    REQUIRE(as_set(s.advance(10.0f)) == std::set<int>{197, 202});
    REQUIRE(s.empty());
}

TEST_CASE("TrapState: clear returns what was running and empties", "[trap]")
{
    mth::TrapState s;
    s.arm(204, 30.0f);
    s.arm(197, 30.0f);
    REQUIRE(as_set(s.clear()) == std::set<int>{197, 204});
    REQUIRE(s.empty());
    REQUIRE(s.clear().empty());
}
