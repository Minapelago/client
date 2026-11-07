#include <span>

#include "mth/core/game/game_symbols.hpp"
#include "pal/pal_module.hpp"
#include "pal/windows/signature_scan.hpp"

namespace pal
{

std::uintptr_t resolve_game_symbol(const char *mangled_name)
{
    return scan_resolve(mangled_name);
}

namespace
{
// See pal_module.hpp. Kept next to the resolver so the list stays with the platform that uses it.
constexpr const char *kPlatformSymbols[] = {
    mth::sym::level_up_menu_update,
    mth::sym::shop_init_state,
};
} // namespace

std::span<const char *const> required_platform_symbols()
{
    return kPlatformSymbols;
}

} // namespace pal
