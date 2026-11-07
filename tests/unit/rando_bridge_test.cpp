#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "mocks/fake_ap_link.hpp"
#include "mth/core/ap/ap_state.hpp"
#include "mth/core/ap/rando_bridge.hpp"
#include "mth/core/save/ap_save_state.hpp"

using mth::ap_loc_id;

namespace
{
// The bridge's routing never touches the statefile, so these cases need no backing store.
mth::ApSaveState detached()
{
    return mth::ApSaveState([] { return std::nullopt; }, [](std::string_view) {});
}

// ApState is non-copyable/non-movable (mutex + atomic); populate in-place.
void connect_with(mth::ApState &s, std::vector<std::int64_t> missing)
{
    s.apply(mth::ApConnected{{}, "{}", 1, {}, std::move(missing)});
}
} // namespace

TEST_CASE("rando_bridge: valid location is sent once", "[mth][rando]")
{
    mth::test::FakeApLink link;
    link.connected = true;
    mth::ApState state;
    connect_with(state, {ap_loc_id(5), ap_loc_id(6)});
    mth::RandoBridge bridge(link, state);

    bridge.on_location_collected(5);
    REQUIRE(link.sent_locations == std::vector<std::int64_t>{ap_loc_id(5)});

    bridge.on_location_collected(5);
    REQUIRE(link.sent_locations.size() == 1);
}

TEST_CASE("rando_bridge: unknown locations and negative slots are dropped", "[mth][rando]")
{
    mth::test::FakeApLink link;
    link.connected = true;
    mth::ApState state;
    connect_with(state, {ap_loc_id(0), ap_loc_id(5)});
    mth::RandoBridge bridge(link, state);

    bridge.on_location_collected(7);
    bridge.on_location_collected(-1);
    REQUIRE(link.sent_locations.empty());
}

TEST_CASE("rando_bridge: persists checks and flushes the full set", "[mth][rando]")
{
    mth::ApSaveState save = detached();

    mth::test::FakeApLink link;
    link.connected = true;
    mth::ApState state;
    connect_with(state, {ap_loc_id(5), ap_loc_id(9)});
    mth::RandoBridge bridge(link, state);
    bridge.attach_save_state(save);

    bridge.on_location_collected(5);
    REQUIRE(save.is_checked(5));
    REQUIRE(link.sent_locations == std::vector<std::int64_t>{ap_loc_id(5)});
    bridge.on_location_collected(5);
    REQUIRE(link.sent_locations == std::vector<std::int64_t>{ap_loc_id(5)});

    bridge.on_location_collected(9);
    link.sent_locations.clear();
    bridge.flush(); // resend the whole set
    // Order is the checked set's, and the server dedups anyway, so only membership and count matter.
    REQUIRE(std::set<std::int64_t>(link.sent_locations.begin(), link.sent_locations.end()) == std::set<std::int64_t>{ap_loc_id(5), ap_loc_id(9)});
    REQUIRE(link.sent_locations.size() == 2);
}

TEST_CASE("rando_bridge: disconnected checks persist, flush on connect", "[mth][rando]")
{
    mth::ApSaveState save = detached();

    mth::test::FakeApLink link;
    link.connected = false;
    mth::ApState state;
    connect_with(state, {ap_loc_id(5)});
    mth::RandoBridge bridge(link, state);
    bridge.attach_save_state(save);

    bridge.on_location_collected(5);
    REQUIRE(save.is_checked(5));
    REQUIRE(link.sent_locations.empty());

    link.connected = true;
    bridge.flush();
    REQUIRE(link.sent_locations == std::vector<std::int64_t>{ap_loc_id(5)});
}

TEST_CASE("rando_bridge: is_ap_location reflects the server set", "[mth][rando]")
{
    mth::test::FakeApLink link;
    mth::ApState state;
    connect_with(state, {ap_loc_id(5)});
    mth::RandoBridge bridge(link, state);
    REQUIRE(bridge.is_ap_location(5));
    REQUIRE_FALSE(bridge.is_ap_location(6));
    REQUIRE_FALSE(bridge.is_ap_location(-1));
}

