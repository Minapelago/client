#include <catch2/catch_test_macros.hpp>

#include "mth/core/rules/goal.hpp"

TEST_CASE("the finish goal, and any unknown goal_config, keys only on the game-clear flag", "[goal]")
{
    REQUIRE(mth::goal_met(mth::kGoalFinish, 99, 99, true, 0, 0));
    REQUIRE_FALSE(mth::goal_met(mth::kGoalFinish, 0, 0, false, 50, 50));
    REQUIRE(mth::goal_met(7, 0, 0, true, 0, 0));
    REQUIRE_FALSE(mth::goal_met(7, 0, 0, false, 99, 99));
}

TEST_CASE("the generator and boss goals compare their count to the threshold", "[goal]")
{
    REQUIRE_FALSE(mth::goal_met(mth::kGoalGenerators, 5, 99, false, 4, 0));
    REQUIRE(mth::goal_met(mth::kGoalGenerators, 5, 99, false, 5, 0));
    REQUIRE(mth::goal_met(mth::kGoalGenerators, 5, 99, false, 7, 0));

    REQUIRE_FALSE(mth::goal_met(mth::kGoalBosses, 99, 10, false, 0, 9));
    REQUIRE(mth::goal_met(mth::kGoalBosses, 99, 10, false, 0, 10));
    REQUIRE(mth::goal_met(mth::kGoalBosses, 99, 10, false, 0, 12));
}

TEST_CASE("game-clear flag is in domain only as a boolean byte", "[goal]")
{
    REQUIRE(mth::game_clear_flag_in_domain(0));
    REQUIRE(mth::game_clear_flag_in_domain(1));
    REQUIRE_FALSE(mth::game_clear_flag_in_domain(2));
    REQUIRE_FALSE(mth::game_clear_flag_in_domain(0xab));
    REQUIRE_FALSE(mth::game_clear_flag_in_domain(0xff));
}

TEST_CASE("generators_done counts only generators the seed started broken (#141)", "[goal]")
{
    // Real slot_data shape: broken_generators {Noxs=0, Queensbury=1, Septemburg=2} must be fixed; the other
    // three start pre-repaired. Everything below is in SaveSlot 0x290 bit space.
    const std::uint64_t broken = mth::broken_generator_mask({0, 1, 2}); // bits {2,1,3}
    const std::uint64_t prelit = mth::broken_generator_mask({3, 4, 5}); // bits {4,5,6}

    REQUIRE(mth::generators_done(prelit, broken) == 0);
    REQUIRE(mth::generators_done(prelit | (std::uint64_t{1} << 2), broken) == 1);
    REQUIRE(mth::generators_done(prelit | broken, broken) == 3);
    // The bug #141 fixed: unmasked, the pre-lit trio alone already reads 3 and fires the goal immediately.
    REQUIRE(mth::generators_done(prelit, mth::kAllGeneratorsMask) == 3);
    REQUIRE(mth::goal_met(mth::kGoalGenerators, 3, 0, false, mth::generators_done(prelit, mth::kAllGeneratorsMask), 0));
    REQUIRE_FALSE(mth::goal_met(mth::kGoalGenerators, 3, 0, false, mth::generators_done(prelit, broken), 0));
}

TEST_CASE("a goal set including Noxs Bayou completes once all its generators are fixed (#146)", "[goal]")
{
    // #146: a 3-generator goal including Noxs Bayou (index 0) never completed, because the mask put its bit at 0, which SetGeneratorFixed never sets.
    const std::uint64_t broken = mth::broken_generator_mask({0, 1, 2});
    const std::uint64_t all_three_fixed = (std::uint64_t{1} << 2) | (std::uint64_t{1} << 1) | (std::uint64_t{1} << 3);
    REQUIRE(mth::generators_done(all_three_fixed, broken) == 3);
    REQUIRE(mth::goal_met(mth::kGoalGenerators, 3, 0, false, mth::generators_done(all_three_fixed, broken), 0));
}
