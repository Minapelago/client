#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "mth/core/ap/broadcast.hpp"
#include "mth/core/ap/slot_data.hpp"

namespace mth
{

// One scouted shop location: what AP item it holds, for whom, and its classification flags.
// Strings are resolved in the link layer (net thread) and carried here as plain data.
struct ScoutInfo
{
    int collection_slot{};
    std::string item_name;
    std::string player_alias;
    std::string player_game;
    unsigned item_flags{};
    bool is_self{};
};

// Description-widget text for a scouted location: recipient + their game, or a self form.
[[nodiscard]] inline std::string format_scout_desc(const ScoutInfo &info)
{
    if (info.is_self)
        return "for you";
    return "for " + info.player_alias + " (" + info.player_game + ")";
}

// Scouted locations by collection slot. Null when the slot has not been scouted.
[[nodiscard]] inline const ScoutInfo *find_scout(const std::map<int, ScoutInfo> &scouts, int collection_slot)
{
    const auto it = scouts.find(collection_slot);
    return it != scouts.end() ? &it->second : nullptr;
}

struct ReceivedItem
{
    std::int64_t item_id{};
    int index{}; // monotonic dedup cursor
    int player_from{};
    unsigned flags{};
};

// Inbound events: produced by the net thread, applied on the game thread.
struct ApConnected
{
    std::string seed;      // persistence key (with player_slot)
    std::string slot_data; // raw JSON, logged by size only; config below is the parsed form
    int player_slot{-1};
    std::vector<std::int64_t> checked_locations;
    std::vector<std::int64_t> missing_locations;
    SlotDataConfig config{}; // everything read out of slot_data (deathlink already force-off adjusted)
};
struct ApItemReceived
{
    ReceivedItem item;
};
// Locations the server reports checked (Connected full set + RoomUpdate deltas): Collect, same-slot coop, or connect-time self-heal.
// Reconciled locally without re-sending.
struct ApLocationsChecked
{
    std::vector<std::int64_t> ap_location_ids;
};
struct ApConnecting
{
};
struct ApDisconnected
{
};
struct ApConnectionRefused
{
    std::vector<std::string> errors;
};
struct ApStatusChanged
{
    std::string text;
};
struct ApDeathReceived
{
    std::string source; // who died: a slot name, or a character name from within the sender's game
    std::string cause;  // full sentence already naming the dead player; empty when the sender supplied none
};
// Filtered and pre-rendered on the net thread (apclientpp resolves names only there); the coordinator forwards it to the banner.
struct ApPrintBroadcast
{
    std::vector<BannerSegment> segments;
};
// Scouted shop locations, resolved to display strings on the net thread; App fills its scout map from them on the game thread.
struct ApScoutInfo
{
    std::vector<ScoutInfo> locations;
};

// The server authenticated a different (seed, player_slot) than this process was on. Emitted just before that connection's ApConnected so the old
// session drops first. Not emitted on a same seed+slot reconnect.
struct ApSessionEnded
{
};

using ApEvent = std::variant<ApConnected, ApConnecting, ApItemReceived, ApLocationsChecked, ApDisconnected, ApConnectionRefused, ApStatusChanged,
                             ApDeathReceived, ApPrintBroadcast, ApScoutInfo, ApSessionEnded>;

} // namespace mth
