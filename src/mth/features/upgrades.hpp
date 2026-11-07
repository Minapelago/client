#pragma once

#include <functional>

namespace mth
{

// twin: mth/core/rules/progression.hpp (counts received capacity upgrades).
// Sets the low counts[i] bits of each per-type upgrade field (Magic, Health, Spark, Vial, Trinket; pre-clamped, kUpgradeCount entries) on the active save, then
// calls Player::UpdateStats to recompute maxima. Game thread only; false if unavailable or player is null (caller retries).
bool apply_upgrades(const int *counts, void *player);

// Hooks SaveSlot::Clear (new-file creation only) to zero the default region-18 upgrade fields while should_suppress() is true, so AP supplies the starting kit.
// false if unresolved.
bool install_newfile_kit_suppressor(std::function<bool()> should_suppress);
void remove_newfile_kit_suppressor();

} // namespace mth
