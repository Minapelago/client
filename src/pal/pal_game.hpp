#pragma once

#include <cstdint>
#include <functional>

#include "mth/core/game/game_symbols.hpp"
#include "pal/pal_layout.hpp"
#include "pal/pal_mem.hpp"
#include "pal/pal_module.hpp"

// Game-side pieces pal provides: the shared g_saveManager accessors and the detours whose code differs per build (pal/*/game_detours_*.cpp). Offsets that
// differ only in value live in pal_layout.hpp.
namespace pal
{

// g_saveManager, resolved on the first call. A miss is cached too: the symbol table never changes, and on Windows each resolve is a full .text scan.
inline std::uintptr_t save_manager_global()
{
    static const std::uintptr_t global = resolve_game_symbol(mth::sym::save_manager);
    return global;
}

// Active SaveSlot* through g_saveManager (see layout::kSaveManagerLiveSlotOff). Null when the global is unresolved or the slot is implausible, since title
// and menu init leave it uninitialized.
inline void *active_save_slot()
{
    const std::uintptr_t mgr = save_manager_global();
    if (mgr == 0)
        return nullptr;
    void *slot = at<void *>(mgr, layout::kSaveManagerLiveSlotOff);
    return pointer_looks_valid(slot) ? slot : nullptr;
}

// Shop-purchase callback: runs the AP collect logic for the bought slot and returns the itemType the game stores (a dummy where the platform redirects to
// suppress the vanilla grant, else the original).
using ShopBuyFn = int (*)(int loc_idx, int item_type);

// Shop-purchase detection. The platform owns the hooked function (ShopMenu::ItemPresent on Linux, ShopMenu::InitState on Windows), the field layout and
// commit detection, and calls on_buy on a committed buy. False if not installed.
bool install_shop_purchase_hook(ShopBuyFn on_buy);

// ---- Per-stat level cap. Linux detours the cap function; Windows wraps LevelUpMenu::Update and
// presents an at-cap stat as maxed. ----

// True once the platform's level-cap symbols resolve (Linux: LevelUpMenu::UpdateState and CombatData::GetNewGameMaxLevelPlayer; Windows: g_saveManager
// and LevelUpMenu::Update). Cached on first call.
bool level_cap_available();

// `cap(stat, vanilla_cap)` runs on the game thread in the LevelUpMenu buy gate and returns the cap for `stat` (0=attack, 1=defense, 2=sidearm), or
// vanilla_cap for none. `after_update` gets the menu at the end of each LevelUpMenu update, after the platform's cap work; null skips it.
using LevelCapFn = std::function<int(int stat, int vanilla_cap)>;
using LevelUpMenuFn = void (*)(void *level_up_menu);
void set_level_cap_provider(LevelCapFn cap, LevelUpMenuFn after_update);

} // namespace pal
