#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "mocks/fake_ap_link.hpp"
#include "mth/core/ap/ghost_stream.hpp"

using mth::ghost::decode;
using mth::ghost::encode;
using mth::ghost::kSamplesPerPacket;
using mth::ghost::Peers;
using mth::ghost::Playback;
using mth::ghost::Recorder;
using mth::ghost::Sample;

namespace
{

constexpr std::uint32_t kRoom = (12u << 16) | 3u;
// Samples per 60 Hz step at the fastest playout speed. A move further than that in one step is a skip.
constexpr double kFastest = 1.1;

// x carries the grid index, so a test can read which sample is on show.
Sample sample_at(std::int64_t index)
{
    Sample s;
    s.room = kRoom;
    s.pos[0] = static_cast<float>(index);
    s.sheet = mth::ghost::kSheets[0].hash;
    s.visible = true;
    return s;
}

// Both ends on one real-time axis in half steps (1/120 s), each with its own game clock. Packets arrive in order, as over the websocket.
struct Link
{
    Recorder sender;
    Playback receiver;
    std::deque<std::pair<std::int64_t, std::vector<std::int64_t>>> wire; // arrival time, packet
    std::int64_t now = 0;
    std::int64_t sender_clock = 0;
    std::int64_t receiver_clock = 0;
    std::int64_t newest = -1;    // newest index sampled
    std::int64_t held_until = 0; // a stalled link delivers nothing before this
    std::optional<Sample> shown;
    std::size_t held = 0;  // receiver steps on which the shown pose did not move
    std::size_t jumps = 0; // receiver steps on which it moved further than playback at the fastest speed

    void tick(bool sender_120, bool sender_running, bool receiver_120, std::int64_t transit)
    {
        ++now;
        if (sender_running && (sender_120 || now % 2 == 0))
        {
            sender_clock += sender_120 ? 1 : 2;
            if (sender.due(sender_clock))
            {
                newest = sender_clock / 2;
                if (std::vector<std::int64_t> packet = sender.add(sample_at(newest)); !packet.empty())
                    wire.emplace_back(std::max({now + transit, held_until, wire.empty() ? 0 : wire.back().first}), std::move(packet));
            }
        }
        if (receiver_120 || now % 2 == 0)
        {
            const std::int64_t half_steps = receiver_120 ? 1 : 2;
            receiver_clock += half_steps;
            while (!wire.empty() && wire.front().first <= now)
            {
                const std::optional<mth::ghost::Packet> packet = decode(wire.front().second);
                REQUIRE(packet.has_value());
                receiver.push(*packet, receiver_clock);
                wire.pop_front();
            }
            const std::optional<Sample> before = shown;
            shown = receiver.play(receiver_clock);
            if (before && shown)
            {
                const double moved = static_cast<double>(shown->pos[0]) - before->pos[0];
                held += moved == 0.0 ? 1 : 0;
                jumps += moved > kFastest * static_cast<double>(half_steps) / 2.0 + 1e-6 ? 1 : 0;
            }
        }
    }

    // How many samples the shown pose trails the newest one taken.
    [[nodiscard]] double lag() const
    {
        return static_cast<double>(newest) - shown->pos[0];
    }
};

constexpr std::int64_t kTransit = 12; // 100 ms
constexpr std::int64_t kSecond = 120;
// Settled lag bound in samples: transit and jitter (8), the sender's filling batch (11), a gap of at most 15 plus the 3-sample margin, and one spare.
constexpr double kMaxLag = 8.0 + 11.0 + 15.0 + 3.0 + 1.0;
constexpr std::uint32_t kOtherRoom = (12u << 16) | 4u;

std::vector<std::int64_t> packet_at(std::int64_t first)
{
    std::vector<Sample> samples;
    for (std::size_t i = 0; i < kSamplesPerPacket; ++i)
        samples.push_back(sample_at(first + static_cast<std::int64_t>(i)));
    return encode(first, samples);
}

mth::ApConnected connected_as(int slot, std::vector<int> mina_slots)
{
    mth::ApConnected c;
    c.player_slot = slot;
    c.team = 0;
    c.mina_slots = std::move(mina_slots);
    return c;
}

// A SetReply on `owner`'s room key Set by `setter`, or a Retrieved entry when setter is -1.
mth::ApStored room_key(int owner, std::int64_t room, int setter)
{
    return {.key = "MTH_level_0_" + std::to_string(owner), .slot = setter, .value = room};
}

mth::ApStored ghost_key(int owner, int setter, std::vector<std::int64_t> wire)
{
    return {.key = "MTH_ghost_0_" + std::to_string(owner), .slot = setter, .value = 0, .ghost = std::move(wire)};
}

Peers::Ghost *ghost_of(Peers &peers, int slot)
{
    for (Peers::Ghost &g : peers.ghosts())
        if (g.slot == slot)
            return &g;
    return nullptr;
}

std::size_t count(const std::vector<std::string> &keys, const std::string &key)
{
    return static_cast<std::size_t>(std::count(keys.begin(), keys.end(), key));
}

} // namespace

