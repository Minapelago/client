#pragma once

#include <cstdint>
#include <functional>

#include "mth/core/data/game_symbols.hpp"
#include "pal/pal_layout.hpp"
#include "pal/pal_mem.hpp"
#include "pal/pal_module.hpp"

// Game-side pieces that pal provides: the g_saveManager accessors shared by both builds, and the
// detours whose code differs per build (pal/linux/game_detours_linux.cpp and
// pal/windows/game_detours_windows.cpp). Offsets that only differ in value are in pal_layout.hpp.
namespace pal
{

// g_saveManager, resolved on the first call. A miss is kept too: the symbol table does not change, and
// on Windows each resolve is a full .text signature scan.
inline std::uintptr_t save_manager_global()
{
    static const std::uintptr_t global = resolve_game_symbol(mth::sym::save_manager);
    return global;
}

// Active SaveSlot* through g_saveManager (see layout::kSaveManagerLiveSlotOff). Null when the global did
// not resolve or the slot is not a plausible pointer: title and menu init leave it uninitialized, and
// every caller null-checks, so this fails closed.
inline void *active_save_slot()
{
    const std::uintptr_t mgr = save_manager_global();
    if (mgr == 0)
        return nullptr;
    void *slot = at<void *>(mgr, layout::kSaveManagerLiveSlotOff);
    return pointer_looks_valid(slot) ? slot : nullptr;
}

// Shop-purchase callback: given the bought slot, runs the AP collect logic and returns the itemType
// the game should store (a dummy to suppress the vanilla grant where the platform redirects, else
// the original itemType unchanged).
using ShopBuyFn = int (*)(int loc_idx, int item_type);

// Install the shop-purchase detection hook. The platform owns which game function it hooks
// (ShopMenu::ItemPresent on Linux, ShopMenu::InitState on Windows), the field layout, and the
// commit detection; on a committed buy it invokes on_buy. Returns false if not installed.
bool install_shop_purchase_hook(ShopBuyFn on_buy);
void remove_shop_purchase_hook();

// ---- Per-stat level cap. Linux detours the cap function; Windows wraps LevelUpMenu::Update and
// presents an at-cap stat as maxed. ----

// True once the platform's level-cap symbols resolve (Linux: LevelUpMenu::UpdateState and
// CombatData::GetNewGameMaxLevelPlayer; Windows: g_saveManager and LevelUpMenu::Update).
// Resolves + caches on first call. The feature no-ops if false.
bool level_cap_available();

// Install the level-cap hooks. `cap(stat, vanilla_cap)` runs on the game thread during the
// LevelUpMenu buy-gate and returns the cap to enforce for `stat` (0=attack,1=defense,2=sidearm);
// return vanilla_cap for "no restriction". `after_update` runs on the game thread at the end of each
// per-frame LevelUpMenu update with the menu, once the platform's own cap work is done; null skips it.
// No-op if unavailable.
using LevelCapFn = std::function<int(int stat, int vanilla_cap)>;
using LevelUpMenuFn = void (*)(void *level_up_menu);
void set_level_cap_provider(LevelCapFn cap, LevelUpMenuFn after_update);

// Remove the level-cap hooks and clear the callback. Called by the mth/ owner's destructor.
void remove_level_cap_hook();

} // namespace pal
