#pragma once

#include <functional>

namespace mth
{

// PawnShopNPC::OnNPCEvent suppressor that makes the pawn shop ("Pawnty") uninteractible while should_disable() is true. should_disable() runs on the game
// thread, so it must be cheap and thread-safe. If the chokepoint does not resolve, install logs and leaves the shop alone.
void install_pawn_shop_hook(std::function<bool()> should_disable);

} // namespace mth