TEST_CASE("ghost stream: samples survive the wire to a hundredth of a pixel", "[ghost]")
{
    Sample a = sample_at(7);
    a.pos[0] = 1234.5678f;
    a.pos[1] = -98.7654f;
    a.pos[2] = 0.25f;
    a.sort = 0.6f;
    a.scale[0] = -1.5f;
    a.scale[1] = 2.0f;
    a.seq = 0xdeadbeefu;
    a.frame = 17;
    a.layer = 13;
    Sample b = sample_at(8);
    b.sheet = mth::ghost::kSheets[1].hash;
    b.visible = false;
    const Sample in[] = {a, b};

    const std::vector<std::int64_t> wire = encode(4321, in);
    REQUIRE(wire.size() == 2 + 2 * 12);
    const std::optional<mth::ghost::Packet> out = decode(wire);
    REQUIRE(out.has_value());
    REQUIRE(out->first == 4321);
    REQUIRE(out->samples.size() == 2);
    for (std::size_t i = 0; i < 2; ++i)
    {
        const Sample &x = in[i];
        const Sample &y = out->samples[i];
        CHECK(y.room == x.room);
        for (int k = 0; k < 3; ++k)
            CHECK(std::fabs(y.pos[k] - x.pos[k]) <= 0.0005f);
        CHECK(std::fabs(y.sort - x.sort) <= 0.0005f);
        CHECK(y.scale[0] == x.scale[0]);
        CHECK(y.scale[1] == x.scale[1]);
        CHECK(y.sheet == x.sheet);
        CHECK(y.seq == x.seq);
        CHECK(y.frame == x.frame);
        CHECK(y.layer == x.layer);
        CHECK(y.visible == x.visible);
    }
    // Decoding then encoding gives back the same ints.
    REQUIRE(encode(out->first, out->samples) == wire);
}

TEST_CASE("ghost stream: a malformed or hostile packet decodes to nothing", "[ghost]")
{
    const Sample one[] = {sample_at(0)};
    const std::vector<std::int64_t> good = encode(10, one);
    REQUIRE(decode(good).has_value());

    auto bad = [&](std::size_t at, std::int64_t value)
    {
        std::vector<std::int64_t> wire = good;
        wire[at] = value;
        return !decode(wire).has_value();
    };
    CHECK(bad(0, 2));           // unknown format
    CHECK(bad(1, -1));          // negative index
    CHECK(bad(1, INT64_MAX));   // index that would overflow
    CHECK(bad(2, -1));          // room
    CHECK(bad(2, 0x100000000)); // room beyond 32 bits
    CHECK(bad(3, 100000001));   // x beyond the World
    CHECK(bad(5, INT64_MIN));   // z
    CHECK(bad(6, -100000001));  // sort
    CHECK(bad(7, 16001));       // scale x
    CHECK(bad(8, -16001));      // scale y
    CHECK(bad(9, 0x12345678));  // a sheet the ghost cannot wear
    CHECK(bad(10, -1));         // seq
    CHECK(bad(11, 0x10000));    // frame
    CHECK(bad(12, 2));          // visible is a flag
    CHECK(bad(13, 32));         // layer
    CHECK(bad(13, -1));         // layer

    CHECK_FALSE(decode({}).has_value());
    CHECK_FALSE(decode(std::vector<std::int64_t>(good.begin(), good.begin() + 2)).has_value()); // header only
    std::vector<std::int64_t> ragged = good;
    ragged.push_back(0);
    CHECK_FALSE(decode(ragged).has_value());

    std::vector<Sample> many(61, sample_at(0));
    CHECK_FALSE(decode(encode(0, many)).has_value());
    many.resize(60);
    CHECK(decode(encode(0, many)).has_value());
}

