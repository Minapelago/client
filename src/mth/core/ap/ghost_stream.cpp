#include "mth/core/ap/ghost_stream.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <variant>

#include "pal/pal_log.hpp"

namespace mth::ghost
{

namespace
{

constexpr std::int64_t kFormat = 1;
// Over a year at 60 Hz, far from overflow in index arithmetic.
constexpr std::int64_t kIndexLimit = std::numeric_limits<std::int32_t>::max() - static_cast<std::int64_t>(kMaxSamples);
constexpr double kFixed = 1000.0;
// World units; a room is a few dozen across.
constexpr double kPositionLimit = 100000.0;
constexpr double kScaleLimit = 16.0;
constexpr std::int64_t kFrameLimit = 0xffff;
constexpr std::int64_t kU32Max = std::numeric_limits<std::uint32_t>::max();

std::int64_t to_fixed(float v, double limit)
{
    return std::llround(std::clamp(static_cast<double>(v), -limit, limit) * kFixed);
}

float from_fixed(std::int64_t v)
{
    return static_cast<float>(static_cast<double>(v) / kFixed);
}

bool in(std::int64_t v, std::int64_t lo, std::int64_t hi)
{
    return v >= lo && v <= hi;
}

bool fixed_in(std::int64_t v, double limit)
{
    const auto bound = static_cast<std::int64_t>(limit * kFixed);
    return in(v, -bound, bound);
}

// Buffer tuning, in samples or per packet.
constexpr double kMargin = 3.0;       // 50 ms beyond the gap for jitter it has not caught yet
constexpr double kGapFall = 1.0 / 32; // per packet, about 6 s to settle at 5 packets a second
constexpr double kLevelGain = 1.0 / 4;
constexpr double kNudge = 0.02; // playout speed change per sample of level error
constexpr double kMaxNudge = 0.1;
constexpr double kTimeoutGaps = 5.0; // a second at 5 packets a second
constexpr double kMinTimeout = 30.0; // half a second

} // namespace

const Sheet *find_sheet(std::uint32_t hash)
{
    for (const Sheet &sheet : kSheets)
        if (sheet.hash == hash)
            return &sheet;
    return nullptr;
}

std::vector<std::int64_t> encode(std::int64_t first, std::span<const Sample> samples)
{
    std::vector<std::int64_t> out;
    out.reserve(kHeaderInts + samples.size() * kSampleInts);
    out.push_back(kFormat);
    out.push_back(first);
    for (const Sample &s : samples)
    {
        out.insert(out.end(), {
                                  static_cast<std::int64_t>(s.room),
                                  to_fixed(s.pos[0], kPositionLimit),
                                  to_fixed(s.pos[1], kPositionLimit),
                                  to_fixed(s.pos[2], kPositionLimit),
                                  to_fixed(s.sort, kPositionLimit),
                                  to_fixed(s.scale[0], kScaleLimit),
                                  to_fixed(s.scale[1], kScaleLimit),
                                  static_cast<std::int64_t>(s.sheet),
                                  static_cast<std::int64_t>(s.seq),
                                  static_cast<std::int64_t>(s.frame),
                                  s.visible ? 1 : 0,
                                  static_cast<std::int64_t>(s.layer),
                              });
    }
    return out;
}

std::optional<Packet> decode(std::span<const std::int64_t> wire)
{
    if (wire.size() < kHeaderInts + kSampleInts || (wire.size() - kHeaderInts) % kSampleInts != 0 || wire[0] != kFormat)
        return std::nullopt;
    const std::size_t count = (wire.size() - kHeaderInts) / kSampleInts;
    if (count > kMaxSamples || !in(wire[1], 0, kIndexLimit))
        return std::nullopt;
    Packet packet;
    packet.first = wire[1];
    packet.samples.resize(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::span<const std::int64_t> v = wire.subspan(kHeaderInts + i * kSampleInts, kSampleInts);
        const bool ok = in(v[0], 0, kU32Max) && fixed_in(v[1], kPositionLimit) && fixed_in(v[2], kPositionLimit) && fixed_in(v[3], kPositionLimit) &&
                        fixed_in(v[4], kPositionLimit) && fixed_in(v[5], kScaleLimit) && fixed_in(v[6], kScaleLimit) && in(v[7], 0, kU32Max) &&
                        find_sheet(static_cast<std::uint32_t>(v[7])) != nullptr && in(v[8], 0, kU32Max) && in(v[9], 0, kFrameLimit) && in(v[10], 0, 1) &&
                        in(v[11], 0, kLayerLimit - 1);
        if (!ok)
            return std::nullopt;
        Sample &s = packet.samples[i];
        s.room = static_cast<std::uint32_t>(v[0]);
        s.pos[0] = from_fixed(v[1]);
        s.pos[1] = from_fixed(v[2]);
        s.pos[2] = from_fixed(v[3]);
        s.sort = from_fixed(v[4]);
        s.scale[0] = from_fixed(v[5]);
        s.scale[1] = from_fixed(v[6]);
        s.sheet = static_cast<std::uint32_t>(v[7]);
        s.seq = static_cast<std::uint32_t>(v[8]);
        s.frame = static_cast<std::uint32_t>(v[9]);
        s.visible = v[10] == 1;
        s.layer = static_cast<std::int32_t>(v[11]);
    }
    return packet;
}

bool Recorder::due(std::int64_t now)
{
    const std::int64_t index = now / 2;
    if (index == index_)
        return false;
    index_ = index;
    return true;
}

std::vector<std::int64_t> Recorder::add(const std::optional<Sample> &sample)
{
    if (sample)
    {
        if (pending_.empty())
            first_ = index_;
        pending_.push_back(*sample);
        if (pending_.size() < kSamplesPerPacket)
            return {};
    }
    else if (pending_.empty())
    {
        return {};
    }
    std::vector<std::int64_t> packet = encode(first_, pending_);
    pending_.clear();
    return packet;
}

const Sample *Playback::find(std::int64_t index) const
{
    if (index < 0)
        return nullptr;
    const Slot &slot = ring_[static_cast<std::size_t>(index) % kCapacity];
    return slot.index == index ? &slot.sample : nullptr;
}

double Playback::target() const
{
    return gap_ + kMargin;
}

void Playback::reset()
{
    live_ = false;
    held_.reset();
    for (Slot &slot : ring_)
        slot.index = -1;
}

void Playback::push(const Packet &packet, std::int64_t now)
{
    const auto count = static_cast<std::int64_t>(packet.samples.size());
    const std::int64_t last = packet.first + count - 1;
    const auto capacity = static_cast<std::int64_t>(kCapacity);
    // Packets arrive in order, so one ending before the newest sample means the sender restarted its count.
    if (live_ && (last < newest_ || packet.first > newest_ + capacity))
        reset();
    for (std::int64_t i = 0; i < count; ++i)
    {
        const std::int64_t index = packet.first + i;
        ring_[static_cast<std::size_t>(index) % kCapacity] = {index, packet.samples[static_cast<std::size_t>(i)]};
    }

    if (!live_)
    {
        live_ = true;
        newest_ = last;
        last_arrival_ = now;
        last_play_ = now;
        gap_ = static_cast<double>(count);
        level_ = target();
        // Start as if the buffer were full; nothing shows until the playout point reaches the first sample.
        pos_ = static_cast<double>(newest_) - target();
        return;
    }
    if (last <= newest_)
        return; // nothing new, so not an arrival

    // The gap tracks the largest recent wait. A wait over two packets is a stall, which the hold and the skip handle instead.
    const double seen = static_cast<double>(now - last_arrival_) / 2.0;
    if (seen <= 2.0 * static_cast<double>(count))
        gap_ = seen > gap_ ? seen : gap_ + (seen - gap_) * kGapFall;
    newest_ = last;
    last_arrival_ = now;

    const double level = static_cast<double>(newest_) - pos_;
    if (level > target() + static_cast<double>(count))
    {
        // A burst after a stall: catching up by speed would take seconds.
        pos_ = static_cast<double>(newest_) - target();
        level_ = target();
    }
    else
    {
        level_ += (level - level_) * kLevelGain;
    }
}

std::optional<Sample> Playback::play(std::int64_t now)
{
    if (!live_)
        return std::nullopt;
    const std::int64_t steps = now - last_play_;
    last_play_ = now;
    if (static_cast<double>(now - last_arrival_) / 2.0 > std::max(kTimeoutGaps * gap_, kMinTimeout))
    {
        reset();
        return std::nullopt;
    }
    const double rate = 1.0 + std::clamp((level_ - target()) * kNudge, -kMaxNudge, kMaxNudge);
    pos_ += rate * static_cast<double>(steps) / 2.0;
    if (pos_ > static_cast<double>(newest_))
        pos_ = static_cast<double>(newest_); // ran dry: hold the newest pose
    const auto index = static_cast<std::int64_t>(std::floor(pos_));
    const Sample *a = find(index);
    if (a == nullptr)
        return held_;
    Sample out = *a;
    const double t = pos_ - static_cast<double>(index);
    const Sample *b = find(index + 1);
    if (t > 0.0 && b != nullptr && b->room == a->room)
        for (int i = 0; i < 3; ++i)
            out.pos[i] = static_cast<float>(a->pos[i] + (b->pos[i] - a->pos[i]) * t);
    held_ = out;
    return out;
}

void Peers::apply(const ApEvent &ev)
{
    if (const auto *c = std::get_if<ApConnected>(&ev))
    {
        reset();
        connected_ = true;
        own_ = c->player_slot;
        own_key_ = datastore_key("ghost", c->team, own_);
        std::vector<std::string> levels;
        for (int slot : c->mina_slots)
        {
            peers_.push_back({slot, datastore_key("level", c->team, slot), datastore_key("ghost", c->team, slot)});
            levels.push_back(peers_.back().level_key);
        }
        // Room keys change rarely, so these stay subscribed for the connection; the Get fetches each current room.
        if (!levels.empty())
            link_.watch(levels, true);
        return;
    }
    if (std::holds_alternative<ApDisconnected>(ev))
    {
        reset(); // the server drops a connection's subscriptions with it
        return;
    }
    const auto *stored = std::get_if<ApStored>(&ev);
    if (stored == nullptr || !connected_)
        return;
    for (Peer &p : peers_)
    {
        if (stored->key == p.level_key)
        {
            // Anyone can Set any key, so only the owner's SetReply counts. A Retrieved entry (slot -1) has no setter.
            if (stored->slot != -1 && stored->slot != p.slot)
                return;
            const std::optional<std::int64_t> v = stored->value;
            p.room =
                v && *v >= 0 && *v <= std::numeric_limits<std::uint32_t>::max() ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(*v)) : std::nullopt;
            return;
        }
        if (stored->key == p.ghost_key)
        {
            take(p.slot, p.watched, *stored);
            return;
        }
    }
    // echo_, not echo_watched_, so a reply drained right after set_echo(false) cannot recreate the entry.
    if (stored->key == own_key_)
        take(own_, echo_, *stored);
}

