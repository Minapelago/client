#include <array>
#include <cstdint>
#include <set>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/game/modifiers.hpp"

using mth::CheatClass;

TEST_CASE("class_of partitions the index space", "[modifiers]")
{
    REQUIRE(mth::class_of(-1) == CheatClass::Invalid);
    REQUIRE(mth::class_of(254) == CheatClass::Invalid);
    REQUIRE(mth::class_of(0) == CheatClass::Continuous);
    REQUIRE(mth::class_of(31) == CheatClass::Continuous);
    REQUIRE(mth::class_of(126) == CheatClass::Continuous);
    REQUIRE(mth::class_of(19) == CheatClass::Grant);
    REQUIRE(mth::class_of(54) == CheatClass::Grant);
    REQUIRE(mth::class_of(140) == CheatClass::Grant);
    REQUIRE(mth::class_of(123) == CheatClass::Randomizer);
    REQUIRE(mth::class_of(218) == CheatClass::Combo);
    REQUIRE(mth::class_of(253) == CheatClass::Combo);
}

TEST_CASE("class_of range boundaries are fence-post correct", "[modifiers]")
{
    // Edges around every range so an off-by-one in class_of is caught.
    REQUIRE(mth::class_of(18) == CheatClass::Continuous);
    REQUIRE(mth::class_of(20) == CheatClass::Continuous);
    REQUIRE(mth::class_of(53) == CheatClass::Continuous);
    REQUIRE(mth::class_of(55) == CheatClass::Continuous);
    REQUIRE(mth::class_of(121) == CheatClass::Continuous);
    REQUIRE(mth::class_of(122) == CheatClass::Randomizer);
    REQUIRE(mth::class_of(125) == CheatClass::Randomizer);
    REQUIRE(mth::class_of(127) == CheatClass::Continuous);
    REQUIRE(mth::class_of(128) == CheatClass::Grant);
    REQUIRE(mth::class_of(172) == CheatClass::Grant);
    REQUIRE(mth::class_of(173) == CheatClass::Continuous);
    REQUIRE(mth::class_of(213) == CheatClass::Continuous);
    REQUIRE(mth::class_of(214) == CheatClass::Combo);
}

TEST_CASE("is_safe is exactly the continuous set", "[modifiers]")
{
    REQUIRE(mth::is_safe(31));
    REQUIRE_FALSE(mth::is_safe(140));
    REQUIRE_FALSE(mth::is_safe(123));
    REQUIRE_FALSE(mth::is_safe(218));
}

TEST_CASE("is_ap_denied covers the hostile modifiers", "[modifiers]")
{
    REQUIRE(mth::is_ap_denied(0));
    REQUIRE(mth::is_ap_denied(2));
    REQUIRE(mth::is_ap_denied(5));
    REQUIRE(mth::is_ap_denied(17));
    REQUIRE(mth::is_ap_denied(21));
    REQUIRE(mth::is_ap_denied(19));
    REQUIRE(mth::is_ap_denied(54));
    REQUIRE(mth::is_ap_denied(70));
    REQUIRE(mth::is_ap_denied(95));
    REQUIRE(mth::is_ap_denied(123));
    REQUIRE(mth::is_ap_denied(126));
    REQUIRE(mth::is_ap_denied(140));
    REQUIRE(mth::is_ap_denied(48));
    REQUIRE(mth::is_ap_denied(107));
    REQUIRE(mth::is_ap_denied(119));
    REQUIRE(mth::is_ap_denied(253));
}

TEST_CASE("is_ap_denied leaves difficulty and cosmetic modifiers alone", "[modifiers]")
{
    REQUIRE_FALSE(mth::is_ap_denied(1));
    REQUIRE_FALSE(mth::is_ap_denied(8));
    REQUIRE_FALSE(mth::is_ap_denied(9));
    REQUIRE_FALSE(mth::is_ap_denied(20));
    REQUIRE_FALSE(mth::is_ap_denied(31));
    REQUIRE_FALSE(mth::is_ap_denied(62));
    REQUIRE_FALSE(mth::is_ap_denied(102));
    REQUIRE_FALSE(mth::is_ap_denied(121));
    REQUIRE_FALSE(mth::is_ap_denied(186));
    REQUIRE_FALSE(mth::is_ap_denied(213));
    REQUIRE_FALSE(mth::is_ap_denied(-1));
    REQUIRE_FALSE(mth::is_ap_denied(254));
}

