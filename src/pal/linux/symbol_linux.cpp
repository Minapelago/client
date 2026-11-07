#include <span>

#include <frida-gum.h>

#include "mth/core/game/game_symbols.hpp"
#include "pal/pal_module.hpp"

namespace pal
{

std::uintptr_t resolve_game_symbol(const char *mangled_name)
{
    const GumModuleDetails *m = gum_process_get_main_module();
    if (m == nullptr || mangled_name == nullptr)
        return 0;
    const GumAddress addr = gum_module_find_symbol_by_name(m->name, mangled_name);
    return static_cast<std::uintptr_t>(addr);
}

namespace
{
// See pal_module.hpp. Kept next to the resolver so the list stays with the platform that uses it.
constexpr const char *kPlatformSymbols[] = {
    mth::sym::get_new_game_max_level_player,
    mth::sym::level_up_menu_update_state,
    mth::sym::shop_item_present,
    // Required: it is the only reachable SpringBellows::CollideWith here, and its name carries the +376 subobject offset, so a layout change renames it
    // and must fail loudly (#188).
    mth::sym::spring_bellows_collide_listener,
};
} // namespace

std::span<const char *const> required_platform_symbols()
{
    return kPlatformSymbols;
}

} // namespace pal
