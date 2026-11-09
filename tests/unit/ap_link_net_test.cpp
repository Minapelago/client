// The real ApLink against a scripted Archipelago server over loopback.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "mocks/scripted_ap_server.hpp"
#include "mth/core/ap/ap_events.hpp"
#include "mth/net/ap_link_apclient.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{

constexpr std::chrono::seconds kWaitTimeout{5};

template <class F> bool wait_until(F &&done)
{
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (!done())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Points the uuid file (XDG_DATA_HOME) and apclientpp's data package cache (XDG_CACHE_HOME) at a scratch directory for the fixture's lifetime.
class ScratchHome
{
  public:
    ScratchHome() : dir_(fs::temp_directory_path() / ("mthap_net_test_" + std::to_string(::getpid())))
    {
        fs::create_directories(dir_ / "mth-apclient");
        override_env("XDG_DATA_HOME", old_data_);
        override_env("XDG_CACHE_HOME", old_cache_);
    }
    ~ScratchHome()
    {
        restore_env("XDG_DATA_HOME", old_data_);
        restore_env("XDG_CACHE_HOME", old_cache_);
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    ScratchHome(const ScratchHome &) = delete;
    ScratchHome &operator=(const ScratchHome &) = delete;

  private:
    void override_env(const char *name, std::optional<std::string> &old)
    {
        if (const char *v = std::getenv(name))
            old = v;
        ::setenv(name, dir_.c_str(), 1);
    }
    static void restore_env(const char *name, const std::optional<std::string> &old)
    {
        if (old)
            ::setenv(name, old->c_str(), 1);
        else
            ::unsetenv(name);
    }

    fs::path dir_;
    std::optional<std::string> old_data_;
    std::optional<std::string> old_cache_;
};

json connected(int slot, json slot_data, std::vector<std::int64_t> checked = {}, std::vector<std::int64_t> missing = {})
{
    return {{"cmd", "Connected"},
            {"team", 0},
            {"slot", slot},
            {"players", json::array({{{"team", 0}, {"slot", slot}, {"alias", "Mina"}, {"name", "Mina"}}})},
            {"checked_locations", checked},
            {"missing_locations", missing},
            {"slot_data", std::move(slot_data)},
            {"slot_info", json::object()},
            {"hint_points", 0}};
}

json received_items(int index, const std::vector<std::int64_t> &item_ids)
{
    json items = json::array();
    for (std::int64_t id : item_ids)
        items.push_back({{"item", id}, {"location", 7}, {"player", 2}, {"flags", 0}});
    return {{"cmd", "ReceivedItems"}, {"index", index}, {"items", std::move(items)}};
}

json death_bounce(const std::string &source)
{
    return {{"cmd", "Bounced"}, {"tags", {"DeathLink"}}, {"data", {{"time", 1.5}, {"source", source}, {"cause", source + " fell"}}}};
}

// Destruction order matters: the link disconnects while the server is still up, then the server stops, then the environment is restored.
struct LinkFixture
{
    ScratchHome home;
    mth::test::ScriptedApServer server;
    mth::net::ApLink link;
    std::vector<mth::ApEvent> events;

    void connect(const std::string &slot = "Mina")
    {
        link.connect("localhost:" + std::to_string(server.port()), slot, "");
    }

    template <class T> [[nodiscard]] std::vector<const T *> all(std::size_t from = 0) const
    {
        std::vector<const T *> out;
        for (std::size_t i = from; i < events.size(); ++i)
            if (const T *ev = std::get_if<T>(&events[i]))
                out.push_back(ev);
        return out;
    }

    // Index into `events` of the first T at or after `from`, or events.size() when there is none.
    template <class T> [[nodiscard]] std::size_t index_of(std::size_t from = 0) const
    {
        for (std::size_t i = from; i < events.size(); ++i)
            if (std::holds_alternative<T>(events[i]))
                return i;
        return events.size();
    }

    void pump()
    {
        for (mth::ApEvent &ev : link.drain_events())
            events.push_back(std::move(ev));
    }

    // Drains the link until `count` events of type T have arrived since `from`, returning a copy of the last; fails the test on timeout.
    template <class T> T wait_for(const char *what, std::size_t count = 1, std::size_t from = 0)
    {
        const bool ok = wait_until(
            [&]
            {
                pump();
                return all<T>(from).size() >= count;
            });
        if (!ok)
            FAIL("timed out waiting for " << what);
        return *all<T>(from)[count - 1];
    }

    void wait_for_sent(const std::string &cmd, std::size_t count = 1)
    {
        if (!wait_until([&] { return server.received(cmd).size() >= count; }))
            FAIL("timed out waiting for the client to send " << cmd);
    }
};

std::vector<std::int64_t> item_ids(const std::vector<const mth::ApItemReceived *> &items)
{
    std::vector<std::int64_t> out;
    for (const mth::ApItemReceived *it : items)
        out.push_back(it->item.item_id);
    return out;
}

} // namespace

TEST_CASE("ApLink: a Connected reply emits ApConnected with the parsed slot_data", "[net][ap_link]")
{
    LinkFixture f;
    f.server.reply_to("Connect", {connected(3, {{"goal_config", 1}, {"goal_generators", 2}, {"death_link", true}, {"kear_rando", 0}}, {5}, {6, 7})});
    f.connect();

    const mth::ApConnected c = f.wait_for<mth::ApConnected>("ApConnected");
    REQUIRE(c.seed == "seed-a");
    REQUIRE(c.player_slot == 3);
    REQUIRE(c.checked_locations == std::vector<std::int64_t>{5});
    REQUIRE(c.missing_locations == std::vector<std::int64_t>{6, 7});
    REQUIRE(c.config.goal_config == 1);
    REQUIRE(c.config.goal_generators == 2);
    REQUIRE(c.config.deathlink);
    REQUIRE(c.config.kear_mode == mth::KearMode::Vanilla);

    const json connect = f.server.received("Connect").at(0);
    REQUIRE(connect.at("game") == "Mina The Hollower");
    REQUIRE(connect.at("name") == "Mina");
}

TEST_CASE("ApLink: ConnectionRefused emits ApConnectionRefused with the server's errors", "[net][ap_link]")
{
    LinkFixture f;
    f.server.reply_to("Connect", {{{"cmd", "ConnectionRefused"}, {"errors", {"InvalidSlot"}}}});
    f.connect();

    const mth::ApConnectionRefused r = f.wait_for<mth::ApConnectionRefused>("ApConnectionRefused");
    REQUIRE(r.errors == std::vector<std::string>{"InvalidSlot"});
    REQUIRE(f.all<mth::ApConnected>().empty());
}

TEST_CASE("ApLink: slot_data with a wrong-typed key refuses the connection", "[net][ap_link]")
{
    LinkFixture f;
    f.server.reply_to("Connect", {connected(1, {{"ossex_start", "yes"}}), received_items(0, {10})});
    f.server.reply_to("Connect", {connected(1, json::object()), received_items(0, {10})});
    f.connect();
    f.wait_for_sent("Connect");
    f.server.push(received_items(0, {11}));

    const mth::ApConnectionRefused r = f.wait_for<mth::ApConnectionRefused>("ApConnectionRefused");
    REQUIRE(r.errors.size() == 1);
    REQUIRE(r.errors[0].starts_with("invalid slot_data: "));

    // The refused attempt's events all precede the next attempt's, so the final item list also covers the bad packet.
    const std::size_t mark = f.events.size();
    f.connect();
    f.wait_for<mth::ApItemReceived>("the item after reconnect", 1, mark);

    REQUIRE(f.all<mth::ApConnectionRefused>().size() == 1);
    REQUIRE(f.all<mth::ApDisconnected>().empty());
    REQUIRE(f.all<mth::ApConnected>().size() == 1);
    REQUIRE(f.index_of<mth::ApConnected>() >= mark);
    REQUIRE(item_ids(f.all<mth::ApItemReceived>()) == std::vector<std::int64_t>{10});
}

TEST_CASE("ApLink: reconnecting to the same seed and slot keeps the session and its item cursor", "[net][ap_link]")
{
    LinkFixture f;
    f.server.reply_to("Connect", {connected(1, json::object()), received_items(0, {10, 11})});
    f.server.reply_to("Connect", {connected(1, json::object()), received_items(0, {10, 11, 12})});
    f.connect();
    f.wait_for<mth::ApItemReceived>("first two items", 2);

    const std::size_t mark = f.events.size();
    f.connect();
    f.wait_for<mth::ApItemReceived>("the new item after reconnect", 1, mark);

    REQUIRE(f.all<mth::ApConnected>(mark).size() == 1);
    REQUIRE(f.all<mth::ApSessionEnded>(mark).empty());
    REQUIRE(item_ids(f.all<mth::ApItemReceived>(mark)) == std::vector<std::int64_t>{12});
}

TEST_CASE("ApLink: reconnecting to a different seed ends the session before ApConnected and redelivers from index 0", "[net][ap_link]")
{
    LinkFixture f;
    f.server.reply_to("Connect", {connected(1, json::object()), received_items(0, {10, 11})});
    f.server.reply_to("Connect", {connected(1, json::object()), received_items(0, {20})});
    f.connect();
    f.wait_for<mth::ApItemReceived>("first seed's items", 2);

    const std::size_t mark = f.events.size();
    f.server.set_seed("seed-b");
    f.connect();
    f.wait_for<mth::ApItemReceived>("second seed's item", 1, mark);

    const std::size_t ended = f.index_of<mth::ApSessionEnded>(mark);
    REQUIRE(ended < f.index_of<mth::ApConnected>(mark));
    REQUIRE(f.all<mth::ApConnected>(mark).at(0)->seed == "seed-b");
    REQUIRE(item_ids(f.all<mth::ApItemReceived>(mark)) == std::vector<std::int64_t>{20});
}

TEST_CASE("ApLink: reconnecting to a different slot on the same seed ends the session", "[net][ap_link]")
{
    LinkFixture f;
    f.server.reply_to("Connect", {connected(1, json::object())});
    f.server.reply_to("Connect", {connected(2, json::object())});
    f.connect();
    f.wait_for<mth::ApConnected>("first ApConnected");

    const std::size_t mark = f.events.size();
    f.connect("Other");
    f.wait_for<mth::ApConnected>("second ApConnected", 1, mark);

    REQUIRE(f.index_of<mth::ApSessionEnded>(mark) < f.index_of<mth::ApConnected>(mark));
}

TEST_CASE("ApLink: ReceivedItems at an index the client already holds is not redelivered", "[net][ap_link]")
{
    LinkFixture f;
    f.server.reply_to("Connect", {connected(1, json::object()), received_items(0, {10, 11}), received_items(1, {11, 12})});
    f.connect();
    f.wait_for<mth::ApItemReceived>("three distinct items", 3);

    const auto items = f.all<mth::ApItemReceived>();
    REQUIRE(item_ids(items) == std::vector<std::int64_t>{10, 11, 12});
    REQUIRE(items[2]->item.index == 2);
}

TEST_CASE("ApLink: ReceivedItems that skip an index trigger a Sync and the resent list fills the gap", "[net][ap_link]")
{
    LinkFixture f;
    f.server.reply_to("Connect", {connected(1, json::object()), received_items(0, {10})});
    f.server.reply_to("Sync", {received_items(0, {10, 11, 12})});
    f.connect();
    f.wait_for<mth::ApItemReceived>("the first item");

    f.server.push(received_items(2, {12}));
    f.wait_for_sent("Sync");
    f.wait_for<mth::ApItemReceived>("the resynced items", 3);

    REQUIRE(f.server.received("Sync").size() == 1);
    const auto items = f.all<mth::ApItemReceived>();
    REQUIRE(item_ids(items) == std::vector<std::int64_t>{10, 11, 12});
    REQUIRE(items[1]->item.index == 1);
    REQUIRE(items[2]->item.index == 2);
}

TEST_CASE("ApLink: our own deathlink echo is dropped and another player's death is delivered", "[net][ap_link]")
{
    LinkFixture f;
    f.server.reply_to("Connect", {connected(1, {{"death_link", true}})});
    f.connect();
    f.wait_for<mth::ApConnected>("ApConnected");

    f.server.push(death_bounce("Mina"));
    f.server.push(death_bounce("Other"));
    f.wait_for<mth::ApDeathReceived>("the other player's death");

    const auto deaths = f.all<mth::ApDeathReceived>();
    REQUIRE(deaths.size() == 1);
    REQUIRE(deaths[0]->source == "Other");
    REQUIRE(deaths[0]->cause == "Other fell");
}

TEST_CASE("ApLink: LocationChecks and the goal StatusUpdate reach the server as sent", "[net][ap_link]")
{
    LinkFixture f;
    f.server.reply_to("Connect", {connected(1, json::object())});
    f.connect();
    f.wait_for<mth::ApConnected>("ApConnected");

    f.link.send_locations({101, 102});
    f.link.set_goal();
    f.wait_for_sent("LocationChecks");
    f.wait_for_sent("StatusUpdate", 2);

    REQUIRE(f.server.received("LocationChecks").at(0).at("locations") == json::array({101, 102}));
    bool goal_seen = false;
    for (const json &update : f.server.received("StatusUpdate"))
        goal_seen = goal_seen || update.at("status") == 30;
    REQUIRE(goal_seen);
}

TEST_CASE("ApLink: the ghost calls reach the server as SetNotify with Get and a Set carrying the packet", "[net][ap_link]")
{
    LinkFixture f;
    f.server.reply_to("Connect", {connected(1, json::object())});
    f.connect();
    f.wait_for<mth::ApConnected>("ApConnected");

    f.link.watch({"MTH_level_0_2", "MTH_level_0_3"}, true);
    f.link.watch({"MTH_ghost_0_2"}, false);
    f.link.send_ghost("MTH_ghost_0_1", {1, 40, 7});
    f.wait_for_sent("Set");

    const json keys = json::array({"MTH_level_0_2", "MTH_level_0_3"});
    REQUIRE(f.server.received("SetNotify").size() == 2);
    REQUIRE(f.server.received("SetNotify").at(0).at("keys") == keys);
    REQUIRE(f.server.received("SetNotify").at(1).at("keys") == json::array({"MTH_ghost_0_2"}));
    REQUIRE(f.server.received("Get").size() == 1);
    REQUIRE(f.server.received("Get").at(0).at("keys") == keys);
    REQUIRE(f.server.received("Set").at(0) ==
            json{{"cmd", "Set"}, {"key", "MTH_ghost_0_1"}, {"default", 0}, {"want_reply", false}, {"operations", json::array()}, {"g", {1, 40, 7}}});
}

TEST_CASE("ApLink: Connected names the other Mina slots on our team, and SetReply and Retrieved arrive as ApStored", "[net][ap_link]")
{
    LinkFixture f;
    json c = connected(1, json::object());
    c["players"] = json::array();
    c["slot_info"] = json::object();
    const auto add = [&c](int team, int slot, const char *game)
    {
        c["players"].push_back({{"team", team}, {"slot", slot}, {"alias", "P"}, {"name", "P"}});
        c["slot_info"][std::to_string(slot)] = {{"name", "P"}, {"game", game}, {"type", 1}, {"group_members", json::array()}};
    };
    add(0, 1, "Mina The Hollower");
    add(0, 2, "Mina The Hollower");
    add(0, 3, "Hylics 2");
    add(1, 4, "Mina The Hollower");
    f.server.reply_to("Connect", {c});
    f.connect();
    const mth::ApConnected connected_ev = f.wait_for<mth::ApConnected>("ApConnected");
    REQUIRE(connected_ev.team == 0);
    REQUIRE(connected_ev.mina_slots == std::vector<int>{2});

    const std::size_t mark = f.events.size();
    const json reply = {{"cmd", "SetReply"}, {"key", "MTH_ghost_0_2"}, {"value", 0},          {"original_value", 0},
                        {"slot", 2},         {"default", 0},           {"want_reply", false}, {"operations", json::array()}};
    json good = reply;
    good["g"] = {1, 2, 3};
    json floats = reply;
    floats["g"] = {1, 2.5, 3};
    json unstamped = reply;
    unstamped.erase("slot");
    f.server.push(good);
    f.server.push(floats);
    f.server.push(unstamped);
    f.server.push({{"cmd", "Retrieved"}, {"keys", {{"MTH_level_0_2", 65539}}}});
    f.wait_for<mth::ApStored>("the Retrieved entry", 3, mark);

    const auto stored = f.all<mth::ApStored>(mark);
    REQUIRE(stored.size() == 3);
    REQUIRE(stored[0]->key == "MTH_ghost_0_2");
    REQUIRE(stored[0]->slot == 2);
    REQUIRE(stored[0]->value == 0);
    REQUIRE(stored[0]->ghost == std::vector<std::int64_t>{1, 2, 3});
    REQUIRE(stored[1]->slot == 2);
    REQUIRE(stored[1]->ghost.empty());
    REQUIRE(stored[2]->key == "MTH_level_0_2");
    REQUIRE(stored[2]->slot == -1);
    REQUIRE(stored[2]->value == 65539);
}
