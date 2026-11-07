#pragma once

#include <climits>
#include <functional>
#include <utility>

namespace mth
{

// Receipt for a grant with no durable bookkeeping (dev console, one-off effects). Never acked; it sits below every real index, including the negative console
// range.
inline constexpr int kNoReceipt = INT_MIN;

// twin: mth/features/item_granter.hpp is the game-coupled impl.
// Grants one item by game itemType. The impl may queue it for a later engine window: the return value reports acceptance, the applied sink reports
// application. Durable "granted" state keys off the sink (#175).
class IItemGranter
{
  public:
    virtual ~IItemGranter() = default;

    // receipt: the caller's opaque id, handed back through the applied sink once the grant runs. Returns false if unavailable now; caller retries
    // without marking. A true return promises exactly one applied-sink call for the receipt, possibly before returning.
    virtual bool grant(int item_type, int receipt) = 0;

    // Drops anything accepted but not yet applied; the caller retries those receipts. Called on a session change so one seed's queue can't land in
    // another's save.
    virtual void discard_pending() = 0;

    void set_applied_sink(std::function<void(int)> sink)
    {
        applied_sink_ = std::move(sink);
    }

  protected:
    void notify_applied(int receipt)
    {
        // Receipt check first: a kNoReceipt grant can come from the overlay thread, which must not read the sink while the game thread swaps it.
        if (receipt != kNoReceipt && applied_sink_)
            applied_sink_(receipt);
    }

  private:
    std::function<void(int)> applied_sink_;
};

} // namespace mth
