#include <catch2/catch_test_macros.hpp>

#include "mth/core/game/game_tables.hpp"

// is_item_keyed_collected_kind identifies storage kinds whose collected state is a global per-itemType have-item bit (kinds 1/9/11) instead of the
// location's own bit index (kinds 8/12/17/19). An out-of-order AP grant of the vanilla item then marks the location collected and the chest spawns
// open (#61), so those AP locations report the AP checked-state.

TEST_CASE("is_item_keyed_collected_kind: global have-item-bit kinds alias item ownership", "[game_tables]")
{
    CHECK(mth::tables::is_item_keyed_collected_kind(1));
    CHECK(mth::tables::is_item_keyed_collected_kind(9));
    CHECK(mth::tables::is_item_keyed_collected_kind(11));
}

TEST_CASE("is_item_keyed_collected_kind: location-bit-keyed kinds do not alias", "[game_tables]")
{
    CHECK_FALSE(mth::tables::is_item_keyed_collected_kind(8));
    CHECK_FALSE(mth::tables::is_item_keyed_collected_kind(12));
    CHECK_FALSE(mth::tables::is_item_keyed_collected_kind(17));
    CHECK_FALSE(mth::tables::is_item_keyed_collected_kind(19));
    CHECK_FALSE(mth::tables::is_item_keyed_collected_kind(0));
}

// is_armor_upgrade_itemtype flags the two itemTypes whose vanilla effect applies inside Items::OnPickup, before the hooked OnPickupDone, so the
// OnPickup detour suppresses them for AP locations (#71).
TEST_CASE("is_armor_upgrade_itemtype: vest and damage armor only", "[game_tables]")
{
    CHECK(mth::tables::is_armor_upgrade_itemtype(0x4f));
    CHECK(mth::tables::is_armor_upgrade_itemtype(0x50));
    CHECK_FALSE(mth::tables::is_armor_upgrade_itemtype(0x45));
    CHECK_FALSE(mth::tables::is_armor_upgrade_itemtype(0x4e));
    CHECK_FALSE(mth::tables::is_armor_upgrade_itemtype(0x51));
    CHECK_FALSE(mth::tables::is_armor_upgrade_itemtype(0));
}

// should_redirect_collected_query decides whether the IsItemCollected override redirects to the AP checked-state: yes for capacity-upgrade (#8) and
// item-keyed have-bit (#61) locations, no for an ownership query (param5/b5 = true) on a weapon-kind (1) location. The weapon-swap chest reads
// ownership that way and needs the real have-item bit, or a weapon received from another player is hidden from it.

TEST_CASE("should_redirect_collected_query: non-aliasing locations never redirect", "[game_tables]")
{
    // Kind 0 and location-bit-keyed kinds, not capacity: pass through regardless of query context.
    CHECK_FALSE(mth::tables::should_redirect_collected_query(/*is_capacity=*/false, /*kind=*/0, /*ownership=*/false));
    CHECK_FALSE(mth::tables::should_redirect_collected_query(false, 8, false));
    CHECK_FALSE(mth::tables::should_redirect_collected_query(false, 12, true));
    CHECK_FALSE(mth::tables::should_redirect_collected_query(false, 19, false));
}

TEST_CASE("should_redirect_collected_query: location-collected queries redirect (issues #8/#61)", "[game_tables]")
{
    CHECK(mth::tables::should_redirect_collected_query(/*is_capacity=*/true, /*kind=*/9, /*ownership=*/false));
    CHECK(mth::tables::should_redirect_collected_query(false, 1, false));
    CHECK(mth::tables::should_redirect_collected_query(false, 9, false));
    CHECK(mth::tables::should_redirect_collected_query(false, 11, false));
}

TEST_CASE("should_redirect_collected_query: ownership query on a weapon (kind 1) passes through", "[game_tables]")
{
    // The weapon-swap chest's ownership read must see the real have-item bit.
    CHECK_FALSE(mth::tables::should_redirect_collected_query(/*is_capacity=*/false, /*kind=*/1, /*ownership=*/true));
}

