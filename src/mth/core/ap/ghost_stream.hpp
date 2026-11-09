#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mth/core/ap/ap_link.hpp"
#include "mth/core/game/hook_hash.hpp"

namespace mth::ghost
{

// 5 packets a second, about 250 ms of batching delay. Receivers size their buffer from the gap they see, so builds with other values mix.
inline constexpr std::size_t kSamplesPerPacket = 12;
inline constexpr std::size_t kMaxSamples = 60; // a sender batching more than a second is broken or hostile
inline constexpr std::size_t kHeaderInts = 2;
inline constexpr std::size_t kSampleInts = 12;
inline constexpr std::size_t kMaxWireInts = kHeaderInts + kMaxSamples * kSampleInts;

// Real collision layers fit well inside this, so anything larger is a wrong offset or a hostile sender.
inline constexpr std::int32_t kLayerLimit = 32;

// The player's two anim sets, keyed by the engine_hash ycCutterAnimCache stores for the path. init_seq is a sequence the game itself starts on the
// sheet (Linux 0xd3277a, 0xd327bc), so AnimEffect::Init is known to find it.
struct Sheet
{
    std::string_view path;
    std::string_view init_seq;
    std::uint32_t hash = hookhash::engine_hash(path);
};
inline constexpr Sheet kSheets[] = {
    {"player/Mina.anb.yc", "bicyclePieces"},
    {"player/minaBeast.anb.yc", "transform"},
};
[[nodiscard]] const Sheet *find_sheet(std::uint32_t hash);

// One 60 Hz sample of the body sprite.
struct Sample
{
    std::uint32_t room = 0;     // (gamestate << 16) | room, the MTH_level value
    float pos[3]{};             // drawn position, with the AnimParent and local translation folded in
    float sort = 0.0f;          // added to pos[1] for the y sort only
    float scale[2]{1.0f, 1.0f}; // a negative x is the sprite flip
    std::uint32_t sheet = 0;    // a kSheets hash
    std::uint32_t seq = 0;      // engine_hash of the full sequence name
    std::uint32_t frame = 0;    // unchecked; clamp_frame before use
    std::int32_t layer = 0;     // collision layer, which sets the depth bias
    bool visible = false;
};

struct Packet
{
    std::int64_t first = 0; // grid index of samples[0]
    std::vector<Sample> samples;
};

// Flat int array: format tag, first index, then 12 ints per sample. Positions, sort and scale travel in thousandths. encode() expects finite floats.
[[nodiscard]] std::vector<std::int64_t> encode(std::int64_t first, std::span<const Sample> samples);
// Untrusted input: any bad field drops the whole packet, and the ranges keep every decoded float finite.
[[nodiscard]] std::optional<Packet> decode(std::span<const std::int64_t> wire);

// The sender's batching. Clocks count half steps (1/120 s) of game time so the 60 Hz grid survives a MaxFPS change.
class Recorder
{
  public:
    bool due(std::int64_t now); // true when now crossed a grid point; now advances at most 2 per call
    // A packet once kSamplesPerPacket are in; a missing sample flushes early, since a packet holds only consecutive samples. Empty otherwise.
    [[nodiscard]] std::vector<std::int64_t> add(const std::optional<Sample> &sample);

  private:
    std::int64_t index_ = -1;
    std::int64_t first_ = 0;
    std::vector<Sample> pending_;
};

// The receiver's buffer. Steers its playout speed to keep about one packet gap plus a margin buffered.
class Playback
{
  public:
    void push(const Packet &packet, std::int64_t now); // a packet from decode()
    // Position interpolated between grid points, the rest from the playout point. nullopt before the first packet and after a timeout.
    [[nodiscard]] std::optional<Sample> play(std::int64_t now);

  private:
    static constexpr std::size_t kCapacity = 256; // samples, about 4 s
    static_assert(kMaxSamples < kCapacity, "a whole packet has to fit the ring");

    struct Slot
    {
        std::int64_t index = -1;
        Sample sample;
    };
    [[nodiscard]] const Sample *find(std::int64_t index) const;
    [[nodiscard]] double target() const; // samples to hold when a packet lands
    void reset();

    std::array<Slot, kCapacity> ring_{};
    bool live_ = false;
    double pos_ = 0.0; // playout point, in grid indices
    std::int64_t newest_ = 0;
    std::int64_t last_arrival_ = 0;
    std::int64_t last_play_ = 0;
    std::optional<Sample> held_; // shown through a hole in the stream
    double gap_ = 0.0;           // in samples
    double level_ = 0.0;         // samples held when packets landed, smoothed
};

// The AP side: follows each other Mina slot's MTH_level key and, while one shares our room, subscribes to its MTH_ghost key and plays it back.
// The console echo subscribes to our own key too. Game thread only.
class Peers
{
  public:
    static constexpr std::size_t kMaxGhosts = 8; // the echo first, then slots in the order Connected lists them

    struct Ghost
    {
        int slot = -1; // -1 for a free entry
        Playback playback;
    };

    explicit Peers(IApLink &link) : link_(link)
    {
    }

    void apply(const ApEvent &ev);
    // Once per fixed step. room is nullopt before the first gameplay reading; now is in half steps.
    void tick(std::optional<std::uint32_t> room, std::int64_t now, bool paused);
    // Connected, and the echo is on or another Mina slot is in our room.
    [[nodiscard]] bool streaming() const
    {
        return streaming_;
    }
    void send(std::vector<std::int64_t> wire); // dropped while not streaming
    void set_echo(bool on);
    [[nodiscard]] bool echo() const
    {
        return echo_;
    }
    [[nodiscard]] std::span<Ghost> ghosts()
    {
        return ghosts_;
    }

  private:
    struct Peer
    {
        int slot = 0;
        std::string level_key;
        std::string ghost_key;
        std::optional<std::uint32_t> room;
        bool watched = false; // in our room and subscribed
    };
    void reset();
    void take(int owner, bool watched, const ApStored &stored);
    void drop(int owner);

    IApLink &link_;
    bool connected_ = false;
    int own_ = -1;
    std::string own_key_;
    std::vector<Peer> peers_;
    bool echo_ = false;
    bool echo_watched_ = false;
    std::int64_t now_ = 0;
    bool paused_ = false;
    bool streaming_ = false;
    std::array<Ghost, kMaxGhosts> ghosts_{};
};

// ycCutterAnimComponent::UpdateRenderFrame indexes the frame table unchecked, so a received frame must be clamped. nullopt with no frames.
[[nodiscard]] constexpr std::optional<std::uint32_t> clamp_frame(std::uint32_t frame, std::uint32_t frame_count) noexcept
{
    if (frame_count == 0)
        return std::nullopt;
    return frame < frame_count ? frame : frame_count - 1;
}

} // namespace mth::ghost
