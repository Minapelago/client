#pragma once

#include <cstdint>

namespace mth
{

// Clears the chosen-starter-weapon field so Weapons::GetStarterReplacement stops remapping that weapon's collection slot (should_clear_starter_swap decides).
// Bound AP save only, game thread only.
void clear_starter_weapon_swap(bool authed, bool slot_ok);

// Clamps the per-family weapon ownership bitfield to the AP-granted tiers and keeps the active-tier field in step. `authorized` has kWeaponFamilyCount entries.
// Writes only on the bound AP save and a real difference, and refuses an all-empty mask over a save that owns weapons (weapon_clamp_ready). Game thread only.
void enforce_weapon_ownership(const std::uint32_t *authorized, bool authed, bool slot_ok);

} // namespace mth