TEST_CASE("is_ap_denied range boundaries are fence-post correct", "[modifiers]")
{
    // Edges around every denied range so an off-by-one is caught.
    REQUIRE(mth::is_ap_denied(7));
    REQUIRE_FALSE(mth::is_ap_denied(16));
    REQUIRE_FALSE(mth::is_ap_denied(18));
    REQUIRE_FALSE(mth::is_ap_denied(71));
    REQUIRE_FALSE(mth::is_ap_denied(73));
    REQUIRE(mth::is_ap_denied(89));
    REQUIRE(mth::is_ap_denied(98));
    REQUIRE_FALSE(mth::is_ap_denied(99));
    REQUIRE_FALSE(mth::is_ap_denied(115));
    REQUIRE(mth::is_ap_denied(116));
    REQUIRE(mth::is_ap_denied(120));
    REQUIRE(mth::is_ap_denied(122));
    REQUIRE(mth::is_ap_denied(125));
    REQUIRE_FALSE(mth::is_ap_denied(127));
    REQUIRE(mth::is_ap_denied(128));
    REQUIRE(mth::is_ap_denied(172));
    REQUIRE_FALSE(mth::is_ap_denied(173));
    REQUIRE(mth::is_ap_denied(214));
    REQUIRE(mth::is_ap_denied(253));
}

namespace
{
// One index at each end of each of the four 64-bit words, plus 255, the top of the range the mask can address (the real cheat count stops at 253).
constexpr std::array<int, 9> kBoundaryIndices = {0, 63, 64, 127, 128, 191, 192, 253, 255};

bool bit_set(const std::uint64_t *mask, int idx)
{
    return ((mask[idx / 64] >> (idx % 64)) & 1u) != 0;
}
} // namespace

TEST_CASE("cheat_mask: setting one index touches only that bit, in only that word", "[cheat_mask]")
{
    for (int idx : kBoundaryIndices)
    {
        std::uint64_t mask[4] = {0, 0, 0, 0};
        mth::cheat_mask_set(mask, idx, true);

        const int w = idx / 64;
        REQUIRE(mask[w] == std::uint64_t{1} << (idx % 64));
        for (int other = 0; other < 4; ++other)
        {
            if (other != w)
                REQUIRE(mask[other] == 0);
        }
    }
}

TEST_CASE("cheat_mask: clearing zeroes only the target bit", "[cheat_mask]")
{
    std::uint64_t mask[4] = {~std::uint64_t{0}, ~std::uint64_t{0}, ~std::uint64_t{0}, ~std::uint64_t{0}};
    for (int idx : kBoundaryIndices)
        mth::cheat_mask_set(mask, idx, false);

    for (int idx = 0; idx < 256; ++idx)
    {
        bool was_cleared = false;
        for (int b : kBoundaryIndices)
            was_cleared = was_cleared || (b == idx);
        REQUIRE(bit_set(mask, idx) == !was_cleared);
    }
}

TEST_CASE("cheat_mask: set then clear round-trips to zero", "[cheat_mask]")
{
    for (int idx : kBoundaryIndices)
    {
        std::uint64_t mask[4] = {0, 0, 0, 0};
        mth::cheat_mask_set(mask, idx, true);
        mth::cheat_mask_set(mask, idx, false);
        for (int w = 0; w < 4; ++w)
            REQUIRE(mask[w] == 0);
    }
}