TEST_CASE("rando_bridge: is_checked reflects collected locations, with or without a save", "[mth][rando]")
{
    mth::test::FakeApLink link;
    link.connected = true;
    mth::ApState state;
    connect_with(state, {ap_loc_id(5), ap_loc_id(9)});

    mth::RandoBridge session_only(link, state);
    REQUIRE_FALSE(session_only.is_checked(5));
    session_only.on_location_collected(5);
    REQUIRE(session_only.is_checked(5));

    mth::ApSaveState save = detached();
    mth::RandoBridge bridge(link, state);
    bridge.attach_save_state(save);
    REQUIRE_FALSE(bridge.is_checked(5));
    bridge.on_location_collected(5);
    REQUIRE(bridge.is_checked(5));
    REQUIRE(save.is_checked(5));
    REQUIRE_FALSE(bridge.is_checked(9));
    REQUIRE_FALSE(bridge.is_checked(-1));
}

TEST_CASE("rando_bridge: send_goal sends the AP goal once, and only when authenticated", "[mth][rando]")
{
    mth::test::FakeApLink link;
    link.connected = true;
    mth::ApState never_connected;
    mth::RandoBridge offline(link, never_connected);
    offline.send_goal();
    REQUIRE(link.goal_calls == 0);

    mth::ApState state;
    connect_with(state, {});
    mth::RandoBridge bridge(link, state);
    bridge.send_goal();
    bridge.send_goal(); // one-shot: GigaLionel hits the death funnels 3x per kill
    REQUIRE(link.goal_calls == 1);
}

TEST_CASE("rando_bridge: reconcile_server_checked marks AP locations once, without sending", "[mth][rando]")
{
    mth::ApSaveState save = detached();
    mth::test::FakeApLink link;
    link.connected = true;
    mth::ApState state;
    connect_with(state, {ap_loc_id(5), ap_loc_id(9)});
    mth::RandoBridge bridge(link, state);
    bridge.attach_save_state(save);

    REQUIRE(bridge.reconcile_server_checked(5));
    REQUIRE(bridge.is_checked(5));
    REQUIRE(save.is_checked(5));
    REQUIRE_FALSE(bridge.reconcile_server_checked(5));
    REQUIRE_FALSE(bridge.reconcile_server_checked(7));
    REQUIRE_FALSE(bridge.reconcile_server_checked(-1));
    REQUIRE(link.sent_locations.empty());
}

TEST_CASE("rando_bridge: reconcile_server_checked is a no-op without a save", "[mth][rando]")
{
    mth::test::FakeApLink link;
    mth::ApState state;
    connect_with(state, {ap_loc_id(5)});
    mth::RandoBridge bridge(link, state); // no attach_save_state

    REQUIRE_FALSE(bridge.reconcile_server_checked(5));
    REQUIRE_FALSE(bridge.is_checked(5));
    REQUIRE(link.sent_locations.empty());
}

TEST_CASE("rando_bridge: detach stops writing the released save and clears its dedup", "[mth][rando]")
{
    mth::test::FakeApLink link;
    link.connected = true;
    mth::ApState state;
    connect_with(state, {ap_loc_id(5), ap_loc_id(6)});
    mth::RandoBridge bridge(link, state);

    mth::ApSaveState save = detached();
    bridge.attach_save_state(save);
    bridge.on_location_collected(5);
    REQUIRE(save.is_checked(5));

    bridge.detach_save_state();
    bridge.on_location_collected(6);
    REQUIRE_FALSE(save.is_checked(6));
}

TEST_CASE("rando_bridge: a new server's flush never resends the previous save's checks (#124)", "[mth][rando]")
{
    mth::test::FakeApLink link;
    link.connected = true;
    mth::ApState state;
    connect_with(state, {ap_loc_id(165), ap_loc_id(186)});
    mth::RandoBridge bridge(link, state);

    mth::ApSaveState save_a = detached();
    bridge.attach_save_state(save_a);
    bridge.on_location_collected(165);
    bridge.on_location_collected(186);
    link.sent_locations.clear();

    // Explicit connect to a different server: the session clear releases A's save before B's attaches.
    bridge.reset_session();
    mth::ApSaveState save_b = detached();
    bridge.attach_save_state(save_b);
    bridge.flush();
    REQUIRE(link.sent_locations.empty());
}

