#pragma once

#include <cstddef>
#include <cstring>

#include "mth/core/game/game_symbols.hpp"

namespace mth::sym
{

// GetSymAddr takes the game's plain source-level name (the other paths key off the mangled one). Only names the API documents as supported belong
// here; unlisted ones fall through to the platform resolver. The game supplies these addresses, so they can't drift.
struct NativeSymName
{
    const char *mangled;
    const char *plain;
};

inline constexpr NativeSymName kNativeSymNames[] = {
    {s_r_items, "s_rItems"},
    {s_r_item_collection, "s_rItemCollection"},
    {pickup_init, "Pickup::Init"},
    {shop_init_state, "ShopMenu::InitState"},
    {shop_is_out_of_stock, "Shop::IsOutOfStock"},
    {shop_get, "Shop::Get"},
    {shop_set_cursor, "ShopMenu::SetCursor"},
    {activate_save_slot, "SessionManager::ActivateSaveSlot"},
    {toggle_cheat, "CheatManager::ToggleCheat"},
    {set_cheat_applied, "CheatManager::SetCheatApplied"},
    {level_up_menu_update, "LevelUpMenu::Update"},
    {player_set_burrow_ground, "Player::SetBurrowGround"},
    {player_rope_climb_start, "Player::RopeClimbStart"},
    {bounce_plant_collide, "BouncePlant::CollideWith"},
    {bounce_plant_launch, "BouncePlant::BounceLaunch"},
    {spring_bellows_collide, "SpringBellows::CollideWith"},
    {player_pickup_carryable, "Player::PickUpAnyNearbyCarryableObject"},
    {mina_on_burrow_jump, "Mina::OnBurrowJump"},
    {train_authority_on_npc_event, "TrainAuthority::OnNPCEvent"},
    {pawn_shop_on_npc_event, "PawnShopNPC::OnNPCEvent"},
    {hub_fountain_bulb_update, "HubFountain::Bulb::Update"},
};

// Mangled symbol -> a mod hook dispatched from inside that function. The hook name's hash is inlined at the dispatch site, so it anchors the
// function without a carved signature. Add an entry whenever the mod needs the address, detours included: Game::FixedUpdate is anchored but
// still detoured, because the hook fires at the top and the tick must run after the original.
struct HookAnchor
{
    const char *mangled;
    const char *hook_name;
};

inline constexpr HookAnchor kHookAnchors[] = {
    // Its carved signature broke on r149150. The API has no grant-by-type call, so the address is needed to call it.
    {on_pickup_done, "ItemsOnPickupDone"},
    {game_fixed_update, "FixedUpdate"},
};

namespace detail
{
// The `value` field of the row whose mangled name matches, or nullptr.
template <typename Row, std::size_t N> [[nodiscard]] const char *find_by_mangled(const Row (&table)[N], const char *Row::*value, const char *mangled) noexcept
{
    if (mangled == nullptr)
        return nullptr;
    for (const Row &row : table)
        if (std::strcmp(row.mangled, mangled) == 0)
            return row.*value;
    return nullptr;
}
} // namespace detail

// Returns the hook name anchoring a mangled symbol, or nullptr when there is none.
[[nodiscard]] inline const char *hook_anchor_for(const char *mangled) noexcept
{
    return detail::find_by_mangled(kHookAnchors, &HookAnchor::hook_name, mangled);
}

// Returns the GetSymAddr name for a mangled symbol, or nullptr when the API does not expose it.
[[nodiscard]] inline const char *native_sym_name(const char *mangled) noexcept
{
    return detail::find_by_mangled(kNativeSymNames, &NativeSymName::plain, mangled);
}

} // namespace mth::sym
