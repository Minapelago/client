#pragma once

#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace mth
{

// Work posted from any thread, run by whichever thread calls drain(). The dev console posts from the overlay, and App drains on the game thread.
class CommandQueue
{
  public:
    void post(std::function<void()> fn)
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_.push_back(std::move(fn));
    }

    // The batch runs outside the lock, so a command can post without deadlocking. What it posts waits for the next drain.
    void drain()
    {
        std::vector<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lk(mu_);
            batch.swap(pending_);
        }
        for (auto &fn : batch)
            fn();
    }

  private:
    std::mutex mu_;
    std::vector<std::function<void()>> pending_;
};

} // namespace mth
