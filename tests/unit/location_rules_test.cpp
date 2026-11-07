#include <catch2/catch_test_macros.hpp>

#include "mth/core/game/game_tables.hpp"

// Storage kinds 1/9/11 keep collected state as a global per-itemType have-item bit, so an out-of-order AP grant of the vanilla item marks the
// location collected and the chest spawns open (#61). Those AP locations report the AP checked-state instead.
TEST_CASE("is_item_keyed_collected_kind: only the global have-item-bit kinds alias item ownership", "[game_tables]")
{
    CHECK(mth::tables::is_item_keyed_collected_kind(1));
    CHECK(mth::tables::is_item_keyed_collected_kind(9));
    CHECK(mth::tables::is_item_keyed_collected_kind(11));

    CHECK_FALSE(mth::tables::is_item_keyed_collected_kind(8));
    CHECK_FALSE(mth::tables::is_item_keyed_collected_kind(12));
    CHECK_FALSE(mth::tables::is_item_keyed_collected_kind(17));
    CHECK_FALSE(mth::tables::is_item_keyed_collected_kind(19));
    CHECK_FALSE(mth::tables::is_item_keyed_collected_kind(0));
}

// The vest and damage armor apply inside Items::OnPickup, before the hooked OnPickupDone, so the OnPickup detour suppresses them at AP locations (#71).
TEST_CASE("is_armor_upgrade_itemtype: vest and damage armor only", "[game_tables]")
{
    CHECK(mth::tables::is_armor_upgrade_itemtype(0x4f));
    CHECK(mth::tables::is_armor_upgrade_itemtype(0x50));
    CHECK_FALSE(mth::tables::is_armor_upgrade_itemtype(0x4e));
    CHECK_FALSE(mth::tables::is_armor_upgrade_itemtype(0x51));
    CHECK_FALSE(mth::tables::is_armor_upgrade_itemtype(0));
}

TEST_CASE("should_redirect_collected_query: capacity and item-keyed locations redirect, others pass through (#8/#61)", "[game_tables]")
{
    CHECK(mth::tables::should_redirect_collected_query(/*is_capacity=*/true, /*kind=*/9, /*ownership=*/false));
    CHECK(mth::tables::should_redirect_collected_query(false, 1, false));
    CHECK(mth::tables::should_redirect_collected_query(false, 9, false));
    CHECK(mth::tables::should_redirect_collected_query(false, 11, false));

    CHECK_FALSE(mth::tables::should_redirect_collected_query(false, 0, false));
    CHECK_FALSE(mth::tables::should_redirect_collected_query(false, 8, false));
    CHECK_FALSE(mth::tables::should_redirect_collected_query(false, 12, true));
    CHECK_FALSE(mth::tables::should_redirect_collected_query(false, 19, false));
}

// The weapon-swap chest reads ownership with param5 set and needs the real have-item bit, or a weapon received from another player is hidden from it.
TEST_CASE("should_redirect_collected_query: an ownership query passes through only on a weapon location", "[game_tables]")
{
    CHECK_FALSE(mth::tables::should_redirect_collected_query(/*is_capacity=*/false, /*kind=*/1, /*ownership=*/true));
    CHECK(mth::tables::should_redirect_collected_query(false, 9, true));
    CHECK(mth::tables::should_redirect_collected_query(false, 11, true));
    CHECK(mth::tables::should_redirect_collected_query(true, 9, true));
}

// Usable keys = popcount(collected bits) - spent, and under kear_rando it must stay 0. Load rebuilds the collected bits but not the spent bump, so
// reconciliation raises spent to popcount and never lowers it.
TEST_CASE("kear_reconciled_spent: raises spent to popcount, never lowers it", "[game_tables]")
{
    CHECK(mth::tables::kear_reconciled_spent(0b1u, 0) == 1);
    CHECK(mth::tables::kear_reconciled_spent(0b1011u, 1) == 3);
    CHECK(mth::tables::kear_reconciled_spent(0b111u, 3) == 3);
    CHECK(mth::tables::kear_reconciled_spent(0b1u, 3) == 3);
    CHECK(mth::tables::kear_reconciled_spent(0u, 2) == 2);
}

// Vanilla Pickup::Init stores this pair over an already-collected small treasure, so the Pickup offset self-check must not score it as drift (#148).
TEST_CASE("is_small_treasure_collect_rewrite: only the vanilla collected-treasure pair is not drift", "[game_tables]")
{
    CHECK(mth::tables::is_small_treasure_collect_rewrite(-1, 40));
    CHECK_FALSE(mth::tables::is_small_treasure_collect_rewrite(-1, 51));
    CHECK_FALSE(mth::tables::is_small_treasure_collect_rewrite(172, 40));
}

