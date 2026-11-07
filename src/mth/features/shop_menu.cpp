// ShopMenu instance accessors, the item-name palette swap, and the two shop detours whose bodies are
// identical on both builds: the SetCursor text post-hook and the Shop::Get flatten. The purchase detour
// stays in pal/{linux,windows}/game_detours_*.cpp, where the buy-confirm frame differs per build.

#include "mth/features/shop_menu.hpp"

#include <cstddef>
#include <cstdint>

#include "mod/mod_api.hpp"
#include "mth/core/data/game_layout.hpp"
#include "mth/core/data/game_symbols.hpp"
#include "mth/core/palette_index.hpp"
#include "mth/core/shop_boxes.hpp"
#include "mth/core/shop_flatten.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"
#include "pal/pal_module.hpp"
#include "pal/scoped_hook.hpp"

namespace mth
{
namespace
{

// ShopItem / ShopItemDef instance offsets. Verified on the Windows depot build: ShopItem+0xf8 (active
// def*), +0xec (stock count), ShopItemDef+0x48 (cached GetCollectionIndex == loc_idx) all match Linux.

ShopFlattenFn g_shop_flatten_cb = nullptr;
[[clang::no_destroy]] pal::ScopedHook g_shop_flatten_hook;
// Shop::Get takes a 64-bit name hash. Use a fixed-width type: `unsigned long` is 32-bit under Windows
// LLP64 and would truncate the hash, so the forwarded lookup finds no shop and returns null.
void *(*g_orig_shop_get)(std::uint64_t name_hash) = nullptr; // Shop::Get(uint64_t) -> ShopDef*

// Shop::Get(nameHash) is the accessor InteractComponent::OpenShop consults before building a shop's
// box list; OR the never-stack bit onto the returned ShopDef so stacked slots flatten (one box/level).
void *repl_shop_get(std::uint64_t name_hash)
{
    void *def = g_orig_shop_get != nullptr ? g_orig_shop_get(name_hash) : nullptr;
    if (def != nullptr && g_shop_flatten_cb != nullptr && g_shop_flatten_cb())
    {
        auto *flags = &pal::at<std::uint32_t>(def, kShopFlagsOff);
        *flags = apply_flatten_flag(*flags, true);
    }
    return def;
}

// ShopMenu::SetCursor post-hook: rewrites the selected box's name+description text from scouted AP data.
// Standalone on Windows (not inlined), so this is a direct post-hook exactly like Linux.
ShopTextFn g_shop_text_cb = nullptr;
[[clang::no_destroy]] pal::ScopedHook g_shop_text_hook;
void (*g_orig_set_cursor)(void *, int, bool) = nullptr;

// ShopMenu instance fields (box list + selection); distinct from the InitState frame offsets above.
// Confirmed identical to Linux by decompiling SetCursor on the r148851 PE.
constexpr std::ptrdiff_t kShopNameWidgetOff = 0x148;
constexpr std::ptrdiff_t kShopDescWidgetOff = 0x150;
constexpr std::ptrdiff_t kShopBoxArrayOff = 0x1c8;
constexpr std::ptrdiff_t kShopBoxCountOff = 0x1d0;
constexpr std::ptrdiff_t kShopCursorOff = 0x1d8;
constexpr std::ptrdiff_t kShopModeOff = 0x23c;
constexpr std::ptrdiff_t kShopBoxItemTypeOff = 0xcc; // ShopItem -> itemType (box's item kind)
constexpr int kShopSkipItemType = 0x65;

// ShopMenu runs in one of two modes, selected by the int at +0x23c: the ctor zeroes it, then stores 1 for
// the shop named `Color` (the Atelier's "Custom Fitting" appearance menu). A mode-1 SetupBoxes takes the
// palette-widget branch and returns without ever filling the box array. Gate on != 0 rather than the
// game's own == 1 so that a mode added by a later build is excluded by default.
[[nodiscard]] bool shop_menu_is_stocked(const void *shop_menu) noexcept
{
    return pal::at<int>(shop_menu, kShopModeOff) == 0;
}

// Resolve the box array plus a count that is safe to walk, or false when this menu has no walkable rows.
// OpenShop allocates +0x1c8 and sets a real row count in +0x1d0 for every menu including a mode-1 one,
// but SetupBoxes returns before filling that array, leaving a live, correctly sized buffer full of
// uninitialized heap. A null check and a range check both pass on it; only the mode gate catches it.
[[nodiscard]] bool shop_box_list(void *shop_menu, void ***out_boxes, int *out_count) noexcept
{
    if (!pal::pointer_looks_valid(shop_menu) || !shop_menu_is_stocked(shop_menu))
        return false;
    void **boxes = pal::at<void **>(shop_menu, kShopBoxArrayOff);
    const int count = shop_box_walk_count(pal::at<int>(shop_menu, kShopBoxCountOff));
    if (!pal::pointer_looks_valid(boxes) || count <= 0)
        return false;
    *out_boxes = boxes;
    *out_count = count;
    return true;
}

void repl_set_cursor(void *self, int index, bool b)
{
    if (g_orig_set_cursor)
        g_orig_set_cursor(self, index, b);
    if (g_shop_text_cb != nullptr && self != nullptr)
        g_shop_text_cb(self);
}

// One clone serves every widget and every shop. Update resolves a second entry of this same clone
// (component+0x17c) besides the one written here, but RenderInternal consumes it only when the flag
// at component+0x1ac is set, and the ctor zeroes it.
void *g_clone = nullptr;
bool g_warned_range = false;
bool g_warned_refcount = false;
bool g_warned_plausibility = false;

// Cloned once and never released. ReleasePalette only decrements and never frees, so releasing
// reclaims nothing and lets the game's release path destroy the clone under a live widget.
void *ensure_clone(void *source)
{
    if (g_clone != nullptr)
        return g_clone;
    void *clone = mod::clone_palette(source);
    if (clone == nullptr)
        return nullptr;
    mod::palette_set_group(clone, -1); // detach from the group remap chain so it resolves from its own colors
    g_clone = clone;
    return g_clone;
}

} // namespace

bool install_shop_flatten_hook(ShopFlattenFn active)
{
    g_shop_flatten_cb = active;
    const std::uintptr_t addr = pal::resolve_game_symbol(sym::shop_get);
    if (addr == 0)
    {
        pal::logf(pal::LogLevel::Warn, "shop: Shop::Get not resolved; stacked-shop flattening disabled");
        g_shop_flatten_cb = nullptr;
        return false;
    }
    g_shop_flatten_hook = pal::ScopedHook(addr, &repl_shop_get, &g_orig_shop_get);
    if (!g_shop_flatten_hook.installed())
    {
        pal::logf(pal::LogLevel::Error, "shop: failed to hook Shop::Get");
        g_shop_flatten_cb = nullptr;
        return false;
    }
    pal::logf(pal::LogLevel::Info, "shop: hooked Shop::Get for flattening at %p", g_shop_flatten_hook.target());
    return true;
}

void remove_shop_flatten_hook()
{
    g_shop_flatten_hook.reset();
    g_shop_flatten_cb = nullptr;
    g_orig_shop_get = nullptr;
}

int shop_selected_loc(void *shop_menu)
{
    void **boxes = nullptr;
    int count = 0;
    if (!shop_box_list(shop_menu, &boxes, &count))
        return -1;
    const int cursor = pal::at<int>(shop_menu, kShopCursorOff);
    if (cursor < 0 || cursor >= count)
        return -1;
    void *box = boxes[cursor];
    if (!pal::pointer_looks_valid(box))
        return -1;
    const int item_type = pal::at<int>(box, kShopBoxItemTypeOff);
    if (item_type == kShopSkipItemType)
        return -1;
    // Sold-out box: stock count 0 (same field the ShopItem::Refresh hook zeroes for a fully-bought slot).
    if (pal::at<int>(box, layout::kShopItemStockOff) == 0)
        return -1;
    void *def = pal::at<void *>(box, layout::kShopItemDefOff);
    if (!pal::pointer_looks_valid(def))
        return -1;
    return pal::at<int>(def, layout::kShopDefLocOff);
}

void shop_enumerate_locs(void *shop_menu, void (*sink)(int loc, void *ctx), void *ctx)
{
    void **boxes = nullptr;
    int count = 0;
    if (sink == nullptr || !shop_box_list(shop_menu, &boxes, &count))
        return;
    for (int i = 0; i < count; ++i)
    {
        void *box = boxes[i];
        if (!pal::pointer_looks_valid(box))
            continue;
        void *def = pal::at<void *>(box, layout::kShopItemDefOff);
        if (!pal::pointer_looks_valid(def))
            continue;
        sink(pal::at<int>(def, layout::kShopDefLocOff), ctx);
    }
}

// Deliberately not mode-gated: SetupBoxes builds a live text widget for the appearance menu too, so these
// return a real widget there. Safe only because on_shop_set_cursor resolves a location first and bails
// when there is none; keep that order, or the appearance menu's title gets overwritten with an AP name.
void *shop_name_widget(void *shop_menu)
{
    return shop_menu != nullptr ? pal::at<void *>(shop_menu, kShopNameWidgetOff) : nullptr;
}

void *shop_desc_widget(void *shop_menu)
{
    return shop_menu != nullptr ? pal::at<void *>(shop_menu, kShopDescWidgetOff) : nullptr;
}

bool install_shop_text_hook(ShopTextFn on_set_cursor)
{
    const std::uintptr_t addr = pal::resolve_game_symbol(sym::shop_set_cursor);
    if (addr == 0)
    {
        pal::logf(pal::LogLevel::Warn, "shop: ShopMenu::SetCursor not resolved; shop text override disabled");
        return false;
    }
    g_shop_text_cb = on_set_cursor;
    g_shop_text_hook = pal::ScopedHook(addr, &repl_set_cursor, &g_orig_set_cursor);
    if (!g_shop_text_hook.installed())
    {
        pal::logf(pal::LogLevel::Error, "shop: failed to hook ShopMenu::SetCursor");
        g_shop_text_cb = nullptr;
        return false;
    }
    pal::logf(pal::LogLevel::Info, "shop: hooked ShopMenu::SetCursor for text override at %p", g_shop_text_hook.target());
    return true;
}

void remove_shop_text_hook()
{
    g_shop_text_hook.reset();
    g_shop_text_cb = nullptr;
    g_orig_set_cursor = nullptr;
}

bool shop_apply_name_palette(void *name_widget, std::uint32_t rgba)
{
    if (!pal::pointer_looks_valid(name_widget) || !mod::palette_api_available())
        return false;

    void *lookup = pal::at<void *>(name_widget, layout::kTextLookupPaletteOff);
    void *output = pal::at<void *>(name_widget, layout::kTextOutputPaletteOff);
    if (!pal::pointer_looks_valid(lookup) || !pal::pointer_looks_valid(output))
        return false;

    void *clone = ensure_clone(output);
    if (clone == nullptr)
        return false;

    const auto target = palette_target_index(mod::palette_get_index(lookup, rgba), mod::palette_get_width(clone));
    if (!target.has_value())
    {
        if (!g_warned_range)
        {
            g_warned_range = true;
            pal::logf(pal::LogLevel::Warn, "shop: palette entry out of range; item name color skipped");
        }
        return false;
    }
    mod::palette_write_index(clone, static_cast<std::int32_t>(*target), rgba);

    if (output != clone)
    {
        std::int32_t *out_rc = &pal::at<std::int32_t>(output, layout::kPaletteRefCountOff);
        const std::uint32_t out_width = mod::palette_get_width(output);
        // pointer_looks_valid above only proves output is a canonical address, not a
        // ycPaletteTexture. Implausible bounds mean the field moved between builds and the writes
        // below would land in an unrelated object.
        if (*out_rc <= 0 || *out_rc >= 0x10000 || out_width == 0 || out_width > 255)
        {
            if (!g_warned_plausibility)
            {
                g_warned_plausibility = true;
                pal::logf(pal::LogLevel::Warn,
                          "shop: output palette at +0x128 does not look like a ycPaletteTexture (refcount=%d width=%u); leaving it untouched", *out_rc,
                          out_width);
            }
            return false;
        }

        // Hand-rolled SetPalette. The widget's reference to the game's palette moves to the clone.
        if (*out_rc > 1)
        {
            *out_rc -= 1;
        }
        else if (!g_warned_refcount)
        {
            // The owning ShopMenu holds its own reference, so this should be unreachable. Leaking a
            // reference is survivable; destroying a palette the game still points at is not.
            g_warned_refcount = true;
            pal::logf(pal::LogLevel::Warn, "shop: name palette refcount would reach zero; leaving it held");
        }

        pal::at<void *>(name_widget, layout::kTextOutputPaletteOff) = clone;
        pal::at<std::int32_t>(clone, layout::kPaletteRefCountOff) += 1;
        pal::at<std::int32_t>(name_widget, layout::kTextPaletteVersionOff) = pal::at<std::int32_t>(clone, layout::kPaletteVersionOff) - 1;
    }
    return true;
}

} // namespace mth
