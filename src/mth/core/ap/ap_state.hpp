#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mth/core/ap/ap_events.hpp"

namespace mth
{

enum class ConnectionPhase
{
    Disconnected,
    Connecting,
    Connected,
    Error
};

// Game-thread-only view of the AP room. Written by ApCoordinator, read by game logic.
class ApState
{
  public:
    void apply(const ApEvent &ev);

    // Dev/console seam: append a received item not from the socket. Uses a private negative index so
    // it never collides with or advances the server's monotonic cursor.
    void inject_received_item(std::int64_t item_id);

    [[nodiscard]] bool authenticated() const
    {
        return authenticated_;
    }
    // By value under the lock: the overlay render thread reads this while the game thread reassigns it.
    [[nodiscard]] std::string status() const
    {
        std::lock_guard<std::mutex> lk(text_mutex_);
        return status_;
    }
    [[nodiscard]] ConnectionPhase phase() const
    {
        return phase_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::string detail() const
    {
        std::lock_guard<std::mutex> lk(text_mutex_);
        return detail_;
    }
    [[nodiscard]] const std::string &seed() const
    {
        return seed_;
    }
    [[nodiscard]] int player_slot() const
    {
        return player_slot_;
    }
    [[nodiscard]] const SlotDataConfig &config() const
    {
        return config_;
    }
    // Usable keys are meaningless unless the pool carries Universal Kear items, so every mode but Vanilla
    // pins them to zero. The world-kear collect grant is neutralized in ALL modes (the spot is an AP
    // location, so the server owns its reward).
    [[nodiscard]] bool kear_keys_suppressed() const
    {
        return config_.kear_mode != KearMode::Vanilla;
    }
    [[nodiscard]] bool is_valid_location(std::int64_t id) const
    {
        return valid_locations_.contains(id);
    }
    // slot_data "removed_locations": pruned from the pool, so the client treats them as already
    // collected. Disjoint from valid_locations_ by construction (see apply): the server's view wins.
    [[nodiscard]] bool is_removed_location(std::int64_t id) const
    {
        return removed_locations_.contains(id);
    }
    [[nodiscard]] const std::set<std::int64_t> &removed_locations() const
    {
        return removed_locations_;
    }
    [[nodiscard]] const std::vector<ReceivedItem> &received_items() const
    {
        return received_items_;
    }
    // Drain the server-reported checked location ids accumulated since the last call (Collect / coop).
    // Game-thread only; App reconciles these into the save-state checked set, then this returns empty.
    [[nodiscard]] std::vector<std::int64_t> take_server_checked_pending()
    {
        return std::exchange(server_checked_pending_, {});
    }
    // True if item_id appears in the received inventory (reconnect-durable: server resends full list).
    [[nodiscard]] bool has_received(std::int64_t item_id) const
    {
        for (const auto &it : received_items_)
            if (it.item_id == item_id)
                return true;
        return false;
    }
    // Drop everything a connection accumulates, so the next starts as it would have at launch. Must run
    // before the next connection's events are applied, which is why the ApSessionEnded marker driving it is
    // ordered ahead of ApConnected; running it after would discard the new server's item stream and its
    // authoritative checked-location report.
    void reset_session();

  private:
    // Store the phase and clear detail_ under its lock (non-error transitions).
    void set_phase(ConnectionPhase p);
    void set_status(std::string text);

    bool authenticated_{false};
    std::string status_{"Idle"};
    std::atomic<ConnectionPhase> phase_{ConnectionPhase::Disconnected};
    std::string detail_{};          // human-readable error/status detail for the login window
    mutable std::mutex text_mutex_; // guards detail_ and status_ across the game/render thread boundary
    std::string seed_{};
    int player_slot_{-1};
    SlotDataConfig config_{}; // pre-connection defaults; every reader gates on authenticated() first
    std::set<std::int64_t> valid_locations_{};
    // config_.removed_locations filtered against valid_locations_ (see apply); config_ keeps the server's unfiltered list.
    std::set<std::int64_t> removed_locations_{};
    std::vector<ReceivedItem> received_items_{};
    std::vector<std::int64_t> server_checked_pending_{}; // server-reported checks awaiting reconcile (game-thread)
    int last_item_index_{-1};
    int console_index_{-1000000}; // synthetic index for console-injected items; decremented, never collides with server's >=0 cursor
};

} // namespace mth
