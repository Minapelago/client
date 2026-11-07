#pragma once

#include <cstdint>
#include <functional>

#include "mth/core/data/ability_ids.hpp"

namespace mth
{

// The ability detours AbilityHooks drives: the movement chokepoints, the TrainAuthority::OnNPCEvent
// destination gate and the SetupBoxes patch. Everything here runs on the game thread unless noted.

// Gate predicate consulted by the detours.
using AbilityBlockFn = std::function<bool(Ability a)>;

// Install the ability detours; each suppresses its action when block(a) is true. No-op for unresolved
// symbols. Returns false if none installed.
bool install_ability_hooks(AbilityBlockFn block);
void remove_ability_hooks();

// Forces the train-present save byte to 0 while blocked; the arrival event re-sets it when
// unblocked. No-op if unavailable.
void enforce_train_presence(bool blocked);

// Requires the generic Train Pass (item 94) before the train can be boarded: forces the train-present byte
// to 0 while the pass is unowned, then releases it once received (#98). No-op if unavailable.
void enforce_train_boarding();

// Publishes the train_rando destination gate read by the OnNPCEvent detour. When rando_active, a selected
// ticket line is cancelled unless its bit is in granted_mask; when inactive the detour uses the console
// Train-ability block instead. Cheap; call each tick.
void set_train_destination_gate(std::uint32_t granted_mask, bool rando_active);

// Observers for the burrow lifetime (#163), invoked by the burrow detours: on_commit(deep) when a
// burrow/swim commit is allowed through (deep = it classified as a swim), on_emerge() when the burrow
// actually ends. Together they track which mode the player is in without reading the burrow-mode field,
// whose offset drifts between builds. Cleared by remove_ability_hooks().
using BurrowCommitFn = std::function<void(bool deep)>;
using BurrowEmergeFn = std::function<void()>;
void set_burrow_observers(BurrowCommitFn on_commit, BurrowEmergeFn on_emerge);

// Live swim-vs-land reading for `player`, the same discriminator the commit classifier uses: -1 unknown
// (null player, or it did not resolve), 0 shallow, 1 deep.
int burrow_water_state(void *player);

// Forces the burrow-emerge commit, surfacing the player. false if unavailable.
bool force_burrow_emerge(void *player);

} // namespace mth
