#include <algorithm>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "mth/core/ap/ability_ids.hpp"
#include "mth/core/ap/ap_state.hpp"

TEST_CASE("ap_state: ApConnected populates slot/locations and authenticates", "[mth][ap_state]")
{
    mth::ApState s;
    REQUIRE_FALSE(s.authenticated());
    s.apply(mth::ApConnected{{}, R"({"x":1})", 3, {10, 11}, {12}});
    REQUIRE(s.authenticated());
    REQUIRE(s.player_slot() == 3);
    REQUIRE(s.is_valid_location(10));
    REQUIRE(s.is_valid_location(12));
    REQUIRE_FALSE(s.is_valid_location(99));
}

// slot_data "kear_rando" is an int mode: 0 = vanilla (the pool carries Universal Kear items, id 63, which must grant real keys), 1/2 = per-lock /
// per-area AP items (usable keys stay pinned at zero). See #130.
TEST_CASE("ap_state: only the AP-item kear modes suppress usable keys", "[mth][ap_state]")
{
    mth::ApState vanilla;
    vanilla.apply(mth::ApConnected{.slot_data = "{}", .player_slot = 1, .config = {.kear_mode = mth::KearMode::Vanilla}});
    REQUIRE_FALSE(vanilla.kear_keys_suppressed());

    mth::ApState s;
    s.apply(mth::ApConnected{.slot_data = "{}", .player_slot = 1, .config = {.kear_mode = mth::KearMode::ApItems}});
    REQUIRE(s.kear_keys_suppressed());

    mth::ApState area;
    area.apply(mth::ApConnected{.slot_data = "{}", .player_slot = 1, .config = {.kear_mode = mth::KearMode::AreaApItems}});
    REQUIRE(area.kear_keys_suppressed());
}

TEST_CASE("ap_state: items dedup by index", "[mth][ap_state]")
{
    mth::ApState s;
    s.apply(mth::ApItemReceived{{1001, 0, 1, 0b001}});
    s.apply(mth::ApItemReceived{{1002, 1, 1, 0b010}});
    s.apply(mth::ApItemReceived{{1001, 0, 1, 0b001}}); // duplicate index 0
    REQUIRE(s.received_items().size() == 2);
    REQUIRE(s.received_items()[1].item_id == 1002);
}

TEST_CASE("ap_state: has_received matches by item_id not index", "[mth][ap_state]")
{
    mth::ApState s;
    s.apply(mth::ApItemReceived{{3001, 0, 1, 0}});
    REQUIRE(s.has_received(3001));
    REQUIRE_FALSE(s.has_received(3000));
    REQUIRE_FALSE(s.has_received(0));
}

TEST_CASE("ap_state: each connection event maps to its phase", "[mth][ap_state]")
{
    using mth::ConnectionPhase;
    mth::ApState s;
    REQUIRE(s.phase() == ConnectionPhase::Disconnected);
    REQUIRE(s.detail().empty());

    s.apply(mth::ApConnecting{});
    REQUIRE(s.phase() == ConnectionPhase::Connecting);
    REQUIRE(s.detail().empty());

    s.apply(mth::ApConnected{{}, "{}", 1, {}, {}});
    REQUIRE(s.phase() == ConnectionPhase::Connected);

    s.apply(mth::ApDisconnected{});
    REQUIRE(s.phase() == ConnectionPhase::Disconnected);
    REQUIRE_FALSE(s.authenticated());

    // The login window shows detail() verbatim, so an error the join drops is a refusal the player
    // never gets an explanation for.
    s.apply(mth::ApConnectionRefused{{"bad slot", "bad pw"}});
    REQUIRE(s.phase() == ConnectionPhase::Error);
    REQUIRE(s.detail().find("bad slot") != std::string::npos);
    REQUIRE(s.detail().find("bad pw") != std::string::npos);
}

