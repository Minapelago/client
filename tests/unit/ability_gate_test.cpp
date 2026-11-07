#include <catch2/catch_test_macros.hpp>

#include "mth/core/ap/ability_ids.hpp"
#include "mth/core/rules/ability_gate.hpp"

using mth::Ability;

TEST_CASE("ability_from_name maps known names and rejects unknown")
{
    REQUIRE(mth::ability_from_name("burrow") == Ability::Burrow);
    REQUIRE(mth::ability_from_name("swim") == Ability::Swim);
    REQUIRE(mth::ability_from_name("rope") == Ability::RopeClimb);
    REQUIRE(mth::ability_from_name("puff") == Ability::BouncePuff);
    REQUIRE(mth::ability_from_name("spring") == Ability::BounceSpring);
    REQUIRE(mth::ability_from_name("carry") == Ability::Carry);
    REQUIRE(mth::ability_from_name("train") == Ability::Train);
    REQUIRE_FALSE(mth::ability_from_name("unknown").has_value());
    REQUIRE_FALSE(mth::ability_from_name("").has_value());
}

TEST_CASE("AbilityGate blocks only randomized, ungranted abilities on the AP slot")
{
    mth::AbilityGate gate;
    gate.set_randomized(Ability::Burrow, true); // randomized this seed
    // Swim left non-randomized.

    const std::function<bool(std::int64_t)> granted = [](std::int64_t) { return true; };
    const std::function<bool(std::int64_t)> ungranted = [](std::int64_t) { return false; };

    REQUIRE(gate.blocked(Ability::Burrow, true, ungranted));
    REQUIRE_FALSE(gate.blocked(Ability::Burrow, true, granted));
    REQUIRE_FALSE(gate.blocked(Ability::Burrow, false, ungranted));
    REQUIRE_FALSE(gate.blocked(Ability::Swim, true, ungranted));
}
