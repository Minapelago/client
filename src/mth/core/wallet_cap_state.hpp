#pragma once

#include <optional>

#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/ap/ap_state.hpp"

namespace mth
{

// slot_data "wallet_cap": the bone wallet is capped by the number of received wallet items
// (kProgWalletId). Base 750, +500 per item, and uncapped once kWalletFreeAt items are held.
inline constexpr int kWalletBase = 750;
inline constexpr int kWalletStep = 500;
inline constexpr int kWalletFreeAt = 8;

// Pure per-item cap: nullopt once count >= kWalletFreeAt (no cap), else kWalletBase + kWalletStep*count.
// Unit-testable, no platform deps.
[[nodiscard]] constexpr std::optional<int> wallet_cap_for(int count) noexcept
{
    if (count >= kWalletFreeAt)
        return std::nullopt;
    if (count < 0)
        count = 0;
    return kWalletBase + kWalletStep * count;
}

// twin: App::enforce_wallet_cap clamps live bones to this each frame.
// The bone cap the received wallet items allow, or nullopt for "uncapped".
[[nodiscard]] inline std::optional<int> wallet_cap(const ApState &state)
{
    int count = 0;
    for (const auto &it : state.received_items())
        if (is_wallet_item(it.item_id))
            ++count;
    return wallet_cap_for(count);
}

} // namespace mth
