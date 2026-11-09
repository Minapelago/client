#include "mth/features/ghost_players.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mod/mod_api.hpp"
#include "mth/core/game/component_types.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/core/game/game_symbols.hpp"
#include "mth/core/game/hook_hash.hpp"
#include "mth/features/room_tracker.hpp"
#include "pal/pal_layout.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"
#include "pal/pal_module.hpp"

namespace mth
{

namespace
{

constexpr std::uint32_t kAnimEffectType = 38; // ENTITYTYPE_ANIM_EFFECT

// Packed like mth::banner_color. Full alpha, since a lower one may cut the sprite out on the opaque pass instead of blending it.
constexpr std::uint32_t kGhostTint = 0xFFFFE060u;
constexpr std::uint64_t kSequenceLimit = 4096;
constexpr std::size_t kSeqNameLimit = 64;
// GameAnim iso sort modes (IsoAnim::GetIsoDepth, Linux 0xb71470, Windows 0x140838cc0): Init sets 3, a fixed band in front of y-sorted sprites;
// the player uses 4, the y sort.
constexpr std::int32_t kInitIsoMode = 3;
constexpr std::int32_t kYSortIsoMode = 4;
// Half steps between spawns of one ghost, so a sender flipping sheet or room every sample cannot force a spawn every step.
constexpr std::int64_t kRespawnCooldown = 36;

// Through the scene graph, which needs no AnimEffect field offset.
void *find_anim(void *effect)
{
    void *children[16];
    const std::size_t total = mod::entity_children(mod::component_parent(effect), children, std::size(children));
    for (std::size_t i = 0; i < std::min(total, std::size(children)); ++i)
        if (mod::component_isa(children[i], rtti::kGameAnim))
            return children[i];
    return nullptr;
}

// engine_hash of the anim's sheet path, or 0 without a sheet.
std::uint32_t sheet_hash(void *anim)
{
    void *cutter = pal::at<void *>(anim, layout::kCutterAnimOff);
    return pal::pointer_looks_valid(cutter) ? pal::at<std::uint32_t>(cutter, layout::kCutterAnimNameHashOff) : 0;
}

// The sheet's sequence named by `hash`, or null. GameAnimPlay takes a name and no API entry lists a sheet's sequences, so this reads the table.
const char *sequence_name(void *anim, std::uint32_t hash)
{
    void *cutter = pal::at<void *>(anim, layout::kCutterAnimOff);
    if (!pal::pointer_looks_valid(cutter))
        return nullptr;
    void *data = pal::at<void *>(cutter, layout::kCutterAnimDataOff);
    if (!pal::pointer_looks_valid(data))
        return nullptr;
    const std::uint64_t count = pal::at<std::uint64_t>(data, layout::kCutterAnimSeqCountOff) >> layout::kCutterAnimSeqCountShift;
    const char *entries = pal::at<const char *>(data, layout::kCutterAnimSeqTableOff);
    if (count >= kSequenceLimit || !pal::pointer_looks_valid(entries))
        return nullptr;
    for (std::uint64_t i = 0; i < count; ++i)
    {
        const char *entry = entries + i * layout::kCutterSeqStride;
        if (pal::at<std::uint32_t>(entry, layout::kCutterSeqHashOff) != hash)
            continue;
        const char *name = pal::at<const char *>(entry, layout::kCutterSeqNameOff);
        if (!pal::pointer_looks_valid(name))
            return nullptr;
        const void *end = std::memchr(name, 0, kSeqNameLimit);
        // Re-hashing catches a table read through wrong offsets before GameAnimPlay gets a stray pointer.
        if (end == nullptr || hookhash::engine_hash({name, static_cast<std::size_t>(static_cast<const char *>(end) - name)}) != hash)
            return nullptr;
        return name;
    }
    return nullptr;
}

// Switches the anim to the body's y sort, just behind the body at equal y. False, writing nothing, when the mode field does not hold Init's value.
bool use_y_sort(void *anim)
{
    std::int32_t &mode = pal::at<std::int32_t>(anim, pal::layout::kGameAnimIsoModeOff);
    if (mode != kInitIsoMode)
        return false;
    mode = kYSortIsoMode;
    // IsoAnim::CalcTransform subtracts this from the depth, where smaller is nearer (Linux 0xb716e9, Windows 0x140839063). One y-sort step is
    // 4.8/16384 (Linux 0xb715d7); two clear the body's AnimParent height of -1 (Linux 0xd0c53f) and move the ghost under 0.04 px.
    constexpr float kBehindAtSameY = 2.0f * 4.8f / 16384.0f;
    pal::at<float>(anim, pal::layout::kGameAnimIsoDepthOff) = -kBehindAtSameY;
    return true;
}

// `world` is the World the ghost was spawned in: the old gamestate World is destroyed, and its handles dropped, before the next one exists.
void despawn(void *world, void *&weak)
{
    if (weak == nullptr)
        return;
    if (void *effect = mod::weak_ptr_get(weak))
        mod::queue_destroy_entity(world, mod::component_parent(effect), false);
    mod::weak_ptr_destroy(weak);
    weak = nullptr;
}

bool finite(const ghost::Sample &s)
{
    return std::isfinite(s.pos[0]) && std::isfinite(s.pos[1]) && std::isfinite(s.pos[2]) && std::isfinite(s.sort) && std::isfinite(s.scale[0]) &&
           std::isfinite(s.scale[1]);
}

} // namespace

void GhostPlayers::toggle_echo()
{
    const bool on = !peers_.echo();
    peers_.set_echo(on);
    // Turned on offline, the echo starts with the next connection.
    pal::logf(pal::LogLevel::Info, on ? "ghost: echo on, through our own ghost key on the server" : "ghost: echo off");
}

void GhostPlayers::tick(void *world, float step_seconds)
{
    // The pause menu freezes the player but not this hook; the clock stops with the player so ghosts freeze too.
    const bool paused = mod::world_is_paused();
    // Game::FixedUpdate steps 1/60 or 1/120 s by the MaxFPS option.
    if (!paused)
        clock_ += std::clamp<std::int64_t>(std::lround(step_seconds * 120.0f), 1, 2);

    const bool settled = room_tracker::settled();
    if (settled)
        room_stale_ = false;
    std::uint32_t screen = 0;
    const std::optional<std::uint32_t> room = room_tracker::current_screen(&screen) ? std::optional<std::uint32_t>(screen) : std::nullopt;
    peers_.tick(room, clock_, paused);
    if (paused)
        return;

    if (recorder_.due(clock_))
        if (std::vector<std::int64_t> wire = recorder_.add(peers_.streaming() ? sample() : std::nullopt); !wire.empty())
            peers_.send(std::move(wire));

    for (std::size_t i = 0; i < drawn_.size(); ++i)
    {
        ghost::Peers::Ghost &ghost = peers_.ghosts()[i];
        Drawn &d = drawn_[i];
        if (d.slot != ghost.slot)
        {
            despawn(world, d.weak);
            d = Drawn{};
            d.slot = ghost.slot;
        }
        show(world, d, ghost.playback.play(clock_), room, settled); // nullopt for a free entry
    }
}

void GhostPlayers::on_world_destroy()
{
    // The teardown frees the ghosts, so only the handles are left to drop.
    for (Drawn &d : drawn_)
    {
        mod::weak_ptr_destroy(d.weak);
        d.weak = nullptr;
    }
    room_stale_ = true;
}

bool GhostPlayers::can_draw()
{
    if (!drawable_)
    {
        init_ = reinterpret_cast<InitFn>(pal::resolve_game_symbol(sym::anim_effect_init));
        const bool api = mod::spawn_anim_api_available() && mod::entity_walk_api_available() && mod::weak_ptr_api_available() && mod::queue_destroy_available();
        drawable_ = init_ != nullptr && api;
        if (!*drawable_)
            pal::logf(pal::LogLevel::Warn, "ghost: cannot draw ghosts on this build (AnimEffect::Init %s, API entries %s)",
                      init_ != nullptr ? "found" : "unresolved", api ? "present" : "missing");
    }
    return *drawable_;
}

std::optional<ghost::Sample> GhostPlayers::sample()
{
    ghost::Sample s;
    // Mid-scroll the room being left is still right for anyone watching from there.
    if (room_stale_ || !room_tracker::current_screen(&s.room))
        return std::nullopt;
    void *player = mod::player_component();
    if (player == nullptr)
        return std::nullopt;
    void *entity = mod::component_parent(player);
    void *body = pal::at<void *>(player, layout::kGameComponentAnimOff);
    if (entity == nullptr || !pal::pointer_looks_valid(body) || !mod::component_isa(body, rtti::kGameAnim) || mod::component_parent(body) != entity)
        return std::nullopt;

    // The Player ctor moves the body's physics offset onto the AnimParent (Linux 0xd0d098, Windows 0x1404fcf34), which RecalcWorldTransform composes
    // in front of the local transform. Player code never sets its rotation.
    void *parent = pal::at<void *>(body, pal::layout::kGameAnimParentOff);
    if (!pal::pointer_looks_valid(parent) || !mod::component_isa(parent, rtti::kAnimParent))
    {
        warn_layout("the player body's AnimParent pointer");
        return std::nullopt;
    }
    mod::Transform root{};
    mod::Transform local{};
    if (!mod::entity_world_transform(entity, root) || !mod::game_anim_local_transform(body, local))
        return std::nullopt;
    const auto &p = pal::at<mod::Transform>(parent, layout::kAnimParentTransformOff);
    for (int i = 0; i < 3; ++i)
    {
        local.t[i] = p.s[i] * local.t[i] + p.t[i];
        local.s[i] *= p.s[i];
    }
    // As the engine composes it: ycTransform::Composite scales the local translation by the entity's (Giant cheats), and IsoAnim::CalcTransform adds
    // both z values unscaled to the drawn y (Linux 0xb71795). Rotation is left out, since only the camera traps rotate.
    for (int i = 0; i < 2; ++i)
    {
        s.pos[i] = root.t[i] + root.s[i] * local.t[i];
        s.scale[i] = root.s[i] * local.s[i];
    }
    s.pos[2] = root.t[2] + local.t[2];
    // The body sorts at the entity's y plus the AnimParent offset, without the local y (iso flag 0x4 is off for the Player).
    s.sort = root.t[1] + pal::at<float>(parent, layout::kAnimParentSortOff) - s.pos[1];
    if (!finite(s))
        return std::nullopt;

    const std::int32_t layer = pal::at<std::int32_t>(player, layout::kGameComponentCollisionLayerOff);
    const bool layer_ok = layer >= 0 && layer < ghost::kLayerLimit;
    if (!layer_ok)
        warn_layout("the player's collision layer");
    s.layer = layer_ok ? layer : 0;
    s.seq = hookhash::engine_hash(mod::game_anim_seq_name(body));
    s.frame = mod::game_anim_seq_frame_idx(body);
    s.visible = mod::game_anim_is_visible(body);
    s.sheet = sheet_hash(body);
    // An unknown sheet would freeze the ghost in its last pose; hidden reads better.
    if (ghost::find_sheet(s.sheet) == nullptr)
    {
        s.sheet = ghost::kSheets[0].hash;
        s.visible = false;
    }
    return s;
}

void GhostPlayers::warn_layout(const char *what)
{
    if (warned_layout_)
        return;
    warned_layout_ = true;
    pal::logf(pal::LogLevel::Warn, "ghost: %s does not read as expected on this build", what);
}

void GhostPlayers::show(void *world, Drawn &d, const std::optional<ghost::Sample> &sample, std::optional<std::uint32_t> room, bool settled)
{
    // Hidden mid-scroll: the engine culls the ghost when the scroll ends, so one respawned meanwhile would flash at the edge.
    if (!sample || !settled || sample->room != room || !can_draw())
    {
        despawn(world, d.weak);
        return;
    }
    void *effect = mod::weak_ptr_get(d.weak);
    // Beast form switches sheets, whose sequences the other sheet lacks.
    if (effect != nullptr && sample->sheet != d.sheet)
    {
        despawn(world, d.weak);
        effect = nullptr;
    }
    if (effect == nullptr)
    {
        // The room-transition cull frees ghosts too.
        mod::weak_ptr_destroy(d.weak);
        d.weak = nullptr;
        if (clock_ < d.next_spawn)
            return;
        effect = spawn(world, d, *sample);
        if (effect == nullptr)
            return;
    }
    apply(d, effect, *sample);
}

void *GhostPlayers::spawn(void *world, Drawn &d, const ghost::Sample &sample)
{
    void *root = mod::world_game_root_entity(world);
    if (root == nullptr)
        return nullptr;
    void *effect = mod::spawn_entity(root, kAnimEffectType, sample.pos, sample.layer);
    if (effect == nullptr)
    {
        fail("SpawnEntity2 returned null");
        return nullptr;
    }
    const ghost::Sheet &sheet = *ghost::find_sheet(sample.sheet); // decode() let only known sheets through
    const StrRef path{sheet.path.data(), sheet.path.size()};
    const StrRef seq{sheet.init_seq.data(), sheet.init_seq.size()};
    init_(effect, &path, &seq, nullptr);
    d.weak = mod::weak_ptr_create(effect);

    // Anything but a playing GameAnim on the requested sheet means Init or the cutter chain moved.
    void *anim = find_anim(effect);
    const std::string first(mod::game_anim_seq_name(anim));
    if (anim == nullptr || first.empty() || sheet_hash(anim) != sheet.hash)
    {
        fail("AnimEffect::Init left no playable GameAnim on the requested sheet");
        return nullptr;
    }
    // AnimEffect::Update kills the effect when its anim finishes: loop forever at rate 0, so only apply() moves the frame.
    mod::game_anim_play(anim, first.c_str(), -1, 0.0f, true);
    mod::game_anim_set_mul_color(anim, kGhostTint);
    if (!use_y_sort(anim))
    {
        fail("the GameAnim iso block does not hold AnimEffect::Init's sort mode");
        return nullptr;
    }
    d.sheet = sheet.hash;
    d.applied_seq = hookhash::engine_hash(first);
    d.missed_seq = 0;
    d.next_spawn = clock_ + kRespawnCooldown;
    return effect;
}

void GhostPlayers::apply(Drawn &d, void *effect, const ghost::Sample &sample)
{
    void *entity = mod::component_parent(effect);
    void *anim = find_anim(effect);
    if (anim == nullptr)
        return;
    // With a stored bias of 0, RecalcWorldTransform derives the depth bias from the owner's collision layer (Linux 0xce377d, Windows 0x14098925d).
    // Nothing else reads it on an AnimEffect.
    pal::at<std::int32_t>(effect, layout::kGameComponentCollisionLayerOff) = sample.layer;
    // Before the transforms, which re-sort and restore this field (Linux 0xce367d, 0xce3c2f).
    pal::at<float>(anim, pal::layout::kGameAnimIsoSortOff) = sample.sort;
    // The parent is the game root, so local is world.
    const mod::Transform place{{0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {sample.pos[0], sample.pos[1], sample.pos[2]}};
    mod::set_entity_local_transform(entity, place);
    const mod::Transform look{{0.0f, 0.0f, 0.0f, 1.0f}, {sample.scale[0], sample.scale[1], 1.0f}, {0.0f, 0.0f, 0.0f}};
    mod::set_game_anim_local_transform(anim, look);
    // A sequence the sheet lacks keeps the last one; remembering the miss saves a table scan per sample.
    if (sample.seq != d.applied_seq && sample.seq != d.missed_seq)
    {
        if (const char *name = sequence_name(anim, sample.seq))
        {
            mod::game_anim_play(anim, name, -1, 0.0f, true);
            d.applied_seq = sample.seq;
        }
        else
        {
            d.missed_seq = sample.seq;
        }
    }
    if (const std::optional<std::uint32_t> frame = ghost::clamp_frame(sample.frame, mod::game_anim_num_seq_frames(anim)))
        mod::game_anim_set_seq_frame_idx(anim, *frame);
    mod::game_anim_set_visible(anim, sample.visible);
}

void GhostPlayers::fail(const char *why)
{
    // What fails once fails for every ghost; the next step despawns them all. The stream carries on.
    pal::logf(pal::LogLevel::Warn, "ghost: %s; drawing off", why);
    drawable_ = false;
}

} // namespace mth
