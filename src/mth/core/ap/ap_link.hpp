#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mth/core/ap/ap_events.hpp"

namespace mth
{

// MTH_<kind>_<team>_<slot>: "level" holds the slot's room (Universal Tracker reads it too), "ghost" carries its ghost packets.
[[nodiscard]] inline std::string datastore_key(std::string_view kind, int team, int slot)
{
    return "MTH_" + std::string(kind) + "_" + std::to_string(team) + "_" + std::to_string(slot);
}

// Transport seam: outbound calls enqueue work; inbound traffic surfaces as ApEvents.
class IApLink
{
  public:
    virtual ~IApLink() = default;

    virtual void connect(const std::string &server, const std::string &slot, const std::string &password) = 0;
    virtual void disconnect() = 0;
    [[nodiscard]] virtual bool is_connected() const = 0;

    virtual void send_locations(const std::vector<std::int64_t> &location_ids) = 0;
    // Request scout (item/player) info for locations; results arrive as an ApScoutInfo event.
    virtual void scout_locations(const std::vector<std::int64_t> &location_ids) = 0;
    virtual void set_goal() = 0;

    virtual void enable_deathlink(bool on) = 0; // tag the connection; call before connect()
    // `detail` is the predicate only; the implementation prefixes our slot name.
    virtual void send_death(const std::string &detail) = 0;

    // Publish the current room/area id to our own "level" datastore_key.
    virtual void report_area(int game_state) = 0;

    // watch() sends SetNotify, plus a Get when `get` is set; replies arrive as ApStored. unwatch() sends nothing yet (see ApLink::unwatch).
    // send_ghost() Sets `key` with no operations and the packet in the extra field "g", so the stored value stays 0.
    virtual void watch(const std::vector<std::string> &keys, bool get) = 0;
    virtual void unwatch(const std::vector<std::string> &keys) = 0;
    virtual void send_ghost(const std::string &key, std::vector<std::int64_t> packet) = 0;

    [[nodiscard]] virtual std::vector<ApEvent> drain_events() = 0;
};

} // namespace mth
