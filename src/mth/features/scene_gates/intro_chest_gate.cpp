#include "mth/features/scene_gates/intro_chest_gate.hpp"

#include <cstddef>
#include <cstdint>

#include "mod/mod_api.hpp"
#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/ap/ap_state.hpp"
#include "mth/core/game/component_types.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/features/scene_gates/scene_walk.hpp"
#include "mth/features/weapon_grants.hpp"
#include "pal/pal_log.hpp"

namespace
{

using mth::looks_like_component;

} // namespace

namespace mth
{

// The seed precollects the starting weapon, but the intro still hands one out (the chest's pick, and the fallback that force-grants the whip), writing SaveSlot
// directly and bypassing the Items::OnPickupDone suppression. So ownership is clamped to the AP grants each tick.
void IntroChestGate::enforce_weapon_grants(const ApState &state, bool authed, bool slot_ok)
{
    WeaponTally tally;
    for (const auto &it : state.received_items())
        tally.add(it.item_id);

    std::uint32_t authorized[kWeaponFamilyCount]{};
    for (int fam = 0; fam < kWeaponFamilyCount; ++fam)
        authorized[fam] = tally.owned_mask(fam);
    enforce_weapon_ownership(authorized, authed, slot_ok);
    // The chest still offers all three starters and equips the pick, so it is demoted too. Weapon-change mode lists owned weapons only, so demoting before a
    // grant lands, or on a save AP does not own, leaves nothing to arm.
    armed_ = authed && slot_ok && any_weapon_authorized(authorized);
}

// Writes only on a real transition, so a demotion logs once and an NPC re-arm logs again. The NPC writes the menu's copy of the byte directly, so an open menu
// is cleared too.
void IntroChestGate::demote_chest(void *chest)
{
    auto *starter = reinterpret_cast<std::uint8_t *>(static_cast<char *>(chest) + mth::layout::kCheckpointChestStarterOff);
    void *menu = *reinterpret_cast<void **>(static_cast<char *>(chest) + mth::layout::kCheckpointChestMenuOff);
    std::uint8_t *menu_starter = nullptr;
    if (looks_like_component(menu) && mod::component_isa(menu, mth::rtti::kWeaponsChestMenu))
        menu_starter = reinterpret_cast<std::uint8_t *>(static_cast<char *>(menu) + mth::layout::kWeaponsChestMenuStarterOff);

    if (*starter == 0 && (menu_starter == nullptr || *menu_starter == 0))
        return; // pure read for every chest outside the intro

    const unsigned prev_chest = *starter;
    const unsigned prev_menu = menu_starter != nullptr ? *menu_starter : 0u;
    *starter = 0;
    if (menu_starter != nullptr)
        *menu_starter = 0;
    const pal::LogLevel level = logged_demoted_ ? pal::LogLevel::Debug : pal::LogLevel::Info;
    logged_demoted_ = true;
    pal::logf(level, "intro: weapon chest %p demoted to weapon-change mode (chest starter %u -> 0, menu %p starter %u -> 0)", chest, prev_chest, menu,
              prev_menu);
}

void IntroChestGate::tick()
{
    if (!armed_)
        return;
    void *root = walker_.due_root();
    if (root == nullptr)
        return;

    // No early exit: a checkpoint room has a trinket chest and a weapon chest, and only one still in starter mode is written.
    const SceneWalk walk = walker_.for_each(root, rtti::kCheckpointChest, [&](void *c) { demote_chest(c); });
    walker_.report(walk);
}

void IntroChestGate::on_world_destroy()
{
    walker_.reset_cadence();
}

} // namespace mth
