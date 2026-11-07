#pragma once

#include <cstdint>

namespace mth
{

// Shop flattening: while active, OR the game's never-stack bit onto each ShopDef before OpenShop
// builds its box list, so stacked slots show one buyable box per level. `active` is polled per
// Shop::Get call (game thread); the bit is only ever set, never cleared, so a disconnect leaves
// already-touched shops flat until the game restarts.
using ShopFlattenFn = bool (*)();
bool install_shop_flatten_hook(ShopFlattenFn active);
void remove_shop_flatten_hook();

// Shop item text: post-hook ShopMenu::SetCursor; the callback receives the ShopMenu and uses the
// helpers below to read the selected location / enumerate boxes / rewrite the name+desc widgets.
using ShopTextFn = void (*)(void *shop_menu);
bool install_shop_text_hook(ShopTextFn on_set_cursor);
void remove_shop_text_hook();

// ShopMenu accessors and text mutators.
[[nodiscard]] int shop_selected_loc(void *shop_menu); // -1 if sold-out / itemType 0x65 / invalid
void shop_enumerate_locs(void *shop_menu, void (*sink)(int loc, void *ctx), void *ctx);
[[nodiscard]] void *shop_name_widget(void *shop_menu);
[[nodiscard]] void *shop_desc_widget(void *shop_menu);

// Points a palette-mapped text widget at a private palette clone carrying `rgba` at the entry the
// widget will select, so the color survives the mapping. False leaves the widget untouched.
bool shop_apply_name_palette(void *name_widget, std::uint32_t rgba);

} // namespace mth
