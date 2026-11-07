#pragma once

#include <functional>

namespace mth
{

// twin: mth/core/upgrade_state.hpp (counts received capacity upgrades).
// Apply per-type upgrade counts (index order Magic,Health,Spark,Vial,Trinket; already clamped to
// caps) to the active save: set that many low bits in each per-type field, then call
// Player::UpdateStats(player) to recompute live maxima. `counts` has kUpgradeCount entries.
// Game-thread only. false if unavailable or player is null (caller retries).
bool apply_upgrades(const int *counts, void *player);

// Hook SaveSlot::Clear (called only at new-file creation); when should_suppress() returns true, zero the
// default region-18 upgrade fields so AP supplies the starting inventory instead. false if unresolved.
bool install_newfile_kit_suppressor(std::function<bool()> should_suppress);
void remove_newfile_kit_suppressor();

} // namespace mth
