#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "mth/core/ap/ability_ids.hpp"

namespace mth
{

// twin: mth/features/ability_hooks.hpp enforces this in-game.
// Pure per-ability gate decision. No game/PAL/IO dependency; the caller supplies the AP-grant lookup
// and whether the active save is the AP slot.
class AbilityGate
{
  public:
    void set_randomized(Ability a, bool on)
    {
        randomized_[static_cast<int>(a)] = on;
    }
    [[nodiscard]] bool randomized(Ability a) const
    {
        return randomized_[static_cast<int>(a)];
    }

    // is_granted is only asked about randomized abilities on the AP slot.
    [[nodiscard]] bool blocked(Ability a, bool slot_is_ap, const std::function<bool(std::int64_t item_id)> &is_granted) const
    {
        return randomized(a) && slot_is_ap && !is_granted(ability_item_id(a));
    }

  private:
    std::array<bool, kAbilityCount> randomized_{};
};

} // namespace mth
