#include <catch2/catch_test_macros.hpp>

#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/game/game_layout.hpp"

TEST_CASE("ap_item_id and game_item_type round-trip", "[ap_ids]")
{
    REQUIRE(mth::ap_item_id(0) == mth::kItemBase);
    REQUIRE(mth::ap_item_id(42) == mth::kItemBase + 42);
    REQUIRE(mth::game_item_type(mth::ap_item_id(42)) == 42);
    REQUIRE(mth::game_item_type(mth::kItemBase) == 0);
}

TEST_CASE("vanilla kear item id is the Universal Kear itemType", "[ap_ids]")
{
    REQUIRE(mth::is_vanilla_kear_item(mth::ap_item_id(63)));
    REQUIRE(mth::is_vanilla_kear_item(63));
    REQUIRE_FALSE(mth::is_vanilla_kear_item(mth::ap_item_id(62)));
    REQUIRE_FALSE(mth::is_vanilla_kear_item(mth::kKearBlockItemBase));
}

TEST_CASE("stat-cap item ids: per-stat and all-stat flavours", "[ap_ids]")
{
    REQUIRE(mth::is_stat_cap_item(mth::kProgStatCapBase) == true);
    REQUIRE(mth::is_stat_cap_item(mth::kProgStatCapBase + 2) == true);
    REQUIRE(mth::is_stat_cap_item(mth::ap_item_id(42)) == false);
    REQUIRE(mth::stat_cap_item_stat(mth::kProgStatCapBase) == 0);
    REQUIRE(mth::stat_cap_item_stat(mth::kProgStatCapBase + 2) == 2);

    REQUIRE(mth::is_stat_cap_all_item(mth::kProgStatCapAllId));
    REQUIRE(mth::is_stat_cap_item(mth::kProgStatCapAllId));
    REQUIRE_FALSE(mth::is_stat_cap_all_item(mth::kProgStatCapBase));
}

TEST_CASE("weapon ids map family+tier to engine itemTypes", "[ap_ids]")
{
    REQUIRE(mth::is_weapon_item(mth::kProgWeaponBase));
    REQUIRE(mth::is_weapon_item(mth::kProgWeaponBase + 4));
    REQUIRE_FALSE(mth::is_weapon_item(mth::kProgStatCapBase));
    REQUIRE(mth::weapon_family(mth::kProgWeaponBase + 3) == 3);

    REQUIRE(mth::weapon_itemtype(0, 1) == 2);
    REQUIRE(mth::weapon_itemtype(0, 3) == 4);
    REQUIRE(mth::weapon_itemtype(4, 1) == 14);
    REQUIRE(mth::weapon_itemtype(4, 3) == 16);
    REQUIRE(mth::weapon_itemtype(0, 4) == -1);
    REQUIRE(mth::weapon_itemtype(5, 1) == -1);
}

// WeaponTally turns the received-item stream into the ownership bits the seed authorized, so the per-tick clamp can revoke a weapon the game hands
// out itself (intro chest, no-weapon fallback). The Nth receipt of a family is tier N and the engine bit is (itemType - 2) % 3, so N tiers is the low N bits.
TEST_CASE("weapon tally: nothing received authorizes no weapon at all", "[ap_ids]")
{
    const mth::WeaponTally tally;
    for (int fam = 0; fam < mth::kWeaponFamilyCount; ++fam)
        CHECK(tally.owned_mask(fam) == 0u);
}

TEST_CASE("weapon tally: one receipt authorizes that family's tier-1 bit only", "[ap_ids]")
{
    mth::WeaponTally tally;
    CHECK(tally.add(mth::kProgWeaponBase + 1) == 1);
    CHECK(tally.owned_mask(1) == 0b001u);
    CHECK(tally.owned_mask(0) == 0u);
    CHECK(tally.owned_mask(4) == 0u);
}

TEST_CASE("weapon tally: three receipts authorize all three tier bits", "[ap_ids]")
{
    mth::WeaponTally tally;
    CHECK(tally.add(mth::kProgWeaponBase + 4) == 1);
    CHECK(tally.add(mth::kProgWeaponBase + 4) == 2);
    CHECK(tally.add(mth::kProgWeaponBase + 4) == 3);
    CHECK(tally.owned_mask(4) == 0b111u);

    // A fourth receipt grants no itemType, so it authorizes no fourth bit.
    CHECK(tally.add(mth::kProgWeaponBase + 4) == 4);
    CHECK(tally.owned_mask(4) == 0b111u);
}

