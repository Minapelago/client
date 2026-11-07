#include <algorithm>
#include <cstdint>
#include <iterator>
#include <set>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "mth/core/game/modifiers.hpp"

using mth::CheatClass;

TEST_CASE("class_of partitions the index space, and is_safe is exactly the continuous set", "[modifiers]")
{
    REQUIRE(mth::class_of(-1) == CheatClass::Invalid);
    REQUIRE(mth::class_of(254) == CheatClass::Invalid);
    REQUIRE(mth::class_of(0) == CheatClass::Continuous);
    REQUIRE(mth::class_of(18) == CheatClass::Continuous);
    REQUIRE(mth::class_of(19) == CheatClass::Grant);
    REQUIRE(mth::class_of(20) == CheatClass::Continuous);
    REQUIRE(mth::class_of(54) == CheatClass::Grant);
    REQUIRE(mth::class_of(121) == CheatClass::Continuous);
    REQUIRE(mth::class_of(122) == CheatClass::Randomizer);
    REQUIRE(mth::class_of(125) == CheatClass::Randomizer);
    REQUIRE(mth::class_of(127) == CheatClass::Continuous);
    REQUIRE(mth::class_of(128) == CheatClass::Grant);
    REQUIRE(mth::class_of(172) == CheatClass::Grant);
    REQUIRE(mth::class_of(173) == CheatClass::Continuous);
    REQUIRE(mth::class_of(213) == CheatClass::Continuous);
    REQUIRE(mth::class_of(214) == CheatClass::Combo);
    REQUIRE(mth::class_of(253) == CheatClass::Combo);

    for (int idx = -1; idx <= mth::kCheatCount; ++idx)
    {
        INFO(idx);
        REQUIRE(mth::is_safe(idx) == (mth::class_of(idx) == CheatClass::Continuous));
    }
}

TEST_CASE("is_ap_denied refuses every save-mutating, shuffling and combo modifier", "[modifiers]")
{
    for (int idx = 0; idx < mth::kCheatCount; ++idx)
    {
        INFO(idx);
        if (mth::class_of(idx) != CheatClass::Continuous)
            REQUIRE(mth::is_ap_denied(idx));
    }
    REQUIRE_FALSE(mth::is_ap_denied(-1));
    REQUIRE_FALSE(mth::is_ap_denied(mth::kCheatCount));
}

TEST_CASE("is_ap_denied refuses continuous modifiers that break a randomized run, and only those", "[modifiers]")
{
    // Reach past the apworld's logic, hand out items, delete AP locations, reroll an AP sidearm, or skip the starting kit (bit 126).
    for (int idx : {0, 21, 86, 107, 70, 126, 89, 98, 116, 120})
    {
        INFO(idx);
        REQUIRE(mth::is_ap_denied(idx));
    }
    // Difficulty and cosmetic sets are the player's choice, and the AP slot itself forces some of them.
    for (int idx : {1, 8, 20, 31, 62, 99, 115, 127, 173, 186, 213, mth::kCheatWarpHome, mth::kCheatCheaperBoneUp, mth::kCheatUnlockBoneUps})
    {
        INFO(idx);
        REQUIRE_FALSE(mth::is_ap_denied(idx));
    }
}

TEST_CASE("cheat_mask: set and clear touch only the target bit", "[cheat_mask]")
{
    // One index at each end of each 64-bit word, plus 255, the top of the range the mask can address.
    for (int idx : {0, 63, 64, 127, 128, 191, 192, 253, 255})
    {
        INFO(idx);
        std::uint64_t mask[4] = {0, 0, 0, 0};
        mth::cheat_mask_set(mask, idx, true);
        for (int w = 0; w < 4; ++w)
            REQUIRE(mask[w] == (w == idx / 64 ? std::uint64_t{1} << (idx % 64) : 0));

        std::uint64_t full[4] = {~std::uint64_t{0}, ~std::uint64_t{0}, ~std::uint64_t{0}, ~std::uint64_t{0}};
        mth::cheat_mask_set(full, idx, false);
        for (int w = 0; w < 4; ++w)
            REQUIRE(~full[w] == mask[w]);
    }
}

TEST_CASE("cheat_mask: an out-of-range index writes nothing, inside or around the mask", "[cheat_mask]")
{
    // idx=-1 and idx=256 would land one word below and above the mask, so the mask sits between two guard words.
    for (int idx : {-1, 256})
    {
        const std::uint64_t before[6] = {0x5555555555555555ull, 0x1111111111111111ull, 0x2222222222222222ull,
                                         0x3333333333333333ull, 0x4444444444444444ull, 0xaaaaaaaaaaaaaaaaull};
        std::uint64_t buf[6];
        std::copy(std::begin(before), std::end(before), buf);
        mth::cheat_mask_set(&buf[1], idx, true);
        mth::cheat_mask_set(&buf[1], idx, false);
        REQUIRE(std::equal(std::begin(buf), std::end(buf), std::begin(before)));
    }
}

// Each trap is forced on live for a while, so it must be continuous, allowed on AP slots, and findable from its item id.
TEST_CASE("trap table: every row is a distinct, live-settable, allowed modifier with a label and duration", "[trap][trap_table]")
{
    std::set<int> seen;
    for (const auto &t : mth::traps())
    {
        INFO(t.modifier_index);
        REQUIRE(seen.insert(t.modifier_index).second);
        REQUIRE(mth::is_safe(t.modifier_index));
        REQUIRE_FALSE(mth::is_ap_denied(t.modifier_index));
        REQUIRE(t.label != nullptr);
        REQUIRE_FALSE(std::string_view(t.label).empty());
        REQUIRE(t.seconds > 0.0f);
        REQUIRE(mth::trap_for_modifier(t.modifier_index) == &t);
    }

    REQUIRE(mth::trap_for_modifier(19) == nullptr);
    REQUIRE(mth::trap_for_modifier(-1) == nullptr);
    REQUIRE(mth::trap_for_modifier(9999) == nullptr);
}
