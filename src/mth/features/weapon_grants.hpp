#pragma once

#include <cstdint>

namespace mth
{

// Clears the chosen-starter-weapon field, so Weapons::GetStarterReplacement stops remapping that weapon's
// collection slot (see should_clear_starter_swap, which decides every write). Durable field: bound AP save
// only. No-op if unavailable. Game-thread only.
void clear_starter_weapon_swap(bool authed, bool slot_ok);

// Clamps the per-family weapon ownership bitfield to the tiers AP granted, and keeps the companion
// active-tier field consistent with it. `authorized` has kWeaponFamilyCount entries. Durable and
// destructive, so it writes only on the bound AP save, only on a real difference, and refuses an all-empty
// mask over a save that owns weapons (see weapon_clamp_ready). No-op if unavailable. Game-thread only.
void enforce_weapon_ownership(const std::uint32_t *authorized, bool authed, bool slot_ok);

} // namespace mth
