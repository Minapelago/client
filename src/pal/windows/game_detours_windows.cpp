#include <cstddef>
#include <cstdint>
#include <utility>

#include "mth/core/data/game_symbols.hpp"
#include "mth/core/shop_ui.hpp"
#include "mth/core/stat_cap_state.hpp"
#include "pal/pal_game.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"
#include "pal/pal_module.hpp"
#include "pal/scoped_hook.hpp"

namespace
{
// ShopMenu::InitState offsets: ItemPresent is inlined into InitState's buy-confirm state (7).
constexpr std::ptrdiff_t kShopStateOff = 0x2c; // int: state machine; 7 = buy-confirm
constexpr int kShopBuyConfirmState = 7;
constexpr std::ptrdiff_t kShopFlagsObjOff = 0x178; // -> ShopDef; flags at mth::kShopFlagsOff (bit 0x4 = pawn-sell)
constexpr std::ptrdiff_t kShopLocIdxOff = 0x1e0;   // int
constexpr std::ptrdiff_t kShopItemTypeOff = 0x1e4; // int

pal::ShopBuyFn g_on_shop_buy = nullptr;
[[clang::no_destroy]] pal::ScopedHook g_shop_hook;
void (*g_orig_init_state)(void *) = nullptr;
int g_shop_last_locidx = -1; // dedup: InitState is re-entered while in the buy-confirm state

// Run the original FIRST: state 7 writes locIdx/itemType partway through InitState, so they are only
// valid post-init. The grant has already run by then, so the vanilla item can't be redirected here; it
// is instead suppressed in the Items::OnPickupDone detour (which skips real grants at AP locations).
void repl_init_state(void *self)
{
    if (g_orig_init_state)
        g_orig_init_state(self);

    if (g_on_shop_buy != nullptr && self != nullptr)
    {
        const int state = pal::at<int>(self, kShopStateOff);
        if (state == kShopBuyConfirmState)
        {
            void *flags_obj = pal::at<void *>(self, kShopFlagsObjOff);
            const unsigned flags = flags_obj != nullptr ? pal::at<unsigned>(flags_obj, mth::kShopFlagsOff) : 0xFFFFFFFFu;
            const int loc_idx = pal::at<int>(self, kShopLocIdxOff);
            const int item_type = pal::at<int>(self, kShopItemTypeOff);

            // Informational: surfaces the bought slot for serverless testing.
            pal::logf(pal::LogLevel::Debug, "shop: buy-confirm locIdx=%d itemType=%d flags=0x%x", loc_idx, item_type, flags);

            // Entering state 7 is the confirmed-buy grant (deducts money + grants).
            // Skip pawn-sells (flags bit 0x4) and dedup the per-frame InitState re-entries.
            if ((flags & 0x4u) == 0 && loc_idx != g_shop_last_locidx)
            {
                g_shop_last_locidx = loc_idx;      // dedup this confirmed buy
                g_on_shop_buy(loc_idx, item_type); // return ignored: no itemType redirect on Windows
            }
        }
        else
        {
            g_shop_last_locidx = -1; // left the buy state; allow the next buy
        }
    }
}

// Items::IsItemCollected override lives in native_mod_entry.cpp (native modding hook; cross-platform).
// This also catches clang-cl's inlined copies of IsItemCollected (e.g. the Pickup-ctor self-kill), which a
// standalone-function detour could not - so the #93 have-bit box workaround is no longer platform-specific.

} // namespace

namespace pal
{

bool install_shop_purchase_hook(ShopBuyFn on_buy)
{
    g_on_shop_buy = on_buy;
    const std::uintptr_t addr = resolve_game_symbol(mth::sym::shop_init_state);
    if (addr == 0)
    {
        logf(LogLevel::Warn, "shop: ShopMenu::InitState not resolved; shop check disabled");
        return false;
    }
    g_shop_hook = ScopedHook(addr, &repl_init_state, &g_orig_init_state);
    if (!g_shop_hook.installed())
    {
        logf(LogLevel::Error, "shop: failed to hook ShopMenu::InitState");
        return false;
    }
    logf(LogLevel::Info, "shop: hooked ShopMenu::InitState at %p", g_shop_hook.target());
    return true;
}

void remove_shop_purchase_hook()
{
    g_shop_hook.reset();
    g_on_shop_buy = nullptr;
}

} // namespace pal

