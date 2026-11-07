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

// Expected to fail: apclientpp's onmessage catches the parse throw, so it never reaches ApLink::run and no refusal is pushed.
TEST_CASE("ApLink: slot_data with a wrong-typed key refuses the connection", "[net][ap_link][!shouldfail]")
{
    LinkFixture f;
    f.server.reply_to("Connect", {connected(1, {{"ossex_start", "yes"}}), received_items(0, {10})});
    f.connect();
    f.wait_for_sent("Connect");
    f.server.push(received_items(0, {11}));

    REQUIRE(wait_until(
        [&]
        {
            f.pump();
            return !f.all<mth::ApConnectionRefused>().empty() || !f.all<mth::ApItemReceived>().empty();
        }));
    REQUIRE(f.all<mth::ApConnectionRefused>().size() == 1);
    REQUIRE(f.all<mth::ApConnected>().empty());
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
    REQUIRE(f.server.received("StatusUpdate").at(1).at("status") == 30);
}
