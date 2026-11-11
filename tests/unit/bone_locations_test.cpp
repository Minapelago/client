#include <cstdint>
#include <set>

#include <catch2/catch_test_macros.hpp>

#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/game/bone_locations.hpp"

// Location ids freeze once a release ships, so the table may only grow at the end.
TEST_CASE("bone table: one row per spawn point, ids dense from kBoneLocBase", "[bones]")
{
    const auto rows = mth::bone_rows();
    REQUIRE_FALSE(rows.empty());
    std::set<int> ids;
    for (const mth::BoneRow &row : rows)
        ids.insert(mth::bone_location_id(row));
    REQUIRE(ids.size() == rows.size());
    CHECK(*ids.begin() == mth::kBoneLocBase);
    CHECK(*ids.rbegin() == mth::kBoneLocBase + static_cast<int>(rows.size()) - 1);
    CHECK(*ids.rbegin() < mth::kBoneLocBase + mth::kBoneLocSegmentWidth);
    CHECK(mth::kBoneLocBase > mth::boss_location_slot(mth::kMaxBossIndex));
}

TEST_CASE("find_bone: a spawn-point key finds its row, anything else finds nothing", "[bones]")
{
    const mth::BoneRow *b = mth::find_bone(0x9f2a6c8e603c24ceull); // hub MainSquare Bone 2
    REQUIRE(b != nullptr);
    CHECK(mth::bone_location_id(*b) == 11323);
    CHECK_FALSE(b->excluded);

    REQUIRE(mth::find_bone(0xbb75d9a6981acfa3ull) != nullptr);
    CHECK(mth::bone_location_id(*mth::find_bone(0xbb75d9a6981acfa3ull)) == 10000);
    REQUIRE(mth::find_bone(0x27a6332daa2dcf71ull) != nullptr);
    CHECK(mth::bone_location_id(*mth::find_bone(0x27a6332daa2dcf71ull)) == 12412);
    REQUIRE(mth::find_bone(0x82d9fcf3d14fd25bull) != nullptr); // crypt_overworld_hell, not revisitable
    CHECK(mth::find_bone(0x82d9fcf3d14fd25bull)->excluded);

    CHECK(mth::find_bone(0) == nullptr); // enemy and breakable drops have no spawn point
    CHECK(mth::find_bone(1) == nullptr);
    CHECK(mth::find_bone(~0ull) == nullptr);
}

TEST_CASE("is_bone_pickup: a bone item type on a pickup with no collection slot", "[bones]")
{
    CHECK(mth::is_bone_pickup(-1, 40));
    CHECK(mth::is_bone_pickup(-1, 41));
    CHECK(mth::is_bone_pickup(-1, 50));
    CHECK_FALSE(mth::is_bone_pickup(-1, 39));
    CHECK_FALSE(mth::is_bone_pickup(-1, 51));
    // A collected child comes back from Init as this refill, so the respawn gate reads Init's argument.
    CHECK_FALSE(mth::is_bone_pickup(-1, 37));
    CHECK_FALSE(mth::is_bone_pickup(12, 41));
}

TEST_CASE("is_bone_grant: only the payout of the bone pickup in progress", "[bones]")
{
    CHECK(mth::is_bone_grant(-1, 41, 41));
    CHECK(mth::is_bone_grant(-1, 50, 50));
    CHECK_FALSE(mth::is_bone_grant(12, 41, 41)); // a collection slot's grant
    CHECK_FALSE(mth::is_bone_grant(-1, 41, 42)); // another pickup's payout
    CHECK_FALSE(mth::is_bone_grant(-1, 41, -1)); // no bone in progress
    CHECK_FALSE(mth::is_bone_grant(-1, 37, 37)); // a refill
}

TEST_CASE("should_destroy_bone: a checked bone the game left alive", "[bones]")
{
    CHECK(mth::should_destroy_bone(true, false));
    CHECK_FALSE(mth::should_destroy_bone(true, true));
    CHECK_FALSE(mth::should_destroy_bone(false, false));
    CHECK_FALSE(mth::should_destroy_bone(false, true));
}
