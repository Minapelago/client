#pragma once

#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

namespace mth
{

// Inbound grants accepted but not yet applied. The receipt rides along so drain() acks only what landed (#175). Thread-safe.
class GrantQueue
{
  public:
    void push(int item_type, int receipt)
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_.push_back(Pending{item_type, receipt});
    }

    // Returns how many grants were dropped.
    std::size_t clear()
    {
        std::lock_guard<std::mutex> lk(mu_);
        return std::exchange(pending_, {}).size();
    }

    // Takes the whole queue. If ready() fails the batch goes back ahead of anything pushed meanwhile; otherwise each grant is applied, then acked.
    template <class Ready, class Apply, class Ack> void drain(Ready &&ready, Apply &&apply, Ack &&ack)
    {
        std::vector<Pending> batch;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (pending_.empty())
                return;
            batch.swap(pending_);
        }
        if (!ready())
        {
            std::lock_guard<std::mutex> lk(mu_);
            pending_.insert(pending_.begin(), batch.begin(), batch.end());
            return;
        }
        for (const Pending &p : batch)
        {
            apply(p.item_type);
            ack(p.receipt);
        }
    }

  private:
    struct Pending
    {
        int item_type;
        int receipt;
    };

    std::mutex mu_;
    std::vector<Pending> pending_;
};

} // namespace mth
