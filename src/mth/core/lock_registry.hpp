#pragma once

#include <mutex>
#include <set>
#include <vector>

namespace mth
{

// twin: mth/features/lock_hooks.hpp consumes this registry (locks and locked chests).
// Thread-safe set of KeyBlock location slots the mod should pre-open. Negative slots
// (cosmetic locks with no save index) are never accepted. Written by AP/console seams,
// read by the game-thread pre-seed pass.
class LockRegistry
{
  public:
    void set_removed(int slot) // no-op for slot < 0
    {
        if (slot < 0)
            return;
        std::lock_guard<std::mutex> lk(mtx_);
        removed_.insert(slot);
    }

    void clear() // session boundary: removals are seeded from the received items of one connection
    {
        std::lock_guard<std::mutex> lk(mtx_);
        removed_.clear();
    }

    [[nodiscard]] bool is_removed(int slot) const
    {
        if (slot < 0)
            return false;
        std::lock_guard<std::mutex> lk(mtx_);
        return removed_.count(slot) != 0;
    }

    [[nodiscard]] std::vector<int> removed_slots() const // copy under lock, for the seed pass
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return std::vector<int>(removed_.begin(), removed_.end());
    }

  private:
    mutable std::mutex mtx_;
    std::set<int> removed_;
};

} // namespace mth