TEST_CASE("should_redirect_collected_query: ownership query on non-weapon kinds still redirects", "[game_tables]")
{
    // Only kind 1 has the weapon-swap chest; subweapons, trinkets and capacity keep the #61/#8/#48 behavior.
    CHECK(mth::tables::should_redirect_collected_query(false, 9, true));
    CHECK(mth::tables::should_redirect_collected_query(false, 11, true));
    CHECK(mth::tables::should_redirect_collected_query(true, 9, true));
}

// kear_reconciled_spent backs the reload-durable kear key cancel. Usable keys = popcount(collected bits) - spent, and under kear_rando it must stay
// 0. The collected bitfield is rebuilt on load but the collect-time spent bump is not, so spent lags and a free key leaks. Reconciliation raises
// spent up to popcount and never lowers it.
TEST_CASE("kear_reconciled_spent: raises spent to popcount when behind", "[game_tables]")
{
    CHECK(mth::tables::kear_reconciled_spent(0b1u, 0) == 1);
    CHECK(mth::tables::kear_reconciled_spent(0b1011u, 0) == 3);
    CHECK(mth::tables::kear_reconciled_spent(0b1011u, 1) == 3);
}

TEST_CASE("kear_reconciled_spent: never lowers spent and no-ops when balanced", "[game_tables]")
{
    CHECK(mth::tables::kear_reconciled_spent(0b111u, 3) == 3);
    CHECK(mth::tables::kear_reconciled_spent(0b1u, 3) == 3);
    CHECK(mth::tables::kear_reconciled_spent(0u, 2) == 2);
}

// is_small_treasure_collect_rewrite pins the loc_idx/itemType pair vanilla Pickup::Init stores over an already-collected small treasure, so the
// Pickup offset self-check does not score it as layout drift (#148).
TEST_CASE("is_small_treasure_collect_rewrite: the vanilla collected-treasure pair is not drift", "[game_tables]")
{
    CHECK(mth::tables::is_small_treasure_collect_rewrite(-1, 40));
}

TEST_CASE("is_small_treasure_collect_rewrite: anything else is real drift", "[game_tables]")
{
    CHECK_FALSE(mth::tables::is_small_treasure_collect_rewrite(-1, 51));
    CHECK_FALSE(mth::tables::is_small_treasure_collect_rewrite(172, 40));
}

// should_clear_starter_swap decides whether the per-tick starter-weapon-swap clear runs. The chosen starter makes Weapons::GetStarterReplacement
// remap its tier-3 collection slot to the whip's (16), so a boat-hold chest reports a slot no seed contains. -1 means "no swap" and restores vanilla
// identity. The field is durable, so it is written only to the bound AP save.
TEST_CASE("should_clear_starter_swap: a swapped bound AP save is cleared", "[game_tables]")
{
    CHECK(mth::tables::should_clear_starter_swap(/*authed=*/true, /*slot_ok=*/true, /*current=*/2));
    CHECK(mth::tables::should_clear_starter_swap(true, true, 0));
}

TEST_CASE("should_clear_starter_swap: an unswapped save is left alone", "[game_tables]")
{
    CHECK_FALSE(mth::tables::should_clear_starter_swap(/*authed=*/true, /*slot_ok=*/true, /*current=*/-1));
}

TEST_CASE("should_clear_starter_swap: never writes outside the bound AP save", "[game_tables]")
{
    CHECK_FALSE(mth::tables::should_clear_starter_swap(/*authed=*/false, /*slot_ok=*/true, /*current=*/2));
    CHECK_FALSE(mth::tables::should_clear_starter_swap(true, false, 2));
    CHECK_FALSE(mth::tables::should_clear_starter_swap(false, false, 2));
}

// SaveSlot+0xc38[fam] is a bit index into the +0xc24[fam] ownership bitfield, not a power level (the game revokes with
// `0xc24[fam] &= ~(1 << (0xc38[fam] & 0x1f))`). The top authorized bit is what the game's ordered grants leave there.
TEST_CASE("weapon_active_bit: the companion field is a bit index into the ownership mask", "[game_tables]")
{
    CHECK(mth::tables::weapon_active_bit(0b001u) == 0);
    CHECK(mth::tables::weapon_active_bit(0b011u) == 1);
    CHECK(mth::tables::weapon_active_bit(0b111u) == 2);
    CHECK(mth::tables::weapon_active_bit(0b100u) == 2);
    CHECK(mth::tables::weapon_active_bit(0u) == 0);
}