TEST_CASE("ap_state: ApLocationsChecked accumulates and drains once", "[mth][ap_state]")
{
    mth::ApState s;
    s.apply(mth::ApLocationsChecked{{10, 11}});
    s.apply(mth::ApLocationsChecked{{12}});

    // App::reconcile_server_checked handles each id on its own, so the batch order is not a contract.
    // A duplicate is, since it would reconcile the same location twice.
    const std::vector<std::int64_t> batch = s.take_server_checked_pending();
    REQUIRE(batch.size() == 3);
    REQUIRE(std::count(batch.begin(), batch.end(), 10) == 1);
    REQUIRE(std::count(batch.begin(), batch.end(), 11) == 1);
    REQUIRE(std::count(batch.begin(), batch.end(), 12) == 1);
    REQUIRE(s.take_server_checked_pending().empty());
}

TEST_CASE("ap_state: reset_session drops the session stream but keeps the connection identity", "[mth][ap_state]")
{
    mth::ApState s;
    s.apply(mth::ApConnected{{}, R"({"a":1})", 7, {}, {mth::ap_loc_id(5)}});
    s.apply(mth::ApItemReceived{mth::ReceivedItem{42, 0, 1, 0}});
    s.apply(mth::ApLocationsChecked{{10}});
    REQUIRE(s.received_items().size() == 1);

    s.reset_session();

    REQUIRE(s.received_items().empty());
    REQUIRE(s.take_server_checked_pending().empty());
    // Deauthenticated, so nothing keyed off the previous seed can be rebuilt before the next ApConnected.
    REQUIRE_FALSE(s.authenticated());

    // The next connection's ApConnected restores a full identity over the cleared state.
    s.apply(mth::ApConnected{{}, R"({"b":2})", 4, {}, {mth::ap_loc_id(6)}});
    REQUIRE(s.authenticated());
    REQUIRE(s.player_slot() == 4);
}

TEST_CASE("ap_state: an item index reused by the next server is not swallowed as a duplicate", "[mth][ap_state]")
{
    mth::ApState s;
    s.apply(mth::ApConnected{{}, "{}", 1, {}, {mth::ap_loc_id(5)}});
    s.apply(mth::ApItemReceived{mth::ReceivedItem{42, 3, 1, 0}});
    REQUIRE(s.received_items().size() == 1);

    s.reset_session();
    s.apply(mth::ApItemReceived{mth::ReceivedItem{99, 3, 1, 0}}); // same index, different server
    REQUIRE(s.received_items().size() == 1);
    REQUIRE(s.received_items()[0].item_id == 99);
}

TEST_CASE("ap_state: removed locations are tracked and cleared on session reset", "[mth][ap_state]")
{
    mth::ApState s;
    s.apply(mth::ApConnected{.slot_data = "{}", .player_slot = 1, .missing_locations = {5, 6}, .config = {.removed_locations = {40, 41}}});
    REQUIRE(s.is_removed_location(40));
    REQUIRE(s.is_removed_location(41));
    REQUIRE_FALSE(s.is_removed_location(5));
    REQUIRE(s.removed_locations().size() == 2);

    s.reset_session();
    REQUIRE_FALSE(s.is_removed_location(40));
    REQUIRE(s.removed_locations().empty());
}

TEST_CASE("ap_state: a removed id the server still lists stays a real check", "[mth][ap_state]")
{
    mth::ApState s;
    s.apply(mth::ApConnected{
        .slot_data = "{}", .player_slot = 1, .checked_locations = {7}, .missing_locations = {5, 6}, .config = {.removed_locations = {5, 7, 40}}});

    REQUIRE_FALSE(s.is_removed_location(5));
    REQUIRE_FALSE(s.is_removed_location(7));
    REQUIRE(s.is_removed_location(40));
    REQUIRE(s.removed_locations().size() == 1);
    REQUIRE(s.is_valid_location(5));
}

TEST_CASE("ap_state: a disconnect keeps the removed set", "[mth][ap_state]")
{
    mth::ApState s;
    s.apply(mth::ApConnected{.slot_data = "{}", .player_slot = 1, .missing_locations = {5}, .config = {.removed_locations = {40}}});
    s.apply(mth::ApDisconnected{});

    REQUIRE_FALSE(s.authenticated());
    REQUIRE(s.is_removed_location(40));
}