// The chosen starter remaps its tier-3 collection slot to the whip's, so a boat-hold chest reports a slot no seed contains; -1 restores identity.
TEST_CASE("should_clear_starter_swap: clears a swap only on the bound AP save", "[game_tables]")
{
    CHECK(mth::tables::should_clear_starter_swap(/*authed=*/true, /*slot_ok=*/true, /*current=*/2));
    CHECK(mth::tables::should_clear_starter_swap(true, true, 0));
    CHECK_FALSE(mth::tables::should_clear_starter_swap(true, true, -1));
    CHECK_FALSE(mth::tables::should_clear_starter_swap(false, true, 2));
    CHECK_FALSE(mth::tables::should_clear_starter_swap(true, false, 2));
}

// SaveSlot+0xc38[fam] is a bit index into the +0xc24[fam] ownership bitfield (the game revokes with `0xc24[fam] &= ~(1 << (0xc38[fam] & 0x1f))`).
TEST_CASE("weapon_active_bit: the companion field is the top authorized bit index", "[game_tables]")
{
    CHECK(mth::tables::weapon_active_bit(0b001u) == 0);
    CHECK(mth::tables::weapon_active_bit(0b011u) == 1);
    CHECK(mth::tables::weapon_active_bit(0b100u) == 2);
    CHECK(mth::tables::weapon_active_bit(0u) == 0);
}

// The clamp is destructive, and an empty authorized mask over a save that owns weapons looks like a receipt list that has not loaded yet.
TEST_CASE("weapon_clamp_ready: refuses to revoke everything on an empty receipt list", "[game_tables]")
{
    CHECK_FALSE(mth::tables::weapon_clamp_ready(/*authorized_any=*/0u, /*owned_any=*/0b100u));
    CHECK(mth::tables::weapon_clamp_ready(0b001u, 0b100u));
    CHECK(mth::tables::weapon_clamp_ready(0u, 0u));
}

TEST_CASE("weapon_fields_in_domain: a read wider than three tiers is not the weapon pair", "[game_tables]")
{
    CHECK(mth::tables::weapon_fields_in_domain(/*owned=*/0b000u, /*active=*/0));
    CHECK(mth::tables::weapon_fields_in_domain(0b111u, 2));
    CHECK_FALSE(mth::tables::weapon_fields_in_domain(0b1000u, 0));
    CHECK_FALSE(mth::tables::weapon_fields_in_domain(0xffffffffu, 0));
    CHECK_FALSE(mth::tables::weapon_fields_in_domain(0b001u, 3));
    CHECK_FALSE(mth::tables::weapon_fields_in_domain(0b001u, -1));
}

TEST_CASE("warp_resolved_slot: a scan match resolves to its remap target, or itself without one", "[game_tables]")
{
    CHECK(mth::tables::warp_resolved_slot(/*matched=*/17, /*warp=*/-1) == 17);
    CHECK(mth::tables::warp_resolved_slot(360, -2) == 360);
    CHECK(mth::tables::warp_resolved_slot(17, 42) == 42);
    CHECK(mth::tables::warp_resolved_slot(3, 0) == 0);
    CHECK(mth::tables::warp_resolved_slot(-1, -1) == -1);
    CHECK(mth::tables::warp_resolved_slot(-1, 42) == -1);
}

TEST_CASE("collection bit index: 0xff is the table's no-bit sentinel, and only < 64 may be shifted", "[tables][gate]")
{
    // Rows 24 and 25 of the shipping table carry 0xff, meaning "no unlock bit".
    REQUIRE(mth::tables::collection_bit_plausible(0));
    REQUIRE(mth::tables::collection_bit_plausible(63));
    REQUIRE(mth::tables::collection_bit_plausible(mth::tables::kNoCollectionBit));
    REQUIRE_FALSE(mth::tables::collection_bit_plausible(64));
    REQUIRE_FALSE(mth::tables::collection_bit_plausible(254));

    // seed_removed_locks does `1ull << bit`; on x86 a shift by 255 wraps to bit 63 and unlocks an unrelated lock.
    REQUIRE(mth::tables::collection_bit_usable(63));
    REQUIRE_FALSE(mth::tables::collection_bit_usable(64));
    REQUIRE_FALSE(mth::tables::collection_bit_usable(mth::tables::kNoCollectionBit));
}
