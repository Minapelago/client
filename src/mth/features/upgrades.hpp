#pragma once

#include <functional>

namespace mth
{

class ApState;
class UpgradeState;

// twin: mth/core/rules/progression.hpp (counts received capacity upgrades).
// Sets the low counts[i] bits of each per-type upgrade field (Magic, Health, Spark, Vial, Trinket; pre-clamped, kUpgradeCount entries) on the active save, then
// calls Player::UpdateStats to recompute maxima. Game thread only; false if unavailable or player is null (caller retries).
bool apply_upgrades(const int *counts, void *player);

// Game thread, each tick. Refreshes the counts from the received stream and applies them to the live player while they are dirty.
void tick_upgrades(UpgradeState &upgrades, const ApState &state);

// Game thread, each tick. Re-asserts the AP vial count through the mod-API accessors (#171); no-op unless authenticated.
void enforce_vial_capacity(const UpgradeState &upgrades, const ApState &state);

// Game thread, each tick. Clamps live bones to the AP wallet cap (#112); no-op unless authenticated with slot_data wallet_cap set.
void enforce_wallet_cap(const ApState &state);

// Hooks SaveSlot::Clear (new-file creation only) to zero the default region-18 upgrade fields while should_suppress() is true, so AP supplies the starting kit.
// false if unresolved.
bool install_newfile_kit_suppressor(std::function<bool()> should_suppress);

} // namespace mth