TEST_CASE("weapon tally: families count independently", "[ap_ids]")
{
    mth::WeaponTally tally;
    tally.add(mth::kProgWeaponBase + 0); // Whip x1
    tally.add(mth::kProgWeaponBase + 2); // Daggers x2
    tally.add(mth::kProgWeaponBase + 2);
    CHECK(tally.owned_mask(0) == 0b001u);
    CHECK(tally.owned_mask(2) == 0b011u);
    CHECK(tally.owned_mask(1) == 0u);
    CHECK(tally.owned_mask(3) == 0u);
    CHECK(tally.owned_mask(4) == 0u);
}

TEST_CASE("weapon tally: non-weapon receipts authorize nothing", "[ap_ids]")
{
    mth::WeaponTally tally;
    CHECK(tally.add(mth::ap_item_id(42)) == 0);
    CHECK(tally.add(mth::kProgStatCapBase) == 0);
    CHECK(tally.add(mth::kProgFishingRodId) == 0);
    CHECK(tally.add(mth::kKearBlockItemBase) == 0);
    CHECK(tally.add(mth::kProgWeaponBase + 3) == 1);
    CHECK(tally.add(mth::ap_item_id(63)) == 0);
    CHECK(tally.owned_mask(3) == 0b001u);
    for (int fam = 0; fam < mth::kWeaponFamilyCount; ++fam)
        if (fam != 3)
            CHECK(tally.owned_mask(fam) == 0u);
}

// The intro weapon chest is demoted to its equip-only mode, which lists only owned weapons; demoting it before a weapon grant lands would leave an
// empty chest and an unarmed player.
TEST_CASE("weapon authorization: an empty mask authorizes nothing", "[ap_ids]")
{
    const mth::WeaponTally tally;
    std::uint32_t authorized[mth::kWeaponFamilyCount]{};
    for (int fam = 0; fam < mth::kWeaponFamilyCount; ++fam)
        authorized[fam] = tally.owned_mask(fam);
    CHECK_FALSE(mth::any_weapon_authorized(authorized));
}

TEST_CASE("weapon authorization: one receipt in any family is enough", "[ap_ids]")
{
    for (int fam = 0; fam < mth::kWeaponFamilyCount; ++fam)
    {
        mth::WeaponTally tally;
        tally.add(mth::kProgWeaponBase + fam);
        std::uint32_t authorized[mth::kWeaponFamilyCount]{};
        for (int f = 0; f < mth::kWeaponFamilyCount; ++f)
            authorized[f] = tally.owned_mask(f);
        CHECK(mth::any_weapon_authorized(authorized));
    }
}

TEST_CASE("weapon authorization: non-weapon receipts still authorize nothing", "[ap_ids]")
{
    mth::WeaponTally tally;
    tally.add(mth::ap_item_id(42));    // a vanilla game item
    tally.add(mth::kProgStatCapBase);  // a stat-cap chain
    tally.add(mth::kProgFishingRodId); // another progressive chain
    std::uint32_t authorized[mth::kWeaponFamilyCount]{};
    for (int fam = 0; fam < mth::kWeaponFamilyCount; ++fam)
        authorized[fam] = tally.owned_mask(fam);
    CHECK_FALSE(mth::any_weapon_authorized(authorized));
}

TEST_CASE("item-space segments are 1000-wide and ordered", "[ap_ids]")
{
    REQUIRE(mth::kItemBase == 0);
    REQUIRE(mth::kProgressiveItemBase == 1000);
    REQUIRE(mth::kKearBlockItemBase == 2000);
    REQUIRE(mth::kAbilityItemBase == 3000);
    REQUIRE(mth::kBlockerItemBase == 4000);
    REQUIRE(mth::kTrapItemBase == 5000);
    // Progressive sub-layout stays within segment 1.
    REQUIRE(mth::kProgStatCapAllId < mth::kKearBlockItemBase);
}

