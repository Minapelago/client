#pragma once

#include <set>

namespace mth
{

// twin: mth/features/lock_hooks.hpp consumes this registry (locks and locked chests).
// Set of KeyBlock location slots the mod should pre-open. Negative slots (cosmetic locks
// with no save index) are never accepted. Written by AP/console seams, read by the
// pre-seed pass and the lock sweep, all on the game thread.
class LockRegistry
{
  public:
    void set_removed(int slot) // no-op for slot < 0
    {
        if (slot < 0)
            return;
        removed_.insert(slot);
    }

    void clear() // session boundary: removals are seeded from the received items of one connection
    {
        removed_.clear();
    }

    [[nodiscard]] bool is_removed(int slot) const
    {
        if (slot < 0)
            return false;
        return removed_.count(slot) != 0;
    }

    [[nodiscard]] const std::set<int> &removed_slots() const
    {
        return removed_;
    }

  private:
    std::set<int> removed_;
};

} // namespace mth
