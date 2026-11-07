#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace mth
{

// InteractComponent::OpenShop merges consecutive same-item-type shop entries into one stacked box
// unless the shop's "never stack" bit is set; setting it makes every level its own buyable box (what
// the built-in shuffler produces indirectly via randomized item types). ShopDef flags are a dword at
// +0x228; bit 0x100 is the never-stack control.
inline constexpr std::ptrdiff_t kShopFlagsOff = 0x228;
inline constexpr std::uint32_t kShopNeverStackBit = 0x100;

// Pure: OR the never-stack bit when the mod is active, else pass flags through. No platform deps.
[[nodiscard]] constexpr std::uint32_t apply_flatten_flag(std::uint32_t flags, bool active) noexcept
{
    return active ? (flags | kShopNeverStackBit) : flags;
}
static_assert(apply_flatten_flag(0u, true) == kShopNeverStackBit);
static_assert(apply_flatten_flag(0u, false) == 0u && apply_flatten_flag(0x55u, false) == 0x55u);
static_assert(apply_flatten_flag(0x2Au, true) == (0x2Au | kShopNeverStackBit));
static_assert(apply_flatten_flag(apply_flatten_flag(0x2Au, true), true) == apply_flatten_flag(0x2Au, true));

// Real shops hold well under this; a larger value means the field being read is not a row count. The
// risk here is one-sided now that the mode gate catches the appearance menu: this can no longer prevent
// a crash, only false-negative a shop the mod's own flatten feature expanded past the bound, which costs
// scouted text on that shop and nothing else.
inline constexpr int kMaxShopBoxes = 64;

// Rows of the ShopMenu box array that are safe to walk. The array is malloc'd and never zeroed, and its
// count only becomes meaningful once SetupBoxes has filled it, so a menu that returned early leaves both
// holding whatever was on the heap. Bail on an out-of-range count rather than clamping: a clamp would
// still walk an array that may not be a box array at all.
[[nodiscard]] constexpr int shop_box_walk_count(int rows) noexcept
{
    return (rows > 0 && rows <= kMaxShopBoxes) ? rows : 0;
}
static_assert(shop_box_walk_count(1) == 1 && shop_box_walk_count(5) == 5 && shop_box_walk_count(kMaxShopBoxes) == kMaxShopBoxes);
static_assert(shop_box_walk_count(0) == 0 && shop_box_walk_count(-1) == 0 && shop_box_walk_count(-0x1000000) == 0);
static_assert(shop_box_walk_count(kMaxShopBoxes + 1) == 0 && shop_box_walk_count(0x1000000) == 0);

// Which palette entry to overwrite so a color survives the engine's palette mapping. A negative
// api_index is PaletteGetIndex's miss answer, which the engine resolves to entry 0. Nullopt when
// the entry does not exist: PaletteWriteIndex is unbounded.
[[nodiscard]] constexpr std::optional<std::uint32_t> palette_target_index(std::int32_t api_index, std::uint32_t palette_width)
{
    const std::uint32_t idx = api_index < 0 ? 0u : static_cast<std::uint32_t>(api_index);
    if (idx >= palette_width)
        return std::nullopt;
    return idx;
}
static_assert(palette_target_index(3, 16) == 3u && palette_target_index(0, 16) == 0u && palette_target_index(15, 16) == 15u);
// A miss from the API (negative) resolves to entry 0, as the engine does.
static_assert(palette_target_index(-1, 16) == 0u && palette_target_index(-999, 16) == 0u);
// PaletteWriteIndex has no bounds check, so an index past the palette must not reach it.
static_assert(!palette_target_index(16, 16) && !palette_target_index(17, 16) && !palette_target_index(0, 0) && !palette_target_index(-1, 0));

} // namespace mth