// The clamp is destructive, so it refuses the shape it cannot tell from a real revoke: an empty authorized mask over a save that owns weapons looks
// like a receipt list that has not loaded yet.
TEST_CASE("weapon_clamp_ready: refuses to revoke everything on an empty receipt list", "[game_tables]")
{
    CHECK_FALSE(mth::tables::weapon_clamp_ready(/*authorized_any=*/0u, /*owned_any=*/0b100u));
    CHECK(mth::tables::weapon_clamp_ready(0b001u, 0b100u));
    CHECK(mth::tables::weapon_clamp_ready(0u, 0u));
}

TEST_CASE("weapon_fields_in_domain: a read wider than three tiers is not the weapon pair", "[game_tables]")
{
    CHECK(mth::tables::weapon_fields_in_domain(/*owned=*/0b000u, /*active=*/0));
    CHECK(mth::tables::weapon_fields_in_domain(0b100u, 2));
    CHECK(mth::tables::weapon_fields_in_domain(0b111u, 2));
    CHECK_FALSE(mth::tables::weapon_fields_in_domain(0b1000u, 0));
    CHECK_FALSE(mth::tables::weapon_fields_in_domain(0xffffffffu, 0));
    CHECK_FALSE(mth::tables::weapon_fields_in_domain(0b001u, 3));
    CHECK_FALSE(mth::tables::weapon_fields_in_domain(0b001u, -1));
}

// warp_resolved_slot turns an s_rItemCollection name-scan result into the slot the unlock bit uses.

TEST_CASE("warp_resolved_slot: a row with no remap resolves to its own index", "[game_tables]")
{
    CHECK(mth::tables::warp_resolved_slot(/*matched=*/17, /*warp=*/-1) == 17);
    CHECK(mth::tables::warp_resolved_slot(0, -1) == 0);
    CHECK(mth::tables::warp_resolved_slot(360, -2) == 360);
}

TEST_CASE("warp_resolved_slot: a remapped row resolves to the remap target", "[game_tables]")
{
    CHECK(mth::tables::warp_resolved_slot(/*matched=*/17, /*warp=*/42) == 42);
    CHECK(mth::tables::warp_resolved_slot(3, 0) == 0);
}

TEST_CASE("warp_resolved_slot: an unmatched scan stays unresolved", "[game_tables]")
{
    CHECK(mth::tables::warp_resolved_slot(/*matched=*/-1, /*warp=*/-1) == -1);
    CHECK(mth::tables::warp_resolved_slot(-1, 42) == -1);
}

TEST_CASE("collection bit index: 0xff is the table's no-bit sentinel, not drift", "[tables][gate]")
{
    // Measured against the shipping table: rows 24 and 25 carry 0xff, meaning "no unlock bit". It is not corruption.
    REQUIRE(mth::tables::collection_bit_plausible(0));
    REQUIRE(mth::tables::collection_bit_plausible(1));
    REQUIRE(mth::tables::collection_bit_plausible(63));
    REQUIRE(mth::tables::collection_bit_plausible(mth::tables::kNoCollectionBit));

    // Anything else at or above 64 cannot be a bit position in a u64 field.
    REQUIRE_FALSE(mth::tables::collection_bit_plausible(64));
    REQUIRE_FALSE(mth::tables::collection_bit_plausible(100));
    REQUIRE_FALSE(mth::tables::collection_bit_plausible(254));
}

TEST_CASE("collection bit index: only < 64 may be shifted", "[tables][gate]")
{
    // seed_removed_locks does `1ull << bit`; on x86 a shift by 255 wraps to bit 63 and unlocks an unrelated lock, so the sentinel must never reach it.
    REQUIRE(mth::tables::collection_bit_usable(0));
    REQUIRE(mth::tables::collection_bit_usable(63));
    REQUIRE_FALSE(mth::tables::collection_bit_usable(64));
    REQUIRE_FALSE(mth::tables::collection_bit_usable(mth::tables::kNoCollectionBit));
}
