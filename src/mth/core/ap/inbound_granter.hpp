#pragma once

#include <functional>
#include <memory>
#include <set>

#include "mth/core/rules/trap_state.hpp" // TrapArm

namespace mth
{

class ApState;
class ApSaveState;
class IItemGranter;

// Drives inbound grants each tick. Stops at first granter failure and retries next tick.
class InboundGranter
{
  public:
    // credit_kear_key: vanilla-kear effect granting one usable key; false until a save/player is live, so the receipt retries unmarked. Empty in tests.
    // arm_trap: forces a cosmetic modifier on for a while. Empty means this build can't run traps, so the receipt is consumed. Both come from App.
    InboundGranter(IItemGranter &granter, ApState &state, ApSaveState &save, std::function<bool()> credit_kear_key = {},
                   std::function<TrapArm(int)> arm_trap = {});
    ~InboundGranter();

    InboundGranter(const InboundGranter &) = delete;
    InboundGranter &operator=(const InboundGranter &) = delete;

    void tick();

  private:
    // Receipts accepted but not yet applied aren't durable, so ApSaveState can't suppress them; in_flight_ holds them until the applied sink acks (#175).
    [[nodiscard]] bool handled(int index) const;
    bool offer(int game_type, int index);
    void on_applied(int receipt);
    void consume(int index); // record the receipt as granted and stage the save

    IItemGranter &granter_;
    ApState &state_;
    ApSaveState &save_;
    std::function<bool()> credit_kear_key_;
    std::function<TrapArm(int)> arm_trap_;
    // Set false by the dtor. The sink lives in the longer-lived granter and may already belong to a successor, so teardown disarms only its own.
    std::shared_ptr<bool> alive_;
    std::set<int> in_flight_;
    int last_handled_ = -1; // last logged skip census; -1 forces the first line
    int last_ungrantable_ = -1;
    int last_in_flight_ = -1;
    int stuck_ticks_ = 0; // consecutive ticks with an undrained queue
    bool stuck_warned_ = false;
    // Trap stall latch, separate from stuck_ticks_ because a trap's not-ready says nothing about the granter queue.
    int trap_notready_ticks_ = 0;
    bool trap_notready_warned_ = false;
};

} // namespace mth