TEST_CASE("cheat_mask: an out-of-range index leaves cheat_mask_set a total no-op", "[cheat_mask]")
{
    // The four mask words sit in a six-word buffer with a guard word on each side, since idx=-1 and idx=256 write one word below and above the mask.
    // Distinct per-word sentinels catch a write to the wrong word inside the mask.
    for (int idx : {-1, 256})
    {
        std::uint64_t buf[6] = {0x5555555555555555ull, 0x1111111111111111ull, 0x2222222222222222ull,
                                0x3333333333333333ull, 0x4444444444444444ull, 0xaaaaaaaaaaaaaaaaull};
        std::uint64_t *mask = &buf[1];
        mth::cheat_mask_set(mask, idx, true);
        REQUIRE(buf[0] == 0x5555555555555555ull);
        REQUIRE(mask[0] == 0x1111111111111111ull);
        REQUIRE(mask[1] == 0x2222222222222222ull);
        REQUIRE(mask[2] == 0x3333333333333333ull);
        REQUIRE(mask[3] == 0x4444444444444444ull);
        REQUIRE(buf[5] == 0xaaaaaaaaaaaaaaaaull);

        mth::cheat_mask_set(mask, idx, false);
        REQUIRE(buf[0] == 0x5555555555555555ull);
        REQUIRE(mask[0] == 0x1111111111111111ull);
        REQUIRE(mask[1] == 0x2222222222222222ull);
        REQUIRE(mask[2] == 0x3333333333333333ull);
        REQUIRE(mask[3] == 0x4444444444444444ull);
        REQUIRE(buf[5] == 0xaaaaaaaaaaaaaaaaull);
    }
}

TEST_CASE("trap table: every trap is a live-settable continuous modifier", "[trap][trap_table]")
{
    for (const auto &t : mth::traps())
        REQUIRE(mth::is_safe(t.modifier_index));
}

TEST_CASE("trap table: no trap is on the AP deny list", "[trap][trap_table]")
{
    for (const auto &t : mth::traps())
        REQUIRE_FALSE(mth::is_ap_denied(t.modifier_index));
}

TEST_CASE("trap table: indices are unique and in range", "[trap][trap_table]")
{
    std::set<int> seen;
    for (const auto &t : mth::traps())
    {
        REQUIRE(t.modifier_index >= 0);
        REQUIRE(t.modifier_index < mth::kCheatCount);
        REQUIRE(seen.insert(t.modifier_index).second);
    }
    REQUIRE(seen.size() == mth::traps().size());
}

TEST_CASE("trap table: every trap has a label and a positive duration", "[trap][trap_table]")
{
    for (const auto &t : mth::traps())
    {
        REQUIRE(t.label != nullptr);
        REQUIRE_FALSE(std::string_view(t.label).empty());
        REQUIRE(t.seconds > 0.0f);
    }
}

TEST_CASE("trap table: lookup returns the row for every modifier index in the table", "[trap][trap_table]")
{
    for (const auto &t : mth::traps())
        REQUIRE(mth::trap_for_modifier(t.modifier_index) == &t);

    REQUIRE(mth::trap_for_modifier(19) == nullptr);
    REQUIRE(mth::trap_for_modifier(-1) == nullptr);
    REQUIRE(mth::trap_for_modifier(9999) == nullptr);
}

TEST_CASE("ap_ids: the trap segment is 1000 wide from kTrapItemBase", "[trap][ap_ids]")
{
    REQUIRE(mth::is_trap_item(mth::kTrapItemBase));
    REQUIRE(mth::is_trap_item(mth::kTrapItemBase + 204));
    REQUIRE(mth::is_trap_item(mth::kTrapItemBase + 999));
    REQUIRE_FALSE(mth::is_trap_item(mth::kTrapItemBase - 1));
    REQUIRE_FALSE(mth::is_trap_item(mth::kTrapItemBase + 1000));
    REQUIRE_FALSE(mth::is_trap_item(mth::ap_item_id(9)));
}

TEST_CASE("ap_ids: a trap id carries its modifier index", "[trap][ap_ids]")
{
    REQUIRE(mth::trap_modifier_index(mth::kTrapItemBase + 204) == 204);
    REQUIRE(mth::trap_modifier_index(mth::kTrapItemBase) == 0);
}

TEST_CASE("the trap index set is frozen, because a seed already carries these item ids", "[trap]")
{
    // Each row's modifier_index is the AP item id offset (kTrapItemBase + index), so retiring or renumbering a row repoints an id live seeds hand out.
    // Adding a row is safe, so this pins the set, not the count. Raw numbers: kCheat_ names would follow an upstream renumber and still pass.
    for (int index : {15, 174, 190, 191, 192, 193, 195, 197, 202, 203, 204, 205})
    {
        INFO("modifier index " << index << " left the table, retiring the AP item id built from it");
        REQUIRE(mth::trap_for_modifier(index) != nullptr);
    }
}