TEST_CASE("ghost stream: the sender samples every 60 Hz grid point through a MaxFPS change", "[ghost]")
{
    Recorder rec;
    std::int64_t clock = 0;
    std::vector<std::int64_t> sampled;
    std::vector<mth::ghost::Packet> packets;
    auto step = [&](int half)
    {
        clock += half;
        if (!rec.due(clock))
            return;
        sampled.push_back(clock / 2);
        if (std::vector<std::int64_t> wire = rec.add(sample_at(clock / 2)); !wire.empty())
            packets.push_back(*decode(wire));
    };
    for (int i = 0; i < 31; ++i) // 60 FPS
        step(2);
    for (int i = 0; i < 49; ++i) // 120 FPS from an odd clock
        step(1);
    for (int i = 0; i < 40; ++i)
        step(2);

    REQUIRE(sampled.size() == 31 + 24 + 40);
    for (std::size_t i = 1; i < sampled.size(); ++i)
        REQUIRE(sampled[i] == sampled[i - 1] + 1);
    REQUIRE(packets.size() == sampled.size() / kSamplesPerPacket);
    for (std::size_t i = 0; i < packets.size(); ++i)
    {
        REQUIRE(packets[i].first == sampled[i * kSamplesPerPacket]);
        REQUIRE(packets[i].samples.size() == kSamplesPerPacket);
        REQUIRE(packets[i].samples.back().pos[0] == static_cast<float>(packets[i].first + kSamplesPerPacket - 1));
    }
}

TEST_CASE("ghost stream: a grid point with nothing to sample sends the batch early", "[ghost]")
{
    Recorder rec;
    for (std::int64_t clock = 2; clock <= 6; clock += 2)
    {
        REQUIRE(rec.due(clock));
        REQUIRE(rec.add(sample_at(clock / 2)).empty());
    }
    REQUIRE(rec.due(8));
    const std::optional<mth::ghost::Packet> early = decode(rec.add(std::nullopt));
    REQUIRE(early.has_value());
    REQUIRE(early->first == 1);
    REQUIRE(early->samples.size() == 3);
    REQUIRE(rec.due(10));
    REQUIRE(rec.add(std::nullopt).empty()); // nothing pending, nothing to send
    for (std::int64_t clock = 12; clock < 12 + 2 * static_cast<std::int64_t>(kSamplesPerPacket) - 2; clock += 2)
    {
        REQUIRE(rec.due(clock));
        REQUIRE(rec.add(sample_at(clock / 2)).empty());
    }
    REQUIRE(rec.due(12 + 2 * static_cast<std::int64_t>(kSamplesPerPacket) - 2));
    const std::optional<mth::ghost::Packet> next = decode(rec.add(sample_at(0)));
    REQUIRE(next.has_value());
    REQUIRE(next->first == 6); // the batch after the hole starts at the hole's end
}

TEST_CASE("ghost stream: jitter does not run the buffer dry or lag it behind", "[ghost]")
{
    Link link;
    std::mt19937 rng(7);
    std::uniform_int_distribution<std::int64_t> jitter(0, 4); // up to 33 ms on top of the transit
    std::size_t held_after_warmup = 0;
    double last_x = -1e9;
    double worst_lag = 0.0;
    for (std::int64_t t = 0; t < 30 * kSecond; ++t)
    {
        link.tick(false, true, false, kTransit + jitter(rng));
        if (t == 2 * kSecond)
            held_after_warmup = link.held;
        if (t > 2 * kSecond)
        {
            REQUIRE(link.shown.has_value());
            REQUIRE(link.shown->pos[0] >= last_x);
            last_x = link.shown->pos[0];
            worst_lag = std::max(worst_lag, link.lag());
        }
    }
    CHECK(link.held == held_after_warmup);
    CHECK(link.jumps == 0);
    CHECK(worst_lag <= kMaxLag);
}

TEST_CASE("ghost stream: a paused sender leaves the last pose held, then playback resumes without a jump", "[ghost]")
{
    Link link;
    for (std::int64_t t = 0; t < 3 * kSecond; ++t)
        link.tick(false, true, false, kTransit);
    REQUIRE(link.held == 0);

    for (std::int64_t t = 0; t < 3 * kSecond; ++t)
    {
        link.tick(false, t >= kSecond / 2, false, kTransit); // half a second with the sender's game paused
        REQUIRE(link.shown.has_value());
    }
    CHECK(link.held > 0);
    CHECK(link.jumps == 0);
}