TEST_CASE("is_vanilla_game_item recognises only item segment 0", "[ap_ids]")
{
    REQUIRE(mth::is_vanilla_game_item(mth::ap_item_id(0)));
    REQUIRE(mth::is_vanilla_game_item(mth::ap_item_id(194)));
    REQUIRE_FALSE(mth::is_vanilla_game_item(mth::kProgWeaponBase));
    REQUIRE_FALSE(mth::is_vanilla_game_item(mth::kProgStatCapBase));
    REQUIRE_FALSE(mth::is_vanilla_game_item(mth::kKearBlockItemBase));
    REQUIRE_FALSE(mth::is_vanilla_game_item(mth::kAbilityItemBase));
    REQUIRE_FALSE(mth::is_vanilla_game_item(mth::kBlockerItemBase));
    REQUIRE_FALSE(mth::is_vanilla_game_item(mth::kTrapItemBase));
    REQUIRE_FALSE(mth::is_vanilla_game_item(-1));
}

TEST_CASE("is_valid_item_type bounds a type against the s_rItems row count", "[ap_ids]")
{
    REQUIRE(mth::is_valid_item_type(0));
    REQUIRE(mth::is_valid_item_type(mth::layout::kItemTypeCount - 1));
    // Item segment 0 runs to 999, but only the first kItemTypeCount ids are itemTypes.
    REQUIRE_FALSE(mth::is_valid_item_type(mth::layout::kItemTypeCount));
    REQUIRE_FALSE(mth::is_valid_item_type(500));
    REQUIRE_FALSE(mth::is_valid_item_type(999));
    REQUIRE_FALSE(mth::is_valid_item_type(-1));
}

TEST_CASE("boss_location_slot maps index into the reserved range", "[boss]")
{
    REQUIRE(mth::boss_location_slot(0) == mth::kBossLocBase);
    REQUIRE(mth::boss_location_slot(5) == mth::kBossLocBase + 5);
    REQUIRE(mth::kBossLocBase >= 361);
}

TEST_CASE("is_legovich_location matches only the WeaponMerchant shop slots", "[legovich]")
{
    REQUIRE(mth::is_legovich_location(174));
    REQUIRE(mth::is_legovich_location(178));
    REQUIRE_FALSE(mth::is_legovich_location(173));
    REQUIRE_FALSE(mth::is_legovich_location(179));
    REQUIRE_FALSE(mth::is_legovich_location(-1));
}

TEST_CASE("upgrade_field_value: vials are authoritative and clear the vanilla base-3 (#83)", "[upgrade]")
{
    // Vanilla seeds 3 vials as SaveSlot+0x18c = 0x7; OR could never reduce it, so the count floored at 3.
    REQUIRE(mth::upgrade_field_value(mth::kVialUpgradeIndex, 0, 0x7u) == 0x0u);
    REQUIRE(mth::upgrade_field_value(mth::kVialUpgradeIndex, 1, 0x7u) == 0x1u);
    REQUIRE(mth::upgrade_field_value(mth::kVialUpgradeIndex, 2, 0x7u) == 0x3u);
    REQUIRE(mth::upgrade_field_value(mth::kVialUpgradeIndex, 3, 0x0u) == 0x7u);
    REQUIRE(mth::upgrade_field_value(mth::kVialUpgradeIndex, 10, 0xFFFFu) == 0x3FFu);
}

TEST_CASE("upgrade_field_value: non-vial pools accumulate onto the current bits", "[upgrade]")
{
    REQUIRE(mth::upgrade_field_value(1, 0, 0x5u) == 0x5u);
    REQUIRE(mth::upgrade_field_value(1, 2, 0x4u) == 0x7u);
    REQUIRE(mth::upgrade_field_value(0, 3, 0x0u) == 0x7u);
}

TEST_CASE("train tickets map itemTypes 95-99 to destination-line bits", "[train]")
{
    REQUIRE(mth::is_train_ticket_item_type(0x5f));
    REQUIRE(mth::is_train_ticket_item_type(0x63));
    REQUIRE_FALSE(mth::is_train_ticket_item_type(0x5e));
    REQUIRE_FALSE(mth::is_train_ticket_item_type(0x64));
    REQUIRE_FALSE(mth::is_train_ticket_item_type(0));

    REQUIRE(mth::train_ticket_bit(0x5f) == 0x1u);
    REQUIRE(mth::train_ticket_bit(0x60) == 0x2u);
    REQUIRE(mth::train_ticket_bit(0x63) == 0x10u);
    REQUIRE(mth::train_ticket_bit(0x64) == 0x0u);
    REQUIRE(mth::train_ticket_bit(41) == 0x0u);
}

