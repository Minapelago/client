#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "mth/core/ap/ap_link.hpp"
#include "mth/core/ap/connect_target.hpp"

class APClient; // full type only in the .cpp

namespace mth::net
{

// apclientpp-backed IApLink. APClient lives on the net thread only.
class ApLink final : public mth::IApLink
{
  public:
    ApLink();
    ~ApLink() override;
    ApLink(const ApLink &) = delete;
    ApLink &operator=(const ApLink &) = delete;

    void connect(const std::string &server, const std::string &slot, const std::string &password) override;
    void disconnect() override;
    [[nodiscard]] bool is_connected() const override;
    void send_locations(const std::vector<std::int64_t> &location_ids) override;
    void scout_locations(const std::vector<std::int64_t> &location_ids) override;
    void set_goal() override;
    void enable_deathlink(bool on) override;
    void send_death(const std::string &detail) override;
    void report_area(int game_state) override;
    [[nodiscard]] std::vector<mth::ApEvent> drain_events() override;

  private:
    void run();
    void enqueue(std::function<void()> cmd);
    void push_event(mth::ApEvent ev);

    void do_connect(const std::string &server, const std::string &slot, const std::string &password);
    void do_disconnect();
    void setup_handlers(const std::string &slot, const std::string &password, mth::TlsMode tls);

    std::unique_ptr<APClient> client_; // net thread only
    bool encrypted_attempt_{true};     // net thread only; mirrors which scheme apclientpp will try next
    int last_item_index_{-1};          // net thread only
    // Server-reported identity of this process's AP session. It survives disconnects; only a different seed/slot authenticating ends it. Empty until the
    // first connect, which always counts as a new session.
    std::string session_seed_;                                              // net thread only
    int session_slot_{-1};                                                  // net thread only
    std::string slot_name_;                                                 // net thread only; captured on connect for bounce source
    std::optional<std::chrono::steady_clock::time_point> connect_deadline_; // net thread only; set while a connect is in flight
    bool teardown_pending_{false};                                          // net thread only; run() drops client_ after the poll that set it

    std::atomic<bool> running_{true};
    std::atomic<bool> connected_{false};
    // Effective deathlink = slot_deathlink_ && !force_off_, cached in deathlink_ for the send/receive/tag paths. force_off_ is the sticky console opt-out
    // (survives reconnect); slot_deathlink_ keeps slot_data "death_link" so clearing the opt-out defers to it.
    std::atomic<bool> deathlink_{false};
    std::atomic<bool> force_off_{false};
    std::atomic<bool> slot_deathlink_{false};
    std::mutex cmd_mutex_;
    std::queue<std::function<void()>> commands_;
    std::mutex event_mutex_;
    std::vector<mth::ApEvent> events_;

    std::thread thread_; // last member: started after all others are initialized
};

} // namespace mth::net