TEST_CASE("ghost stream: after a stall delivers a burst, playback skips ahead instead of catching up", "[ghost]")
{
    Link link;
    for (std::int64_t t = 0; t < 3 * kSecond; ++t)
        link.tick(false, true, false, kTransit);
    const double settled_lag = link.lag();

    REQUIRE(link.held == 0);

    link.held_until = link.now + kSecond * 8 / 10; // the link stalls for 0.8 s and then delivers everything at once
    for (std::int64_t t = 0; t < kSecond * 8 / 10 + kTransit + 2; ++t)
    {
        link.tick(false, true, false, kTransit);
        REQUIRE(link.shown.has_value());
    }
    REQUIRE(link.held > 0);
    REQUIRE(link.jumps == 1);
    // About as far behind as before the stall, give or take a deeper buffer.
    CHECK(link.lag() <= settled_lag + static_cast<double>(kSamplesPerPacket) + 2.0);
}

TEST_CASE("ghost stream: either end changing MaxFPS keeps the stream smooth, and a 120 FPS receiver moves the ghost every half step", "[ghost]")
{
    Link link;
    std::size_t held_after_warmup = 0;
    double last_x = -1e9;
    double max_step_120 = 0.0;
    std::size_t fractional_120 = 0;
    for (std::int64_t t = 0; t < 20 * kSecond; ++t)
    {
        const bool sender_120 = (t / (2 * kSecond)) % 2 == 1;
        const bool receiver_120 = (t / (3 * kSecond)) % 2 == 1;
        link.tick(sender_120, true, receiver_120, kTransit);
        if (t == 2 * kSecond)
            held_after_warmup = link.held;
        if (t <= 2 * kSecond)
            continue;
        REQUIRE(link.shown.has_value());
        const double x = link.shown->pos[0];
        REQUIRE(x >= last_x);
        if (receiver_120 && t % (3 * kSecond) != 0)
        {
            max_step_120 = std::max(max_step_120, x - last_x);
            fractional_120 += x != std::floor(x) ? 1 : 0;
        }
        last_x = x;
    }
    CHECK(link.held == held_after_warmup);
    CHECK(link.jumps == 0);
    CHECK(max_step_120 <= kFastest / 2.0 + 1e-6);
    CHECK(fractional_120 > 100);
}

TEST_CASE("ghost stream: the ghost times out after a silence that scales with the sender's packet gap", "[ghost]")
{
    auto shown_after = [](std::size_t per_packet, std::int64_t silence)
    {
        Playback play;
        std::int64_t now = 0;
        std::int64_t index = 0;
        for (int p = 0; p < 20; ++p)
        {
            std::vector<Sample> samples;
            for (std::size_t i = 0; i < per_packet; ++i)
                samples.push_back(sample_at(index + static_cast<std::int64_t>(i)));
            play.push(*decode(encode(index, samples)), now);
            index += static_cast<std::int64_t>(per_packet);
            for (std::size_t i = 0; i < per_packet; ++i)
                (void)play.play(now += 2);
        }
        const std::int64_t last_arrival = now - static_cast<std::int64_t>(per_packet) * 2;
        std::optional<Sample> shown;
        while (now < last_arrival + silence)
            shown = play.play(now += 2);
        return shown.has_value();
    };
    CHECK(shown_after(12, kSecond * 9 / 10));
    CHECK_FALSE(shown_after(12, kSecond * 11 / 10));
    CHECK(shown_after(24, kSecond * 19 / 10));
    CHECK_FALSE(shown_after(24, kSecond * 21 / 10));
}

TEST_CASE("ghost stream: a sender that starts its count over restarts the stream at once", "[ghost]")
{
    Playback play;
    std::int64_t now = 0;
    auto send = [&](std::int64_t first)
    {
        std::vector<Sample> samples;
        for (std::size_t i = 0; i < kSamplesPerPacket; ++i)
            samples.push_back(sample_at(first + static_cast<std::int64_t>(i)));
        play.push(*decode(encode(first, samples)), now);
        for (std::size_t i = 0; i < kSamplesPerPacket; ++i)
            (void)play.play(now += 2);
    };
    for (std::int64_t first = 0; first < 120; first += 12)
        send(first);
    // A restart within the ring's reach of the old count.
    for (std::int64_t first = 0; first < 36; first += 12)
        send(first);
    const std::optional<Sample> shown = play.play(now += 2);
    REQUIRE(shown.has_value());
    CHECK(shown->pos[0] < 36.0f);
}

TEST_CASE("ghost stream: frames stay inside the sequence", "[ghost]")
{
    using mth::ghost::clamp_frame;
    REQUIRE_FALSE(clamp_frame(0, 0).has_value());
    REQUIRE(clamp_frame(3, 8) == 3u);
    REQUIRE(clamp_frame(8, 8) == 7u);
    REQUIRE(clamp_frame(UINT32_MAX, 8) == 7u);
}

