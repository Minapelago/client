#pragma once

#include <functional>
#include <map>

#include "mth/core/ap/ap_events.hpp"
#include "pal/scoped_hook.hpp"

namespace mth
{

class RandoBridge;

// Outbound location checks from world pickups and shop buys: Pickup::Init (AP-dummy
// redirect + checked-location despawn), Pickup::OnPickup (collect detect), and the
// PAL shop-purchase hook. Also patches the AP dummy item row at construction.
class LocationHooks
{
  public:
    // `scout` may be null. When set, shop box text (name/desc/color) comes from scouted AP data; when null, the shop text hook is not installed.
    explicit LocationHooks(RandoBridge &bridge, const std::map<int, ScoutInfo> *scout = nullptr);
    LocationHooks(const LocationHooks &) = delete;
    LocationHooks &operator=(const LocationHooks &) = delete;

    // Kear key gating per slot_data "kear_rando" mode. `neutralize` (all AP modes): cancel the free key a
    // kear-location collect would grant. `suppress` (AP-item modes only): pin usable keys to 0 via reconcile.
    void set_kear_gating(bool neutralize, bool suppress);

    // Live Player* accessor for the kear key edits, which move the Player+0x11b0 spent mirror alongside SaveSlot+0x1f8. Wired once by App via HookManager.
    void set_player_getter(std::function<void *()> get_player);

    // Vanilla kear mode (#130): grants one usable key for a received Universal Kear by lowering both the SaveSlot and Player spent-counters. false (retry)
    // until a save and player are live. Game thread; the caller owns idempotency (per-receipt marker).
    bool credit_kear_key(void *player);

    // Re-raises the SaveSlot kear spent-counter to popcount so AP-collected kears do not read as usable keys after a load. Game thread, per tick.
    void reconcile_kear_keys();

    // Writes the native durable collected bit for server-collected (Collect/coop) durable-bit locations so their chests spawn opened; reconcile alone only
    // marks our .state. Game thread, per tick; no-op without an active save.
    void enforce_native_bits();

    // Re-arms enforce_native_bits after the active SaveSlot is replaced, which discards the bits written into it.
    void reset_native_bits();

  private:
    pal::ScopedHook pickup_init_;
    pal::ScopedHook shop_oos_; // brackets Shop::IsOutOfStock so the IsItemCollected override can scope to it (#67)
};

} // namespace mth
