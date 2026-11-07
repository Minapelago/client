#pragma once

#include <functional>

#include "mth/core/ap/item_granter_interface.hpp"

namespace mth
{

// Backstop for #162 alongside TicketMachineGate: refuses the station donation machine's Train Pass dispense while the train is randomized. That dispense goes
// through Items::OnPickup at a location the apworld lacks, so is_ap_location never sees it; inbound AP grants use OnPickupDone, so the AP Train Pass still
// lands.
void set_train_pass_machine_blocked(bool on);

// twin: mth/core/ap/item_granter_interface.hpp (the IItemGranter interface).
// IItemGranter via Items::OnPickupDone replay. Owns the OnPickupDone hook, which also refreshes the tracker's Player* on every vanilla pickup. grant() enqueues
// (false until a Player* is captured); drain() replays from the pre-World::Update spawn window, requeueing until the tracker has a cached position.
class ItemGranter final : public IItemGranter
{
  public:
    // is_ap_location: RandoBridge predicate that suppresses vanilla grants at randomized locations.
    // report_collected: sends the check (idempotent) when a suppressed grant is the player collecting an AP location straight through OnPickupDone with no
    // Pickup entity or ShopMenu, e.g. the train-ticket machine.
    ItemGranter(std::function<bool(int)> is_ap_location, std::function<void(int)> report_collected);
    ~ItemGranter() override;
    ItemGranter(const ItemGranter &) = delete;
    ItemGranter &operator=(const ItemGranter &) = delete;

    bool grant(int item_type, int receipt) override;
    void discard_pending() override;
    void drain();
};

} // namespace mth