TEST_CASE("train_destination_blocked cancels un-granted ticket lines", "[train]")
{
    // Line 0 (Ossex/HUB, 95) is the free hub, rideable on the Train Pass alone, so it is never blocked.
    REQUIRE_FALSE(mth::train_destination_blocked(0x5f, 0x00));
    REQUIRE_FALSE(mth::train_destination_blocked(0x5f, 0x01));

    // Lines 1-4 (96-99) are blocked (selection cancelled) unless their bit is in the granted mask.
    REQUIRE(mth::train_destination_blocked(0x60, 0x01));
    REQUIRE_FALSE(mth::train_destination_blocked(0x60, 0x02));

    // Line 4 (99, Coltrane) is always shown in the menu, so the gate must still block it.
    REQUIRE(mth::train_destination_blocked(0x63, 0x00));
    REQUIRE_FALSE(mth::train_destination_blocked(0x63, 0x10));

    // Non-ticket codes are never blocked (100 = Exit/cancel sentinel; a normal item).
    REQUIRE_FALSE(mth::train_destination_blocked(100, 0x00));
    REQUIRE_FALSE(mth::train_destination_blocked(0x5e, 0x00));
    REQUIRE_FALSE(mth::train_destination_blocked(41, 0x00));
}

TEST_CASE("is_train_pass_machine_grant matches only the donation machine's dispense", "[train]")
{
    REQUIRE(mth::is_train_pass_machine_grant(149, 0x5e));

    REQUIRE_FALSE(mth::is_train_pass_machine_grant(-1, 0x5e));
    REQUIRE_FALSE(mth::is_train_pass_machine_grant(149, 0x5f));
    REQUIRE_FALSE(mth::is_train_pass_machine_grant(148, 0x5e));
    REQUIRE_FALSE(mth::is_train_pass_machine_grant(149, 41));
}

TEST_CASE("ticket_machine_seed pre-pays the donation goal down to the configured cost", "[train]")
{
    // Seed + cost equals the compiled-in goal, so the player owes exactly the cost.
    REQUIRE(mth::ticket_machine_seed(1000) == 9000u);
    REQUIRE(mth::ticket_machine_seed(2500) == 7500u);
    REQUIRE(mth::ticket_machine_seed(1) == 9999u);

    // Vanilla cost (or more) seeds nothing.
    REQUIRE(mth::ticket_machine_seed(10000) == 0u);
    REQUIRE(mth::ticket_machine_seed(50000) == 0u);

    // A free donation would complete the machine before the player opened it, so zero and negatives floor at one bone owing.
    REQUIRE(mth::ticket_machine_seed(0) == 9999u);
    REQUIRE(mth::ticket_machine_seed(-1) == 9999u);
}

TEST_CASE("maintained_vial_held preserves the missing flask count across a capacity change", "[upgrade]")
{
    REQUIRE(mth::maintained_vial_held(3, 3, 1) == 1);
    REQUIRE(mth::maintained_vial_held(3, 3, 5) == 5);
    REQUIRE(mth::maintained_vial_held(2, 0, 3) == 1);
    REQUIRE(mth::maintained_vial_held(1, 1, 1) == 1);
    REQUIRE(mth::maintained_vial_held(3, 1, 0) == 0);
    REQUIRE(mth::maintained_vial_held(0, 0, 4) == 4);
}

TEST_CASE("astral platform items decode to the game's own color order", "[items]")
{
    // apworld order is green, red, blue, yellow, purple; the game's Color property is yellow, green, blue, purple, red.
    REQUIRE(mth::astral_platform_color(4000) == 1);
    REQUIRE(mth::astral_platform_color(4001) == 4);
    REQUIRE(mth::astral_platform_color(4002) == 2);
    REQUIRE(mth::astral_platform_color(4003) == 0);
    REQUIRE(mth::astral_platform_color(4004) == 3);

    REQUIRE(mth::is_astral_platform_item(4000));
    REQUIRE(mth::is_astral_platform_item(4004));
    REQUIRE_FALSE(mth::is_astral_platform_item(3999));
    REQUIRE_FALSE(mth::is_astral_platform_item(4005));
}