TEST_CASE("ghost peers: connecting watches the other Mina slots' room keys, and a shared room starts the stream and its subscription", "[ghost]")
{
    mth::test::FakeApLink link;
    Peers peers(link);
    peers.apply(connected_as(1, {2, 3}));
    REQUIRE(link.watched == std::vector<std::string>{"MTH_level_0_2", "MTH_level_0_3"});
    REQUIRE(link.fetched == link.watched);

    peers.tick(kRoom, 2, false);
    CHECK_FALSE(peers.streaming()); // the room keys have not answered yet

    peers.apply(room_key(2, kRoom, -1)); // the Get that came with the subscription
    peers.apply(room_key(3, kOtherRoom, 3));
    peers.tick(kRoom, 4, false);
    REQUIRE(peers.streaming());
    CHECK(count(link.watched, "MTH_ghost_0_2") == 1);
    CHECK(count(link.watched, "MTH_ghost_0_3") == 0);
    CHECK(link.fetched.size() == 2); // a ghost key's stored value is always 0, so no Get

    peers.send(packet_at(0));
    REQUIRE(link.ghost_sets.size() == 1);
    CHECK(link.ghost_sets[0].first == "MTH_ghost_0_1");
    CHECK(link.ghost_sets[0].second == packet_at(0));

    peers.apply(room_key(2, kOtherRoom, 2));
    peers.tick(kRoom, 6, false);
    CHECK_FALSE(peers.streaming());
    peers.send(packet_at(12)); // the batch the Recorder flushes as the stream stops
    CHECK(link.ghost_sets.size() == 1);
    CHECK(link.unwatched == std::vector<std::string>{"MTH_ghost_0_2"});
    peers.tick(kRoom, 8, false);
    CHECK(link.unwatched.size() == 1); // nothing more changed
}

TEST_CASE("ghost peers: a packet counts only when the server stamped its key's owner and that owner shares our room", "[ghost]")
{
    mth::test::FakeApLink link;
    Peers peers(link);
    peers.apply(connected_as(1, {2, 3}));
    peers.apply(room_key(2, kRoom, 2));
    peers.apply(room_key(3, kOtherRoom, 3));
    peers.tick(kRoom, 2, false);

    peers.apply(ghost_key(2, 3, packet_at(0))); // slot 3 writing slot 2's key
    peers.apply(room_key(2, kOtherRoom, 3));    // slot 3 moving slot 2 away
    peers.apply(ghost_key(2, 2, {1, 0, 7}));    // too short to be a packet
    peers.apply(ghost_key(3, 3, packet_at(0))); // slot 3 is elsewhere, still sending to an old subscription
    peers.apply(ghost_key(4, 4, packet_at(0))); // not a Mina slot on our team
    for (const Peers::Ghost &g : peers.ghosts())
        CHECK(g.slot == -1);

    peers.tick(kRoom, 4, false);
    REQUIRE(peers.streaming()); // the forged room key did not move slot 2
    peers.apply(ghost_key(2, 2, packet_at(0)));
    CHECK(ghost_of(peers, 2) != nullptr);
}

TEST_CASE("ghost peers: a disconnect drops every ghost and the subscriptions, and a reconnect makes them again", "[ghost]")
{
    mth::test::FakeApLink link;
    Peers peers(link);
    peers.apply(connected_as(1, {2}));
    peers.apply(room_key(2, kRoom, 2));
    peers.tick(kRoom, 2, false);
    peers.apply(ghost_key(2, 2, packet_at(0)));
    REQUIRE(ghost_of(peers, 2) != nullptr);

    peers.apply(mth::ApDisconnected{});
    peers.tick(kRoom, 4, false);
    CHECK_FALSE(peers.streaming());
    CHECK(ghost_of(peers, 2) == nullptr);
    peers.apply(ghost_key(2, 2, packet_at(12))); // queued before the disconnect
    CHECK(ghost_of(peers, 2) == nullptr);
    CHECK(link.unwatched.empty()); // the server forgot them with the connection

    peers.apply(connected_as(1, {2}));
    CHECK(count(link.watched, "MTH_level_0_2") == 2);
    peers.apply(room_key(2, kRoom, -1));
    peers.tick(kRoom, 6, false);
    CHECK(peers.streaming());
    CHECK(count(link.watched, "MTH_ghost_0_2") == 2);
}

