#pragma once

#include <cstdint>

namespace mth
{

// Shop flattening: while active() (polled per Shop::Get, game thread), ORs the never-stack bit onto each ShopDef before OpenShop builds its boxes, so stacked
// slots show one box per level. The bit is never cleared, so a touched shop stays flat until the game restarts.
using ShopFlattenFn = bool (*)();
bool install_shop_flatten_hook(ShopFlattenFn active);

// Post-hook on ShopMenu::SetCursor. The callback uses the helpers below to read the selection, enumerate boxes and rewrite the name/desc widgets.
using ShopTextFn = void (*)(void *shop_menu);
bool install_shop_text_hook(ShopTextFn on_set_cursor);

// ShopMenu accessors and text mutators.
[[nodiscard]] int shop_selected_loc(void *shop_menu); // -1 if sold-out / itemType 0x65 / invalid
void shop_enumerate_locs(void *shop_menu, void (*sink)(int loc, void *ctx), void *ctx);
[[nodiscard]] void *shop_name_widget(void *shop_menu);
[[nodiscard]] void *shop_desc_widget(void *shop_menu);

// Points a palette-mapped text widget at a private palette clone carrying `rgba` at the entry the
// widget will select, so the color survives the mapping. False leaves the widget untouched.
bool shop_apply_name_palette(void *name_widget, std::uint32_t rgba);

} // namespace mth
