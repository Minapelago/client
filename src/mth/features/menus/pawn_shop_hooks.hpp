#pragma once

#include <functional>

namespace mth
{

// The PawnShopNPC::OnNPCEvent suppressor that makes the pawn shop ("Pawnty") uninteractible while
// should_disable() is true. should_disable() runs on the game thread, so it must be cheap and
// thread-safe. Install logs and leaves the shop alone if the chokepoint does not resolve.
void install_pawn_shop_hook(std::function<bool()> should_disable);
void remove_pawn_shop_hook();

} // namespace mth