TEST_CASE("ghost peers: at most kMaxGhosts keys are subscribed, the echo's first and then the lowest slots", "[ghost]")
{
    mth::test::FakeApLink link;
    Peers peers(link);
    std::vector<int> slots;
    for (int slot = 2; slot <= 12; ++slot)
        slots.push_back(slot);
    peers.apply(connected_as(1, slots));
    for (int slot : slots)
        peers.apply(room_key(slot, kRoom, slot));
    peers.set_echo(true);
    peers.tick(kRoom, 2, false);

    std::vector<std::string> ghosts;
    for (const std::string &key : link.watched)
        if (key.starts_with("MTH_ghost_"))
            ghosts.push_back(key);
    REQUIRE(ghosts.size() == Peers::kMaxGhosts);
    CHECK(count(ghosts, "MTH_ghost_0_1") == 1);
    CHECK(count(ghosts, "MTH_ghost_0_8") == 1);
    CHECK(count(ghosts, "MTH_ghost_0_9") == 0);
}

TEST_CASE("ghost peers: the echo comes back through our own key, only while connected", "[ghost]")
{
    mth::test::FakeApLink link;
    Peers peers(link);
    peers.set_echo(true);
    peers.tick(kRoom, 2, false);
    CHECK_FALSE(peers.streaming());

    peers.apply(connected_as(1, {}));
    peers.tick(kRoom, 4, false);
    REQUIRE(peers.streaming());
    CHECK(count(link.watched, "MTH_ghost_0_1") == 1);
    peers.send(packet_at(0));
    REQUIRE(link.ghost_sets.size() == 1);
    CHECK(link.ghost_sets[0].first == "MTH_ghost_0_1");
    peers.apply(ghost_key(1, 1, link.ghost_sets[0].second));
    REQUIRE(ghost_of(peers, 1) != nullptr);

    // Echo off drops its ghost at once, and a reply drained before the next tick must not bring it back.
    peers.set_echo(false);
    CHECK(ghost_of(peers, 1) == nullptr);
    peers.apply(ghost_key(1, 1, packet_at(12)));
    CHECK(ghost_of(peers, 1) == nullptr);
    peers.tick(kRoom, 6, false);
    CHECK_FALSE(peers.streaming());
    CHECK(ghost_of(peers, 1) == nullptr);
    CHECK(link.unwatched == std::vector<std::string>{"MTH_ghost_0_1"});
}

TEST_CASE("ghost peers: packets arriving while our game is paused are dropped, so playback resumes with one skip", "[ghost]")
{
    mth::test::FakeApLink link;
    Peers peers(link);
    peers.apply(connected_as(1, {2}));
    peers.apply(room_key(2, kRoom, 2));

    Recorder sender;
    std::deque<std::pair<std::int64_t, std::vector<std::int64_t>>> wire;
    std::int64_t receiver_clock = 0;
    std::optional<Sample> shown;
    double last_x = -1e9;
    std::size_t jumps = 0;
    // Both ends at 60 FPS on a 100 ms link. The receiver pauses from the 2nd to the 4th second while the sender plays on.
    for (std::int64_t now = 2; now <= 7 * kSecond; now += 2)
    {
        if (sender.due(now))
            if (std::vector<std::int64_t> packet = sender.add(sample_at(now / 2)); !packet.empty())
                wire.emplace_back(now + kTransit, std::move(packet));
        const bool paused = now >= 2 * kSecond && now < 4 * kSecond;
        if (!paused)
            receiver_clock += 2;
        peers.tick(kRoom, receiver_clock, paused);
        while (!wire.empty() && wire.front().first <= now)
        {
            peers.apply(ghost_key(2, 2, std::move(wire.front().second)));
            wire.pop_front();
        }
        Peers::Ghost *ghost = ghost_of(peers, 2);
        if (paused || ghost == nullptr)
            continue;
        const bool was_shown = shown.has_value();
        shown = ghost->playback.play(receiver_clock);
        REQUIRE((shown || !was_shown)); // once on show, the ghost stays through the pause
        if (shown)
        {
            REQUIRE(shown->pos[0] >= last_x);
            jumps += was_shown && shown->pos[0] - last_x > kFastest + 1e-6 ? 1 : 0;
            last_x = shown->pos[0];
        }
    }
    CHECK(jumps == 1);
    REQUIRE(shown.has_value());
    CHECK(7.0 * kSecond / 2 - shown->pos[0] <= kMaxLag); // as far behind as before the pause
}
