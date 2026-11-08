#include <cstddef>
#include <cstdint>
#include <utility>

#include "mth/core/game/game_symbols.hpp"
#include "pal/pal_game.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"
#include "pal/pal_module.hpp"
#include "pal/scoped_hook.hpp"

namespace
{
// ShopMenu field offsets (build-specific; the AP callback self-checks the read locIdx/itemType).
constexpr std::ptrdiff_t kShopLocIdxOff = 0x218;
constexpr std::ptrdiff_t kShopItemTypeOff = 0x21c;

pal::ShopBuyFn g_on_shop_buy = nullptr;
[[clang::no_destroy]] pal::ScopedHook g_shop_hook;
void (*g_orig_item_present)(void *) = nullptr;

// ItemPresent runs once per granted shop item: read locIdx/itemType, let the AP callback collect it, and write back the itemType before the original grants.
void repl_item_present(void *self)
{
    if (g_on_shop_buy != nullptr && self != nullptr)
    {
        int &loc_idx = pal::at<int>(self, kShopLocIdxOff);
        int &item_type = pal::at<int>(self, kShopItemTypeOff);
        pal::logf(pal::LogLevel::Debug, "shop: ItemPresent locIdx=%d itemType=%d", loc_idx, item_type);
        item_type = g_on_shop_buy(loc_idx, item_type); // redirect suppresses the vanilla grant
    }
    if (g_orig_item_present)
        g_orig_item_present(self);
}

// The Items::IsItemCollected override is the native "IsItemCollected" hook in mod/mod_api.cpp.

// ---- per-stat level cap ----
// LevelUpMenu cursor-selected stat index as an int (build 16280b26 decompile of LevelUpMenu::UpdateState; the state machine is at +0x64).
// repl_max_level logs the value read so it can be confirmed on the shipping build.
constexpr std::ptrdiff_t kLevelUpMenuStatOff = 0xb8;

bool g_lc_resolved = false;
bool g_lc_ok = false;
std::uintptr_t g_addr_lvlup_update = 0;
std::uintptr_t g_addr_max_level = 0;
[[clang::no_destroy]] pal::ScopedHook g_lvlup_update_hook;
[[clang::no_destroy]] pal::ScopedHook g_max_level_hook;
void (*g_orig_lvlup_update)(void *) = nullptr;
int (*g_orig_max_level)(int, int, void *) = nullptr;
pal::LevelCapFn g_cap_fn;
pal::LevelUpMenuFn g_lvlup_after_update = nullptr;
void *g_active_lvlup_menu = nullptr; // set only while inside UpdateState (game thread; nested call)

void repl_lvlup_update(void *self)
{
    void *prev = g_active_lvlup_menu;
    g_active_lvlup_menu = self;
    if (g_orig_lvlup_update)
        g_orig_lvlup_update(self);
    if (g_lvlup_after_update != nullptr)
        g_lvlup_after_update(self);
    g_active_lvlup_menu = prev;
}

int repl_max_level(int a, int b, void *slot)
{
    const int vanilla = g_orig_max_level(a, b, slot);
    if (g_active_lvlup_menu == nullptr || !g_cap_fn)
        return vanilla; // called outside the level-up menu: never restrict
    const int stat = pal::at<int>(g_active_lvlup_menu, kLevelUpMenuStatOff);
    const int capped = g_cap_fn(stat, vanilla);
    // UpdateState calls this every frame while the "can't upgrade further" text plays, so log a reading only when it changes.
    static int last_stat = -1, last_vanilla = -1, last_capped = -1;
    if (stat != last_stat || vanilla != last_vanilla || capped != last_capped)
    {
        last_stat = stat;
        last_vanilla = vanilla;
        last_capped = capped;
        pal::logf(pal::LogLevel::Debug, "levelcap: buy-gate stat=%d vanilla=%d -> cap=%d", stat, vanilla, capped);
    }
    return capped;
}

// Rolls back a partial install: removes both level-cap hooks and clears the callbacks.
void remove_level_cap_hook()
{
    for (pal::ScopedHook *hook : {&g_lvlup_update_hook, &g_max_level_hook})
        hook->reset();
    g_cap_fn = nullptr;
    g_lvlup_after_update = nullptr;
    g_active_lvlup_menu = nullptr;
}

} // namespace

namespace pal
{

bool install_shop_purchase_hook(ShopBuyFn on_buy)
{
    g_on_shop_buy = on_buy;
    const std::uintptr_t addr = resolve_game_symbol(mth::sym::shop_item_present);
    if (addr == 0)
    {
        logf(LogLevel::Warn, "shop: ShopMenu::ItemPresent not resolved; shop check disabled");
        return false;
    }
    g_shop_hook = ScopedHook(addr, &repl_item_present, &g_orig_item_present);
    if (!g_shop_hook.installed())
    {
        logf(LogLevel::Error, "shop: failed to hook ShopMenu::ItemPresent");
        return false;
    }
    logf(LogLevel::Info, "shop: hooked ShopMenu::ItemPresent at %p", g_shop_hook.target());
    return true;
}

bool level_cap_available()
{
    if (g_lc_resolved)
        return g_lc_ok;
    g_lc_resolved = true;
    g_addr_lvlup_update = resolve_game_symbol(mth::sym::level_up_menu_update_state);
    g_addr_max_level = resolve_game_symbol(mth::sym::get_new_game_max_level_player);
    g_lc_ok = g_addr_lvlup_update != 0 && g_addr_max_level != 0;
    if (!g_lc_ok)
        logf(LogLevel::Warn, "levelcap: symbols unresolved (update=0x%llx maxlevel=0x%llx); feature disabled",
             static_cast<unsigned long long>(g_addr_lvlup_update), static_cast<unsigned long long>(g_addr_max_level));
    return g_lc_ok;
}

void set_level_cap_provider(LevelCapFn cap, LevelUpMenuFn after_update)
{
    if (!level_cap_available())
        return;
    g_cap_fn = std::move(cap);
    g_lvlup_after_update = after_update;
    g_lvlup_update_hook = ScopedHook(g_addr_lvlup_update, &repl_lvlup_update, &g_orig_lvlup_update);
    g_max_level_hook = ScopedHook(g_addr_max_level, &repl_max_level, &g_orig_max_level);
    if (!g_lvlup_update_hook.installed() || !g_max_level_hook.installed())
    {
        logf(LogLevel::Error, "levelcap: hook install FAILED (update at %p maxlevel at %p); rolling back", g_lvlup_update_hook.target(),
             g_max_level_hook.target());
        remove_level_cap_hook(); // all-or-nothing: drop any partial hook + clear the callback
        return;
    }
    logf(LogLevel::Info, "levelcap: hooks installed (update=0x%llx maxlevel=0x%llx)", static_cast<unsigned long long>(g_addr_lvlup_update),
         static_cast<unsigned long long>(g_addr_max_level));
}

} // namespace pal