void Peers::take(int owner, bool watched, const ApStored &stored)
{
    if (stored.slot != owner || !watched)
        return;
    // Our clock stops while paused, so packets kept now would all land at one instant; dropping them leaves a hole that playback skips.
    if (paused_)
        return;
    const std::optional<Packet> packet = decode(stored.ghost);
    if (!packet)
        return;
    Ghost *free_entry = nullptr;
    for (Ghost &g : ghosts_)
    {
        if (g.slot == owner)
        {
            g.playback.push(*packet, now_);
            return;
        }
        if (g.slot == -1 && free_entry == nullptr)
            free_entry = &g;
    }
    if (free_entry != nullptr)
    {
        free_entry->slot = owner;
        free_entry->playback.push(*packet, now_);
    }
}

void Peers::drop(int owner)
{
    for (Ghost &g : ghosts_)
        if (g.slot == owner)
            g = Ghost{};
}

void Peers::reset()
{
    connected_ = false;
    own_ = -1;
    own_key_.clear();
    peers_.clear();
    echo_watched_ = false;
    for (Ghost &g : ghosts_)
        g = Ghost{};
}

void Peers::tick(std::optional<std::uint32_t> room, std::int64_t now, bool paused)
{
    now_ = now;
    paused_ = paused;

    std::vector<std::string> on;
    std::vector<std::string> off;
    if (connected_ && echo_ != echo_watched_)
    {
        echo_watched_ = echo_;
        (echo_ ? on : off).push_back(own_key_);
    }
    std::size_t open = kMaxGhosts - (echo_watched_ ? 1 : 0);
    streaming_ = connected_ && echo_;
    for (Peer &p : peers_)
    {
        const bool here = room && p.room == room;
        streaming_ = streaming_ || here;
        const bool want = here && open > 0;
        open -= want ? 1 : 0;
        if (want == p.watched)
            continue;
        p.watched = want;
        pal::logf(pal::LogLevel::Info, want ? "ghost: drawing slot %d, which shares our room" : "ghost: stopped drawing slot %d", p.slot);
        if (want)
        {
            on.push_back(p.ghost_key);
        }
        else
        {
            off.push_back(p.ghost_key);
            drop(p.slot);
        }
    }
    // unwatch() sends nothing yet (see ApLink::unwatch), so take() drops what slots that left keep sending.
    if (!off.empty())
        link_.unwatch(off);
    // A ghost key always stores 0, so no Get.
    if (!on.empty())
        link_.watch(on, false);
}

void Peers::send(std::vector<std::int64_t> wire)
{
    // Also drops the batch the Recorder flushes as the stream stops.
    if (!streaming_)
        return;
    link_.send_ghost(own_key_, std::move(wire));
}

void Peers::set_echo(bool on)
{
    echo_ = on;
    drop(own_); // either way, so the echo restarts with a fresh playback
}

} // namespace mth::ghost
