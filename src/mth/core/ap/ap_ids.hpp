#pragma once

#include <array>
#include <cstdint>

#include "mth/core/game/game_layout.hpp"

namespace mth
{

//   ITEM space                          LOCATION space
//     0  [0..999]     vanilla itemType    0  [0..999]     collection slots (kLocBase + slot)
//     1  [1000..1999] progressive         1  [1000..1999] boss defeats (kBossLocBase + index)
//     2  [2000..2499] kear blocks         2+ reserved
//     2  [2500..2999] area kear locks
//     3  [3000..3999] abilities
//     4  [4000..4999] blockers
//     5  [5000..5999] traps
//     6+ reserved

// Vanilla game items
// ap item id = kItemBase + game itemType (0..194; 0 = engine "None", reserved).
inline constexpr std::int64_t kItemBase = 0;

inline constexpr std::int64_t ap_item_id(int item_type)
{
    return kItemBase + item_type;
}

inline constexpr int game_item_type(std::int64_t ap_item_id_)
{
    return static_cast<int>(ap_item_id_ - kItemBase);
}

// Whether a type is a real s_rItems row. The vanilla segment is 1000 ids wide but the table has kItemTypeCount rows, so granting an id in the
// tail would index out of bounds.
[[nodiscard]] inline constexpr bool is_valid_item_type(int item_type) noexcept
{
    return item_type >= 0 && item_type < layout::kItemTypeCount;
}

// Capacity upgrades: vanilla itemTypes 68..72 (Magic, Health, Spark, Vial, Trinket), stored as a
// popcount of owned bits per type, so OnPickupDone can't stack them. UpgradeState counts receipts and
// apply_upgrades (mth/features/upgrades.cpp) sets that many bits; the granter skips them. Cap = vanilla location count.
inline constexpr int kUpgradeItemBase = 68;
inline constexpr int kUpgradeCount = 5;
inline constexpr int kUpgradeCaps[kUpgradeCount] = {10, 18, 4, 10, 6}; // Magic, Health, Spark, Vial, Trinket (full-game counts)
inline constexpr int kVialUpgradeIndex = 3;                            // Vial's slot in the index order above

// SaveSlot bitfield value granting `count` of capacity-upgrade `upgrade_index` (low `count` bits; popcount == capacity). Vials overwrite `current`:
// the vanilla new file seeds a 3-bit vial base an OR can't clear (#83), and the field holds only vials.
[[nodiscard]] inline constexpr std::uint32_t upgrade_field_value(int upgrade_index, int count, std::uint32_t current) noexcept
{
    const std::uint32_t bits = count <= 0 ? 0u : (count >= 32 ? 0xFFFFFFFFu : (1u << count) - 1u);
    return upgrade_index == kVialUpgradeIndex ? bits : (current | bits);
}

// Plausibility bound for a capacity-upgrade SaveSlot field read before we write it: a valid field is a low-bit mask of at most kUpgradeCaps[index]
// bits. A larger read means the offset drifted and the caller fails closed. An apworld over-grant also exceeds it, so the next resend disables
// upgrades for the session.
[[nodiscard]] inline constexpr bool upgrade_field_in_domain(int upgrade_index, std::uint32_t value) noexcept
{
    const int cap = (upgrade_index >= 0 && upgrade_index < kUpgradeCount) ? kUpgradeCaps[upgrade_index] : 0;
    const std::uint32_t max = cap >= 32 ? 0xFFFFFFFFu : (1u << cap) - 1u;
    return value <= max;
}

// New held-vial count keeping the missing amount (old_max - old_held) across a capacity change, clamped to [0, new_max]: a capacity grant raises
// the ceiling without refilling, and a full player stays full. Used by PlayerSetMaxVials/PlayerSetVials.
[[nodiscard]] inline constexpr int maintained_vial_held(int old_max, int old_held, int new_max) noexcept
{
    const int held = old_held + (new_max - old_max);
    return held < 0 ? 0 : (held > new_max ? new_max : held);
}

inline constexpr bool is_capacity_upgrade_item(std::int64_t ap_item_id_)
{
    return ap_item_id_ >= kUpgradeItemBase && ap_item_id_ < kUpgradeItemBase + kUpgradeCount;
}

inline constexpr int upgrade_index(std::int64_t ap_item_id_)
{
    return static_cast<int>(ap_item_id_ - kUpgradeItemBase); // valid only when is_capacity_upgrade_item()
}

// Train fast-travel: the 5 destinations are vanilla itemTypes 0x5f..0x63 (95..99, "tickets"). A SaveSlot unlocked-lines bitfield holds one bit per
// line (1 << (itemType - kTrainTicketItemTypeBase)). The game sets a bit just for visiting the station (#98), so the client clamps the field to the
// AP-granted tickets each frame.
inline constexpr int kTrainTicketItemTypeBase = 0x5f; // itemType 95 == train line 0
inline constexpr int kTrainLineCount = 5;

[[nodiscard]] inline constexpr bool is_train_ticket_item_type(int item_type) noexcept
{
    return item_type >= kTrainTicketItemTypeBase && item_type < kTrainTicketItemTypeBase + kTrainLineCount;
}

// The unlocked-lines bit for a train-ticket itemType, or 0 when item_type is not a ticket.
[[nodiscard]] inline constexpr std::uint32_t train_ticket_bit(int item_type) noexcept
{
    return is_train_ticket_item_type(item_type) ? (1u << (item_type - kTrainTicketItemTypeBase)) : 0u;
}

// True when a picked line (96-99) lacks its AP ticket. Line 0 (Ossex/HUB, 95) needs only the Train Pass, and non-ticket codes (100 Exit, 101
// locked box) never block.
[[nodiscard]] inline constexpr bool train_destination_blocked(int selected_code, std::uint32_t granted_mask) noexcept
{
    if (selected_code == kTrainTicketItemTypeBase) // 95 = Ossex/HUB, always rideable with the pass
        return false;
    const std::uint32_t bit = train_ticket_bit(selected_code);
    return bit != 0 && (granted_mask & bit) == 0;
}

// The generic Train Pass and the collection index the station's donation machine dispenses it at. 10000 donated bones make the machine call
// Items::OnPickup(kTrainPassLocIdx, kTrainPassItemType), the vanilla train unlock. The pass is an AP item and the location isn't in the apworld, so
// the client has to refuse that grant itself (#162).
inline constexpr int kTrainPassItemType = 0x5e; // 94
inline constexpr int kTrainPassLocIdx = 149;    // Ticket_Pass

[[nodiscard]] inline constexpr bool is_train_pass_machine_grant(int loc_idx, int item_type) noexcept
{
    return loc_idx == kTrainPassLocIdx && item_type == kTrainPassItemType;
}

// Whether Items::OnPickupDone should skip a vanilla grant. AP replays use slot -1 and world AP pickups carry the dummy itemType, so only a real item
// at an AP location is suppressed.
[[nodiscard]] inline constexpr bool should_suppress_vanilla_grant(int slot, int item_type, bool is_ap_location) noexcept
{
    return slot >= 0 && item_type != layout::kApDummyItemType && is_ap_location;
}

// Donating accumulates a SaveSlot counter; the machine completes once it passes kTicketMachineGoal (compiled in). Pre-seeding the counter with
// 10000-cost leaves exactly `cost` owing, and the game clamps the dial to what is still owed, so the player can't overpay (the deposit itself is
// unclamped). cost is clamped to a payable, non-free range.
inline constexpr unsigned kTicketMachineGoal = 10000;
inline constexpr int kTrainPassCostDefault = 1000;

[[nodiscard]] inline constexpr unsigned ticket_machine_seed(int cost) noexcept
{
    if (cost < 1)
        return kTicketMachineGoal - 1; // free would complete the donation before the player sees it
    if (static_cast<unsigned>(cost) >= kTicketMachineGoal)
        return 0; // vanilla cost: nothing to seed
    return kTicketMachineGoal - static_cast<unsigned>(cost);
}

// Progressive Items
// Count-based: the Nth receipt of a chain's id is "tier N". Sub-layout:
//   1000..1004  weapon families  (kProgWeaponBase + family)
//   1005..1007  per-stat cap-ups (kProgStatCapBase + stat)
//   1008        all-stat cap-up  (kProgStatCapAllId)
//   1009        fishing rod      (kProgFishingRodId)

inline constexpr std::int64_t kProgressiveItemBase = 1000;
inline constexpr int kStatCount = 3; // 0=attack, 1=defense, 2=sidearm/magic

// One chain per weapon family
inline constexpr std::int64_t kProgWeaponBase = kProgressiveItemBase;
inline constexpr int kWeaponFamilyCount = 5;
inline constexpr int kWeaponTiers = 3;
inline constexpr int kWeaponItemTypes[kWeaponFamilyCount][kWeaponTiers] = {
    {2, 3, 4},    // Whip
    {5, 6, 7},    // Hammer
    {8, 9, 10},   // Daggers
    {11, 12, 13}, // Buster Bat
    {14, 15, 16}, // Casket
};

inline constexpr bool is_weapon_item(std::int64_t ap_item_id_)
{
    return ap_item_id_ >= kProgWeaponBase && ap_item_id_ < kProgWeaponBase + kWeaponFamilyCount;
}

inline constexpr int weapon_family(std::int64_t ap_item_id_)
{
    return static_cast<int>(ap_item_id_ - kProgWeaponBase); // valid only when is_weapon_item()
}

// Engine itemType for family, -1 if out of range.
inline constexpr int weapon_itemtype(int family, int tier)
{
    if (family < 0 || family >= kWeaponFamilyCount || tier < 1 || tier > kWeaponTiers)
        return -1;
    return kWeaponItemTypes[family][tier - 1];
}

// Running per-family receipt count, shared by the granter (tier to hand out) and the ownership clamp (bits authorized). Both must count the same
// way or the clamp revokes a granted weapon.
struct WeaponTally
{
    int tiers[kWeaponFamilyCount]{};

