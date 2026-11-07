#pragma once

#include <bit>
#include <cstdint>

#include "mth/core/game/game_layout.hpp"

namespace mth::tables
{

// Vanilla Pickup::Init clears the loc_idx the offset self-check reads back: on a "ForceSmallTreasureCollect" spawn point whose location already
// reads collected, one 8-byte store (Pickup+0x380 = 0x28ffffffff: loc_idx -1, itemType 40 = kItemType_Treasure_Smallest) pays out small bones.
// AP locations hit this on the first room reload after their check is sent, so it must not count as offset drift (#148). Pure.
[[nodiscard]] constexpr bool is_small_treasure_collect_rewrite(int stored_loc_idx, int stored_item_type) noexcept
{
    return stored_loc_idx == -1 && stored_item_type == 40;
}

// Reload-durable kear key cancel. usable keys = popcount(SaveSlot+0x1f0) - SaveSlot+0x1f8, which must stay 0 under kear_rando. Reload rebuilds
// the collected bitfield but not the collect-time spent bump (neutralize_kear_grant), so a free key leaks. Raises spent up to popcount and never
// lowers it, so real lock-spends survive. Pure.
[[nodiscard]] constexpr int kear_reconciled_spent(std::uint64_t collected_bits, int spent) noexcept
{
    const int popcount = std::popcount(collected_bits);
    return popcount > spent ? popcount : spent;
}

// "Have-item bit" kinds (1=vessel/weapon tiers, 9=subweapon/spell/ability unlocks + capacity pieces, 11=trinkets): IsItemCollected keys these on
// item type, not the location's bit index, so owning the item (e.g. an out-of-order AP grant) reads the location as collected and the chest
// spawns open (#61). Such AP locations report the AP checked-state instead. Complement of is_durable_bit_kind within the durable families. Pure.
[[nodiscard]] constexpr bool is_item_keyed_collected_kind(int kind) noexcept
{
    return kind == 1 || kind == 9 || kind == 11;
}

// Whether the Items::IsItemCollected override redirects a query to the AP checked-state: yes for capacity-upgrade (#8) and item-keyed have-bit
// (#61) AP locations, except ownership queries (param5/b5 = true) on weapon-kind (1) locations. The weapon-swap chest (WeaponsChestMenu) lists
// owned weapons with b5=true, and a redirect would hide weapons received from other players. Location-collected queries (b5=false: chest-open,
// pickup self-kill, boss reward-rose) keep the redirect; kinds 9/11 are untouched (shop #48, trinkets). Pure.
[[nodiscard]] constexpr bool should_redirect_collected_query(bool is_capacity, int kind, bool ownership_query) noexcept
{
    if (!is_capacity && !is_item_keyed_collected_kind(kind))
        return false; // not an aliasing location
    if (ownership_query && kind == 1)
        return false; // weapon-swap chest ownership read -> real have-item bit
    return true;
}

// The picked starting weapon (SaveSlot+0xc60, -1 = none) makes Weapons::GetStarterReplacement return the whip's collection slot (16) for that
// weapon's tier-3 slot everywhere (pickups, shops, IsOutOfStock, FillChecklist, KeyBlock, WarpDoor). The apworld pins the two belowdecks weapon
// stands to fixed ids, so after a non-whip start one stand reports a slot no seed contains and the other id has no stand. Clearing it to -1 ("no
// swap") restores vanilla slot identity and leaves weapon ownership alone. Durable save field, hence the bound-save gate. Pure.
[[nodiscard]] constexpr bool should_clear_starter_swap(bool authed, bool slot_ok, int current) noexcept
{
    return authed && slot_ok && current != -1;
}

// Companion of the per-family weapon ownership bitfield (SaveSlot+0xc24): SaveSlot+0xc38 holds a bit index into it, not a tier count (the game
// revokes with `0xc24[fam] &= ~(1 << (0xc38[fam] & 0x1f))`, and its ordered grants leave the last tier there). The consistent value is the
// mask's top bit, or 0 for a family owning nothing. Pure.
[[nodiscard]] constexpr int weapon_active_bit(std::uint32_t owned_mask) noexcept
{
    return owned_mask == 0 ? 0 : std::bit_width(owned_mask) - 1;
}

// Plausibility of one family's durable weapon fields (SaveSlot+0xc24 owned bits, +0xc38 active tier), checked before the clamp rewrites all 40
// bytes. Three tiers exist, so a wider mask or a tier outside 0..2 means the offsets drifted. Pure.
[[nodiscard]] constexpr bool weapon_fields_in_domain(std::uint32_t owned, int active) noexcept
{
    return (owned & ~mth::layout::kWeaponTierBits) == 0 && active >= 0 && active < std::bit_width(mth::layout::kWeaponTierBits);
}

// Gate on the destructive weapon ownership clamp. An empty authorized mask over a save that owns weapons is what an unloaded receipt list looks
// like, and clamping would delete the weapons; the seed always precollects a starting weapon, so an empty mask is never real. Both arguments are
// the OR of every family's mask. Pure.
[[nodiscard]] constexpr bool weapon_clamp_ready(std::uint32_t authorized_any, std::uint32_t owned_any) noexcept
{
    return authorized_any != 0 || owned_any == 0;
}

// Armor upgrades (Vitality Vest 0x4f = +25% max HP, Damage armor 0x50) apply in Items::OnPickup itself (an OR into SaveSlot+0xc68 before the
// conditionally tail-called OnPickupDone), so suppressing OnPickupDone alone leaks the upgrade on an AP shop buy (#71). The OnPickup detour
// suppresses these for AP locations. Pure.
[[nodiscard]] constexpr bool is_armor_upgrade_itemtype(int item_type) noexcept
{
    return item_type == 0x4f || item_type == 0x50;
}

// Effective slot of a name-scan result: a matched row's warp-remap field redirects it to another slot,
// and <0 there means the matched index is itself the slot. Unmatched (<0) stays unresolved. Pure.
[[nodiscard]] constexpr int warp_resolved_slot(int matched_idx, int warp_remap) noexcept
{
    if (matched_idx < 0)
        return -1;
    return warp_remap < 0 ? matched_idx : warp_remap;
}

// s_rItemCollection's bit index is a u8 whose 0xff means "this location has no unlock bit"
// (rows 24 and 25 of the shipping table use it). Any other value is a bit position inside a
// SaveSlot u64, so >= 64 without being the sentinel means the field drifted.
inline constexpr std::uint8_t kNoCollectionBit = 0xff;

// Only these may be shifted into the SaveSlot u64. 1ull << 255 is UB and wraps to bit 63 on x86.
[[nodiscard]] constexpr bool collection_bit_usable(std::uint8_t bit)
{
    return bit < 64;
}

// Widens the above by the sentinel: what an intact table can legitimately hold.
[[nodiscard]] constexpr bool collection_bit_plausible(std::uint8_t bit)
{
    return bit < 64 || bit == kNoCollectionBit;
}

} // namespace mth::tables