TEST_CASE("rando_bridge: reset_session re-arms the one-shot goal for the next server", "[mth][rando]")
{
    mth::test::FakeApLink link;
    link.connected = true;
    mth::ApState state;
    connect_with(state, {ap_loc_id(5)});
    mth::RandoBridge bridge(link, state);

    bridge.send_goal();
    bridge.send_goal();
    REQUIRE(link.goal_calls == 1);

    bridge.reset_session();
    bridge.send_goal();
    REQUIRE(link.goal_calls == 2);
}

TEST_CASE("rando_bridge: a removed location reads as a checked AP location", "[mth][rando]")
{
    mth::test::FakeApLink link;
    link.connected = true;
    mth::ApState state;
    state.apply(mth::ApConnected{.slot_data = "{}", .player_slot = 1, .missing_locations = {ap_loc_id(5)}, .config = {.removed_locations = {ap_loc_id(40)}}});
    mth::RandoBridge bridge(link, state);

    REQUIRE(bridge.is_ap_location(40));
    REQUIRE(bridge.is_checked(40));
    REQUIRE(bridge.is_removed(40));
    bridge.on_location_collected(40);
    REQUIRE(link.sent_locations.empty());
    REQUIRE(bridge.is_ap_location(5));
    REQUIRE_FALSE(bridge.is_checked(5));
    REQUIRE_FALSE(bridge.is_removed(5));
    REQUIRE_FALSE(bridge.is_removed(-1));
}

TEST_CASE("rando_bridge: a removed location is never persisted, sent, or flushed", "[mth][rando]")
{
    mth::ApSaveState save = detached();

    mth::test::FakeApLink link;
    link.connected = true;
    mth::ApState state;
    state.apply(mth::ApConnected{.slot_data = "{}", .player_slot = 1, .missing_locations = {ap_loc_id(5)}, .config = {.removed_locations = {ap_loc_id(40)}}});
    mth::RandoBridge bridge(link, state);
    bridge.attach_save_state(save);

    bridge.on_location_collected(40);
    REQUIRE(link.sent_locations.empty());
    REQUIRE_FALSE(save.is_checked(40));

    bridge.on_location_collected(5); // a real check still works
    link.sent_locations.clear();
    bridge.flush();
    REQUIRE(link.sent_locations == std::vector<std::int64_t>{ap_loc_id(5)});
}

TEST_CASE("rando_bridge: a removed location is not reconciled or scouted", "[mth][rando]")
{
    mth::ApSaveState save = detached();

    mth::test::FakeApLink link;
    link.connected = true;
    mth::ApState state;
    state.apply(mth::ApConnected{.slot_data = "{}", .player_slot = 1, .missing_locations = {ap_loc_id(5)}, .config = {.removed_locations = {ap_loc_id(40)}}});
    mth::RandoBridge bridge(link, state);
    bridge.attach_save_state(save);

    REQUIRE_FALSE(bridge.reconcile_server_checked(40));
    REQUIRE_FALSE(save.is_checked(40));

    bridge.request_scouts({5, 40});
    REQUIRE(link.scouted_locations == std::vector<std::int64_t>{ap_loc_id(5)});

    REQUIRE(bridge.removed_slots() == std::set<std::int64_t>{ap_loc_id(40)});
}

TEST_CASE("rando_bridge: flush excludes a removed id even from a stale statefile", "[mth][rando]")
{
    // Loaded content the bridge never wrote, standing in for a statefile that predates the seed's
    // prune: slot 40 is checked in it despite slot_data now marking it removed.
    mth::ApSaveState save([] { return std::optional<std::string>("c 5\nc 40\n"); }, [](std::string_view) {});

    mth::test::FakeApLink link;
    link.connected = true;
    mth::ApState state;
    state.apply(mth::ApConnected{.slot_data = "{}", .player_slot = 1, .missing_locations = {ap_loc_id(5)}, .config = {.removed_locations = {ap_loc_id(40)}}});
    mth::RandoBridge bridge(link, state);
    bridge.attach_save_state(save);

    bridge.flush();
    REQUIRE(link.sent_locations == std::vector<std::int64_t>{ap_loc_id(5)});
}