    // Counts one received item and returns the tier it grants, 0 for a non-weapon id. Every receipt counts,
    // already-granted ones included, so the tier survives a reload.
    constexpr int add(std::int64_t ap_item_id_)
    {
        if (!is_weapon_item(ap_item_id_))
            return 0;
        return ++tiers[weapon_family(ap_item_id_)];
    }

    // SaveSlot ownership bits authorized for `family`. Tiers are granted in order and the engine bit for an itemType is (itemType - 2) % 3, so N
    // tiers is the low N bits. Saturates at the top tier, since nothing beyond it is ever granted.
    [[nodiscard]] constexpr std::uint32_t owned_mask(int family) const
    {
        if (family < 0 || family >= kWeaponFamilyCount)
            return 0;
        const int owned = tiers[family] < kWeaponTiers ? tiers[family] : kWeaponTiers;
        return (1u << owned) - 1u;
    }
};

// True once any family has an authorized tier (the clamp's masks). The intro chest gate arms on this: its equip-only mode lists owned weapons only,
// so demoting the chest before a grant lands leaves nothing to arm.
[[nodiscard]] constexpr bool any_weapon_authorized(const std::uint32_t (&authorized)[kWeaponFamilyCount])
{
    for (int fam = 0; fam < kWeaponFamilyCount; ++fam)
        if (authorized[fam] != 0)
            return true;
    return false;
}

inline constexpr std::int64_t kMapItem = kProgressiveItemBase + 10;
inline constexpr int kMapTiers = 2;
// world_map (75), enhanced_map (76) granted in tier order.
inline constexpr int kMapItemTypes[kMapTiers] = {75, 76};
inline constexpr bool is_map_item(std::int64_t ap_item_id_)
{
    return ap_item_id_ == kMapItem;
}

inline constexpr int map_itemtype(int tier)
{
    if (tier < 1 || tier > kMapTiers)
        return -1;
    return kMapItemTypes[tier - 1];
}

// Stat-cap cap-ups
// derived state (StatCapState), never granted as items. The all-stat id raises
// every stat per receipt.
inline constexpr std::int64_t kProgStatCapBase = kProgWeaponBase + kWeaponFamilyCount; // per-stat: +stat
inline constexpr std::int64_t kProgStatCapAllId = kProgStatCapBase + kStatCount;

inline constexpr bool is_stat_cap_all_item(std::int64_t ap_item_id_)
{
    return ap_item_id_ == kProgStatCapAllId;
}

inline constexpr bool is_stat_cap_item(std::int64_t ap_item_id_)
{
    return (ap_item_id_ >= kProgStatCapBase && ap_item_id_ < kProgStatCapBase + kStatCount) || is_stat_cap_all_item(ap_item_id_);
}

inline constexpr int stat_cap_item_stat(std::int64_t ap_item_id_)
{
    return static_cast<int>(ap_item_id_ - kProgStatCapBase); // valid only for a per-stat id
}

// Fishing rod: single progressive chain; the Nth receipt grants the Nth fishing-rod upgrade itemType.
inline constexpr std::int64_t kProgFishingRodId = kProgStatCapAllId + 1; // 1009
inline constexpr int kFishingRodTiers = 3;
// Upgrade_FishingRod (87), Upgrade_FishingUpgrade (88), Upgrade_FishingGold (89), granted in tier order.
inline constexpr int kFishingRodItemTypes[kFishingRodTiers] = {87, 88, 89};

inline constexpr bool is_fishing_rod_item(std::int64_t ap_item_id_)
{
    return ap_item_id_ == kProgFishingRodId;
}

// Wallet (bone-wallet) capacity: count-based derived state (see wallet_cap()), never granted as an
// item. Each receipt raises the enforced bone cap; the game itself is not told, the mod clamps bones.
inline constexpr std::int64_t kProgWalletId = kProgressiveItemBase + 11; // 1011

inline constexpr bool is_wallet_item(std::int64_t ap_item_id_)
{
    return ap_item_id_ == kProgWalletId;
}

// Engine itemType for a 1-based tier, -1 if out of range.
inline constexpr int fishing_rod_itemtype(int tier)
{
    if (tier < 1 || tier > kFishingRodTiers)
        return -1;
    return kFishingRodItemTypes[tier - 1];
}

// item-category bases
inline constexpr std::int64_t kKearBlockItemBase = 2000;    // kear-lock removals (wired)
inline constexpr std::int64_t kAreaKearLockItemBase = 2500; // kear-lock removals (wired)
inline constexpr std::int64_t kAbilityItemBase = 3000;      // ability gates (Ability enum; see ability_ids.hpp)
inline constexpr std::int64_t kBlockerItemBase = 4000;      // Mirrors End shortcut switches (#28)
inline constexpr std::int64_t kTrapItemBase = 5000;         // modifier traps (kTrapItemBase + modifier index)
inline constexpr std::int64_t kTrapSegmentWidth = 1000;

// The apworld's five "<Color> Astral Switch" items sit at the bottom of the blocker segment in the apworld's color order, which differs from the
// game's Color property. Index = id - kBlockerItemBase, value = the game color the switch reports.
inline constexpr int kAstralPlatformCount = 5;
inline constexpr std::array<int, kAstralPlatformCount> kAstralPlatformColors{
    1, // 4000 green  -> Nox's Bayou mirror
    4, // 4001 red    -> Bone Beach mirror
    2, // 4002 blue   -> Queensbury mirror
    0, // 4003 yellow -> Septemburg mirror
    3, // 4004 purple -> Coltrane Peak mirror
};

[[nodiscard]] inline constexpr bool is_astral_platform_item(std::int64_t id) noexcept
{
    return id >= kBlockerItemBase && id < kBlockerItemBase + kAstralPlatformCount;
}

// Valid only when is_astral_platform_item(). Returns a bit position, so a set of these ORs into a mask.
[[nodiscard]] inline constexpr int astral_platform_color(std::int64_t id) noexcept
{
    return kAstralPlatformColors[static_cast<std::size_t>(id - kBlockerItemBase)];
}

// A trap id carries the modifier index it forces on. The segment is a full 1000 wide, so an id in
// the tail decodes to an index with no trap behind it, and the table lookup rejects those.
[[nodiscard]] inline constexpr bool is_trap_item(std::int64_t id) noexcept
{
    return id >= kTrapItemBase && id < kTrapItemBase + kTrapSegmentWidth;
}

[[nodiscard]] inline constexpr int trap_modifier_index(std::int64_t id) noexcept
{
    return static_cast<int>(id - kTrapItemBase); // valid only when is_trap_item()
}

// special Constants
inline constexpr std::int64_t kMMFirstDoubleKearBlockID = 2304;
inline constexpr std::int64_t kMMSecondDoubleKearBlockID = 2306;

// Locks each area kear item removes (kear_rando=2), one row per lock: {area item id, kear-block item id}. The kear-block ids are also the
// kear_rando=1 per-lock items, except 2306, which item 2304 removes with its own lock.
struct AreaLock
{
    std::int64_t area_item_id;
    std::int64_t lock_item_id;
};

inline constexpr AreaLock kAreaLocks[] = {
    {2509, 2151}, {2509, 2152}, {2509, 2157},               // OS
    {2510, 2358}, {2510, 2359},                             // Train
    {2507, 2019}, {2507, 2020}, {2507, 2021}, {2507, 2022}, // LL
    {2515, 2260}, {2515, 2262},                             // SO
    {2505, 2225}, {2505, 2222}, {2505, 2224}, {2505, 2227}, // EH
    {2516, 2246}, {2516, 2247}, {2516, 2244},               // WW
    {2508, 2307},                                           // MM
    {2511, 2304}, {2511, 2306}, {2511, 2055},               // QB
    {2501, 2284}, {2501, 2283}, {2501, 2285},               // BW
    {2502, 2033},                                           // NB
    {2506, 2336}, {2506, 2338}, {2506, 2337},               // KW
    {2513, 2320}, {2513, 2321}, {2513, 2318},               // SF
    {2503, 2069},                                           // BB
    {2504, 2115}, {2504, 2109},                             // CTP
    {2500, 2277}, {2500, 2278}, {2500, 2127},               // AO
    {2514, 2096},                                           // SB
    {2512, 2141},                                           // RM
};

inline constexpr bool is_vanilla_game_item(std::int64_t ap_item_id_)
{
    return ap_item_id_ >= kItemBase && ap_item_id_ < kProgressiveItemBase;
}

// Universal Kear (vanilla kear mode): the pool carries this item and each receipt must grant one usable
// key. Its AP id is the kear itemType 0x3f (63) in the vanilla item segment.
inline constexpr std::int64_t kUniversalKearItemType = 0x3f;

inline constexpr bool is_vanilla_kear_item(std::int64_t ap_item_id_)
{
    return ap_item_id_ == ap_item_id(kUniversalKearItemType);
}

// Kear blocks
// id = kKearBlockItemBase + engine id
inline constexpr bool is_kear_block_item(std::int64_t ap_item_id_)
{
    return ap_item_id_ >= kKearBlockItemBase && ap_item_id_ < kAreaKearLockItemBase;
}
// Area Kear Locks: This pool is one item that acts like multiple kear locks.
inline constexpr bool is_area_lock_item(std::int64_t ap_item_id_)
{
    return ap_item_id_ >= kAreaKearLockItemBase && ap_item_id_ < kAbilityItemBase;
}

inline constexpr int kear_block_engine_id(std::int64_t ap_item_id_)
{
    return static_cast<int>(ap_item_id_ - kKearBlockItemBase); // valid only when is_kear_block_item()
}

// locations

// Item-collection pickup/shop slots: ap loc id = kLocBase + slot (location segment 0).
inline constexpr std::int64_t kLocBase = 0;

inline constexpr std::int64_t ap_loc_id(int collection_idx)
{
    return kLocBase + collection_idx;
}

// Legovich (NPCBehavior_WeaponMerchant) weapon-upgrade shop slots. Their AP-checked count drives the out-of-stock total that gates the Armand
// encounter, so when the shop asks, it counts purchases and ignores weapons from the multiworld (#67).
inline constexpr int kLegovichLocations[] = {174, 175, 176, 177, 178};

[[nodiscard]] inline constexpr bool is_legovich_location(int loc)
{
    for (int l : kLegovichLocations)
        if (l == loc)
            return true;
    return false;
}

// Boss-defeat locations occupy location segment 1.
inline constexpr int kBossLocBase = 1000;

// kBossInfo has 29 entries (positional indices 0x00..0x1C); GetBossIndex returns 0x00..0x1B in
// observed play but scans the whole table, so accept the full range.
inline constexpr int kMaxBossIndex = 0x1C;

// Precondition: 0 <= boss_index <= kMaxBossIndex.
[[nodiscard]] inline constexpr int boss_location_slot(int boss_index)
{
    return kBossLocBase + boss_index;
}

} // namespace mth