// ---- per-stat level cap (Windows). The cap is inlined into the menu's state machine (no cap fn to detour
// like Linux), so we wrap LevelUpMenu::Update and, before its inlined buy-gate runs, fake the cursor-
// selected at-cap stat's stored level to trip the game's own cap gate (native "max level", no buy, no
// spend); restored after. Only the selected stat is faked: the commit handler re-applies the defense level
// (SaveSlot+0x178) to combat and the bank/level rows commit without the buy-gate, so faking an unselected
// stat would leave combat defense pinned to the sentinel for the screen.
namespace
{
constexpr std::ptrdiff_t kSaveStatArrOff = 0x174; // *(saveSlot + 0x174 + stat*4) = int stat level
constexpr int kLvlMaxRealStat = 2;                // stats 0..2 = attack/defense/sidearm; 3 = bone bank
constexpr int kMaxedLevel = 1000;                 // present an at-cap stat as this so the inlined cap gate trips
// LevelUpMenu state field on the derived `this` (r148714): the state-machine switch value. 3 = the
// interactive selection/row-display state that runs the inlined buy-gate + SetDescription; 0 = entry gate,
// 4 = commit, 6 = reopen. We present capped stats as maxed only in state 3, so the entry gate stays on the
// real level (menu opens, bank row reachable, level-up pulse resolves) and the commit recompute is never fed
// a fake level.
constexpr std::ptrdiff_t kLevelUpMenuStateOff = 0x64;
constexpr int kLevelUpMenuInteractiveState = 3;
// menu cursor row (int): 0..2 real stats, 3/4 level/bank. Same offset as Linux; the fake is scoped to it.
constexpr std::ptrdiff_t kLevelUpMenuStatOff = 0xb8;

std::uintptr_t g_lc_addr_update = 0;
[[clang::no_destroy]] pal::ScopedHook g_lc_update_hook;
void (*g_lc_orig_update)(void *, void *) = nullptr; // LevelUpMenu::Update(this, ycUpdateQueueContext*)
pal::LevelCapFn g_lc_cap_fn;
pal::LevelUpMenuFn g_lc_after_update = nullptr;
bool g_lc_resolved = false;
bool g_lc_ok = false;

// LevelUpMenu::Update wrapper: fake the cursor-selected at-cap stat's level so the inlined cap gate reads it
// as maxed, then restore. provide(stat, sentinel) returns the granted count while enforcing and the sentinel
// otherwise, so vanilla play (level < sentinel) is untouched.
void repl_lvlup_update(void *self, void *ctx)
{
    void *slot = pal::active_save_slot(); // the SaveSlot the menu reads
    int saved[3] = {-1, -1, -1};
    // Interactive state only: faking at the entry state (0) would trip the open gate (menu never opens).
    const int menu_state = self != nullptr ? pal::at<int>(self, kLevelUpMenuStateOff) : -1;
    const int cursor_stat = self != nullptr ? pal::at<int>(self, kLevelUpMenuStatOff) : -1;
    const bool interactive = menu_state == kLevelUpMenuInteractiveState;
    if (slot != nullptr && g_lc_cap_fn)
    {
        for (int s = 0; s <= kLvlMaxRealStat; ++s)
        {
            int *lvl = &pal::at<int>(slot, kSaveStatArrOff + static_cast<std::ptrdiff_t>(s) * 4);
            if (mth::boneup_fake_capped_stat(interactive, s == cursor_stat, *lvl, g_lc_cap_fn(s, 0x7fffffff)))
            {
                saved[s] = *lvl;
                *lvl = kMaxedLevel;
            }
        }
    }
    if (g_lc_orig_update)
        g_lc_orig_update(self, ctx);
    if (slot != nullptr)
        for (int s = 0; s <= kLvlMaxRealStat; ++s)
            if (saved[s] >= 0)
                pal::at<int>(slot, kSaveStatArrOff + static_cast<std::ptrdiff_t>(s) * 4) = saved[s];
    if (g_lc_after_update != nullptr)
        g_lc_after_update(self);
}
} // namespace

namespace pal
{

bool level_cap_available()
{
    if (g_lc_resolved)
        return g_lc_ok;
    g_lc_resolved = true;
    const std::uintptr_t save = save_manager_global();                      // g_saveManager (whole-.text cmov scan)
    g_lc_addr_update = resolve_game_symbol(mth::sym::level_up_menu_update); // LevelUpMenu::Update (per-frame entry)
    g_lc_ok = save != 0 && g_lc_addr_update != 0;
    if (g_lc_ok)
        logf(LogLevel::Info, "levelcap: Windows resolved save-base=0x%llx update=0x%llx", static_cast<unsigned long long>(save),
             static_cast<unsigned long long>(g_lc_addr_update));
    else
        logf(LogLevel::Warn, "levelcap: Windows symbols unresolved (save=0x%llx update=0x%llx); feature disabled", static_cast<unsigned long long>(save),
             static_cast<unsigned long long>(g_lc_addr_update));
    return g_lc_ok;
}

void set_level_cap_provider(LevelCapFn cap, LevelUpMenuFn after_update)
{
    if (!level_cap_available())
        return;
    g_lc_cap_fn = std::move(cap);
    g_lc_after_update = after_update;
    g_lc_update_hook = ScopedHook(g_lc_addr_update, &repl_lvlup_update, &g_lc_orig_update);
    if (!g_lc_update_hook.installed())
    {
        logf(LogLevel::Error, "levelcap: Windows hook install FAILED (LevelUpMenu::Update)");
        remove_level_cap_hook();
        return;
    }
    logf(LogLevel::Info, "levelcap: Windows hook installed (LevelUpMenu::Update=0x%llx)", static_cast<unsigned long long>(g_lc_addr_update));
}

void remove_level_cap_hook()
{
    g_lc_update_hook.reset();
    g_lc_cap_fn = nullptr;
    g_lc_after_update = nullptr;
}

} // namespace pal
