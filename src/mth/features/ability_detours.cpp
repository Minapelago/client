#include "mth/features/ability_detours.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

#include "mod/mod_api.hpp"
#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/data/game_symbols.hpp"
#include "mth/core/sig_scan.hpp"
#include "pal/pal_game.hpp"
#include "pal/pal_layout.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"
#include "pal/pal_module.hpp"
#include "pal/scoped_hook.hpp"

// Every offset in this file is the same on both builds except the ones read from pal::layout: the
// TrainAuthority owner offset, the SetupBoxes patch bytes, and whether a SpringBellows listener body exists.
namespace
{
using mth::Ability;

// Player-object offsets used by the detours (Linux build 828346d4; the Windows Player struct matches).
constexpr std::ptrdiff_t kPlayerWaterListenerOff = 0x2c0; // WaterListener* (swim-vs-land discriminator)

// Pending bounce target (3 floats, FLT_MAX when none) and the bone-bounce variant marker. Player::OnBounce
// consumes both; its own early-out clears them, and the block path has to do the same or the target stays
// armed and fires late (#168).
constexpr std::ptrdiff_t kPlayerBounceTargetOff = 0x252c;
constexpr std::ptrdiff_t kPlayerBounceMarkerOff = 0x24c0;
// PhysicsContactPair -> colliding-entity component kind chain (shared by both CollideWith detours).
constexpr std::ptrdiff_t kContactEntityOff = 0x110;     // *(contactPair) + 0x110 -> entity
constexpr std::ptrdiff_t kEntityInteractCompOff = 0xa8; // entity + 0xa8 -> InteractComponent
constexpr std::ptrdiff_t kInteractKindOff = 0x6c;       // component + 0x6c -> int kind (8 == Player)
constexpr int kInteractKindPlayer = 8;
// TrainAuthority::OnNPCEvent case 0x15 selected-ticket-code chain; 100 = Exit (vanilla cancel).
constexpr unsigned kTrainDestPickEvent = 0x15;
// OnNPCEvent case 1 (interact/dialogue) and case 9 (TriggerRideDone/warp) carry the CTP boss ride gate.
constexpr unsigned kTrainInteractEvent = 1;
constexpr unsigned kTrainRideDoneEvent = 9;
constexpr std::ptrdiff_t kTrainMenuObjOff = 0xc8;  // owner + 0xc8 -> selection obj
constexpr std::ptrdiff_t kTrainSelCodeOff = 0x21c; // obj + 0x21c -> int selected ticket itemType
constexpr int kTrainExitCode = 100;
// SaveSlot generic Train Pass (item 94) owned byte. Set by Items::OnPickupDone on collect; gates boarding
// (train presence) under train_rando.
constexpr std::ptrdiff_t kSaveTrainPassOwnedOff = 0x1c0;
// SaveSlot train-present byte.
constexpr std::ptrdiff_t kSaveTrainPresentOff = 0x1c1;
// CTP boss (Thorne 2) defeated bit: byte +0x281 mask 0x02 of the SaveSlot+0x280 boss bitfield. The Coltrane
// line ride is gated on it (#108).
constexpr std::ptrdiff_t kSaveCtpBossByteOff = 0x281;
constexpr std::uint8_t kCtpBossGateMask = 0x02;

// train_rando destination gate, published from mth each tick. When active, repl_train_npc cancels any
// ticket line whose bit isn't in the granted mask (the +0x1e0 menu clamp can't hide lines 95/99); when
// inactive it falls back to the console Train-ability block.
std::uint32_t g_train_granted_mask = 0;
bool g_train_rando_gate = false;

mth::AbilityBlockFn g_ability_block;
bool g_ab_resolved = false;
bool g_ab_ok = false;

// Resolved targets (cached by abilities_available).
std::uintptr_t g_addr_burrow_ground = 0;
std::uintptr_t g_addr_rope_climb = 0;
std::uintptr_t g_addr_bounce_plant = 0;
std::uintptr_t g_addr_bounce_launch = 0;
std::uintptr_t g_addr_on_bounce = 0;
std::uintptr_t g_addr_spring = 0;
std::uintptr_t g_addr_spring_listener = 0;
std::uintptr_t g_addr_pickup = 0;
std::uintptr_t g_addr_train_npc = 0;
std::uintptr_t g_addr_burrow_jump = 0; // #56

[[clang::no_destroy]] pal::ScopedHook g_burrow_hook;
[[clang::no_destroy]] pal::ScopedHook g_rope_hook;
[[clang::no_destroy]] pal::ScopedHook g_puff_hook;
[[clang::no_destroy]] pal::ScopedHook g_launch_hook;
[[clang::no_destroy]] pal::ScopedHook g_on_bounce_hook;
[[clang::no_destroy]] pal::ScopedHook g_spring_hook;
[[clang::no_destroy]] pal::ScopedHook g_spring_listener_hook;
[[clang::no_destroy]] pal::ScopedHook g_carry_hook;
[[clang::no_destroy]] pal::ScopedHook g_train_hook;
[[clang::no_destroy]] pal::ScopedHook g_burrow_jump_hook; // #56

unsigned long (*g_orig_burrow_ground)(void *) = nullptr;
void (*g_orig_rope_climb)(void *, void *, bool, bool) = nullptr;
void (*g_orig_bounce_plant)(void *, void *) = nullptr;
void (*g_orig_bounce_launch)(void *, void *) = nullptr;
void (*g_orig_on_bounce)(void *) = nullptr;
void (*g_orig_spring)(void *, void *) = nullptr;
void (*g_orig_spring_listener)(void *, void *) = nullptr;
unsigned long (*g_orig_pickup)(void *, bool, bool, bool) = nullptr;
void (*g_orig_train_npc)(void *, unsigned, void *) = nullptr;
void (*g_orig_burrow_jump)(void *) = nullptr; // #56

mth::BurrowCommitFn g_burrow_commit; // #163: burrow lifetime observers (arm/disarm the boundary watcher)
mth::BurrowEmergeFn g_burrow_emerge;

bool ability_blocked(Ability a)
{
    return g_ability_block && g_ability_block(a);
}

// Reads the swim-vs-land discriminator off a player. Shared by the commit classifier and the per-tick
// boundary poll so both see exactly the same signal.
bool player_in_deep_water(void *player)
{
    if (player == nullptr)
        return false;
    void *wl = pal::at<void *>(player, kPlayerWaterListenerOff);
    return mod::water_is_in_deep_water(wl, false);
}

// Free-roam burrow classify-and-commit: deep water => Swim, else Burrow. Scripted/underlab
// entrances route through SetBurrowInObject (unhooked).
unsigned long repl_burrow_ground(void *self)
{
    bool deep = false;
    if (self != nullptr)
    {
        deep = player_in_deep_water(self);
        if (ability_blocked(deep ? Ability::Swim : Ability::Burrow))
            return 0;
    }
    const unsigned long r = g_orig_burrow_ground ? g_orig_burrow_ground(self) : 0;
    // A commit we let through: the boundary watcher now knows which mode the player is in, so it can catch
    // the game flipping between them mid-burrow without reading the mode field (#163).
    if (self != nullptr && g_burrow_commit)
        g_burrow_commit(deep);
    return r;
}

// RopeClimbStart is the attach funnel; per-frame climb movement is a separate path (unhooked).
void repl_rope_climb(void *self, void *rope, bool a, bool b)
{
    if (ability_blocked(Ability::RopeClimb))
        return;
    if (g_orig_rope_climb)
        g_orig_rope_climb(self, rope, a, b);
}

// Both CollideWith funnels reach the colliding entity's InteractComponent the same way.
bool collider_is_player(void *contact_pair)
{
    if (contact_pair == nullptr)
        return false;
    void *cp0 = *reinterpret_cast<void **>(contact_pair);
    if (cp0 == nullptr)
        return false;
    void *entity = pal::at<void *>(cp0, kContactEntityOff);
    if (entity == nullptr)
        return false;
    void *comp = pal::at<void *>(entity, kEntityInteractCompOff);
    if (comp == nullptr)
        return false;
    return pal::at<int>(comp, kInteractKindOff) == kInteractKindPlayer;
}

void repl_bounce_plant(void *self, void *contact_pair)
{
    if (collider_is_player(contact_pair) && ability_blocked(Ability::BouncePuff))
        return;
    if (g_orig_bounce_plant)
        g_orig_bounce_plant(self, contact_pair);
}

// Out-of-line launch for ground/burrow-underable puffs (player is the second arg, always the launchee);
// the floating case bounces inline in CollideWith above (issue #47).
void repl_bounce_launch(void *self, void *player)
{
    if (ability_blocked(Ability::BouncePuff))
        return;
    if (g_orig_bounce_launch)
        g_orig_bounce_launch(self, player);
}

// Player::OnBounce is the only consumer of the pending target and the only writer of the launch velocity,
// so it catches every bounce surface, including the Bone Beach bushes, which are Breakables reached by a
// dispatch inlined into Player::Update / SlideOutOfWall that neither detour above sees (#168). The listener
// sweep calls it unconditionally every tick, so the blocked path must stay allocation- and log-free.
void repl_on_bounce(void *player)
{
    if (player != nullptr && ability_blocked(Ability::BouncePuff))
    {
        float *target = &pal::at<float>(player, kPlayerBounceTargetOff);
        target[0] = target[1] = target[2] = std::numeric_limits<float>::max();
        pal::at<char>(player, kPlayerBounceMarkerOff) = 0;
        return;
    }
    if (g_orig_on_bounce)
        g_orig_on_bounce(player);
}

// Shared by both SpringBellows::CollideWith bodies (see sym::spring_bellows_collide_listener) on a build
// that has two (pal::layout::kHasSpringListenerBody); the other build only has the exported one. The tag
// records which one physics actually dispatched to; it cannot see a build that turns the thunk into a
// real adjustor jmp, only one that drops the thunk symbol entirely.
//
// Latched, not per-call: the listener sweep re-dispatches CollideWith every tick that contact persists,
// and blocking the launch is what makes the player stay in contact, so an unlatched log would fflush at
// frame rate for as long as Mina stands on the spring.
bool spring_blocked(void *contact_pair, const char *body, bool &logged)
{
    if (!collider_is_player(contact_pair) || !ability_blocked(Ability::BounceSpring))
        return false;
    if (!logged)
    {
        logged = true;
        pal::logf(pal::LogLevel::Debug, "abilities: spring launch blocked (%s body)", body);
    }
    return true;
}

void repl_spring(void *self, void *contact_pair)
{
    static bool logged = false;
    if (spring_blocked(contact_pair, "exported", logged))
        return;
    if (g_orig_spring)
        g_orig_spring(self, contact_pair);
}

void repl_spring_listener(void *self, void *contact_pair)
{
    static bool logged = false;
    if (spring_blocked(contact_pair, "listener", logged))
        return;
    if (g_orig_spring_listener)
        g_orig_spring_listener(self, contact_pair);
}

// Blocked: just refuse the grab. This used to also set Player+0x12f0, mislabelled a "low roof pose" flag:
// it actually arms the burrow-landing shockwave and delays the hop-out. The game probes for carryables on
// ledge jumps and landings too, so every probe armed a spurious shockwave. Holding Mina under an overhead
// carryable is repl_burrow_jump's emerge-suppress, not a flag poke.
unsigned long repl_pickup(void *self, bool a, bool b, bool c)
{
    if (self != nullptr && ability_blocked(Ability::Carry))
        return 0;
    return g_orig_pickup ? g_orig_pickup(self, a, b, c) : 0;
}

// #56: no native "duck under a carryable" exists (the burrow-emerge roof/snap handles only low map
// ceilings), so with carry disabled we detect a carryable in grab range and suppress the emerge, leaving
// Mina burrowed beneath it. carryable_overhead is a read-only replica of the game's own grab query.
// Offsets from Player::PickUpAnyNearbyCarryableObject (0xd515b0, build 8de7a6b5). The two query calls go
// through the native API (mod::physics_get_aabb / mod::closest_carryable), so no AABB struct is needed
// here: the box is six floats, center.xyz then half-extent.xyz.
bool carryable_overhead(void *self)
{
    if (self == nullptr)
        return false;
    void *e0 = pal::at<void *>(self, 0x278);
    void *E = e0 != nullptr ? pal::at<void *>(e0, 0x70) : nullptr;
    if (E == nullptr)
        return false;
    void *comp = pal::at<void *>(E, 0x10);
    void *phys = pal::at<void *>(E, 0xb0);
    const int layer = pal::at<int>(E, 0x15c);
    void *w0 = pal::at<void *>(self, 0x10);
    void *w1 = w0 != nullptr ? pal::at<void *>(w0, 0x50) : nullptr;
    void *mgr = w1 != nullptr ? pal::at<void *>(w1, 0x1748) : nullptr;
    if (phys == nullptr || comp == nullptr || mgr == nullptr)
        return false;

    float box[6]{};
    if (!mod::physics_get_aabb(phys, box, false, 0u))
        return false;
    // The game clamps the box's z half-extent to |comp+0xd8| * 1.6 * 0.3 before the query.
    const float fd8 = pal::at<float>(comp, 0xd8);
    const float clamp = (fd8 < 0.0f ? -fd8 : fd8) * 1.6f * 0.3f;
    if (clamp < box[5])
        box[5] = clamp;

    int out_n = 0;
    return mod::closest_carryable(mgr, box, layer, 1.6f, &out_n, 0ull) != nullptr; // radius 1.6, mask 0 (emerge args)
}

void repl_burrow_jump(void *self)
{
    if (self != nullptr && ability_blocked(Ability::Carry) && carryable_overhead(self))
    {
        pal::logf(pal::LogLevel::Debug, "carry: burrow-emerge suppressed (carryable overhead)");
        return; // still burrowed, so the boundary watcher keeps its arm
    }
    if (g_orig_burrow_jump)
        g_orig_burrow_jump(self);
    if (g_burrow_emerge)
        g_burrow_emerge();
}

// TrainAuthority::OnNPCEvent case 0x15 picks a destination by ticket itemType. Forcing the selected
// code to Exit (100) makes vanilla treat it as a cancel.
void repl_train_npc(void *self, unsigned event, void *info)
{
    if (self != nullptr && event == kTrainDestPickEvent)
    {
        void *owner = pal::at<void *>(self, pal::layout::kTrainAuthOwnerOff);
        void *obj = owner != nullptr ? pal::at<void *>(owner, kTrainMenuObjOff) : nullptr;
        if (obj != nullptr)
        {
            int *code = &pal::at<int>(obj, kTrainSelCodeOff);
            // Backstop to the SetupBoxes patch: cancel a picked line that isn't AP-granted.
            const bool block = g_train_rando_gate ? mth::train_destination_blocked(*code, g_train_granted_mask) : ability_blocked(Ability::Train);
            if (block)
                *code = kTrainExitCode;
        }
    }

    // #108: the Coltrane line ride is gated on the CTP boss (Thorne 2), softlocking a player who leaves CTP
    // first. Once the pass is owned, set the boss bit only across the original call so the ride completes,
    // restoring it after so the save is never marked (the boss still arms and fights).
    void *slot = nullptr;
    std::uint8_t saved_boss = 0;
    bool bypass = false;
    if (event == kTrainInteractEvent || event == kTrainRideDoneEvent)
    {
        slot = pal::active_save_slot();
        if (slot != nullptr && pal::at<std::uint8_t>(slot, kSaveTrainPassOwnedOff) != 0)
        {
            auto *gate = &pal::at<std::uint8_t>(slot, kSaveCtpBossByteOff);
            saved_boss = *gate;
            *gate = static_cast<std::uint8_t>(saved_boss | kCtpBossGateMask);
            bypass = true;
        }
    }
    if (g_orig_train_npc)
        g_orig_train_npc(self, event, info);
    if (bypass)
        pal::at<std::uint8_t>(slot, kSaveCtpBossByteOff) = saved_boss;
}

// One-time .text patch of ShopMenu::SetupBoxes' always-shown train-line test, which makes Coltrane Peak
// (99) selectable regardless of ticket and defeats the +0x1e0 clamp. pal::layout::kSetupBoxes* holds each
// build's anchor and the byte to rewrite; afterwards 99 gates on +0x1e0 (driven by enforce_train_destinations)
// while 94 (board) and 95 (Ossex/HUB) stay always-shown. #98.
void patch_train_destination_menu()
{
    namespace layout = pal::layout;
    const pal::ModuleInfo gm = pal::game_module();
    if (gm.base == 0 || gm.size == 0)
    {
        pal::logf(pal::LogLevel::Warn, "train: game module unavailable; SetupBoxes mask patch skipped (99 stays always-shown)");
        return;
    }
    const std::span<const std::uint8_t> region{reinterpret_cast<const std::uint8_t *>(gm.base), gm.size};
    const mth::sig::Match m = mth::sig::find_masked(region, layout::kSetupBoxesPattern, layout::kSetupBoxesMask, sizeof(layout::kSetupBoxesPattern));
    if (!m.found || !m.unique)
    {
        pal::logf(pal::LogLevel::Warn, "train: SetupBoxes mask site %s; Coltrane may stay always-shown", m.found ? "ambiguous" : "not found");
        return;
    }
    auto *site = reinterpret_cast<std::uint8_t *>(gm.base + m.offset + layout::kSetupBoxesSiteIndex);
    if (*site != layout::kSetupBoxesExpected)
    {
        pal::logf(pal::LogLevel::Warn, "train: SetupBoxes patch byte=0x%02x (expected 0x%02x); patch skipped", *site, layout::kSetupBoxesExpected);
        return;
    }
    if (!pal::patch_code(site, &layout::kSetupBoxesPatched, 1))
    {
        pal::logf(pal::LogLevel::Error, "train: SetupBoxes mask patch_code failed");
        return;
    }
    pal::logf(pal::LogLevel::Info, "train: SetupBoxes patch 0x%02x->0x%02x at 0x%llx (un-ticketed Coltrane now non-selectable)", layout::kSetupBoxesExpected,
              layout::kSetupBoxesPatched, static_cast<unsigned long long>(gm.base + m.offset + layout::kSetupBoxesSiteIndex));
}

// True once at least one ability chokepoint symbol resolves on this platform.
bool abilities_available()
{
    if (g_ab_resolved)
        return g_ab_ok;
    g_ab_resolved = true;
    g_addr_burrow_ground = pal::resolve_game_symbol(mth::sym::player_set_burrow_ground);
    g_addr_rope_climb = pal::resolve_game_symbol(mth::sym::player_rope_climb_start);
    g_addr_bounce_plant = pal::resolve_game_symbol(mth::sym::bounce_plant_collide);
    g_addr_bounce_launch = pal::resolve_game_symbol(mth::sym::bounce_plant_launch);
    g_addr_on_bounce = pal::resolve_game_symbol(mth::sym::player_on_bounce);
    g_addr_spring = pal::resolve_game_symbol(mth::sym::spring_bellows_collide);
    if (pal::layout::kHasSpringListenerBody)
        g_addr_spring_listener = pal::resolve_game_symbol(mth::sym::spring_bellows_collide_listener);
    g_addr_pickup = pal::resolve_game_symbol(mth::sym::player_pickup_carryable);
    g_addr_train_npc = pal::resolve_game_symbol(mth::sym::train_authority_on_npc_event);
    g_addr_burrow_jump = pal::resolve_game_symbol(mth::sym::mina_on_burrow_jump); // #56
    g_ab_ok = g_addr_burrow_ground != 0 || g_addr_rope_climb != 0 || g_addr_bounce_plant != 0 || g_addr_bounce_launch != 0 || g_addr_on_bounce != 0 ||
              g_addr_spring != 0 || g_addr_spring_listener != 0 || g_addr_pickup != 0 || g_addr_train_npc != 0;
    if (!g_ab_ok)
        pal::logf(pal::LogLevel::Warn, "abilities: no chokepoint symbols resolved; ability gating disabled");
    return g_ab_ok;
}

} // namespace

namespace mth
{

bool install_ability_hooks(AbilityBlockFn block)
{
    if (!abilities_available())
        return false;
    g_ability_block = std::move(block);

    if (g_addr_burrow_ground != 0)
    {
        g_burrow_hook = pal::ScopedHook(g_addr_burrow_ground, &repl_burrow_ground, &g_orig_burrow_ground);
        if (!g_burrow_hook.installed())
            pal::logf(pal::LogLevel::Error, "abilities: failed to hook Player::SetBurrowGround");
    }
    else
        pal::logf(pal::LogLevel::Warn, "abilities: Player::SetBurrowGround not resolved; burrow/swim gating disabled");
    if (!mod::water_api_available())
        pal::logf(pal::LogLevel::Warn, "abilities: WaterListenerIsInDeepWater unavailable; swim-vs-land treated as land");

    if (g_addr_rope_climb != 0)
    {
        g_rope_hook = pal::ScopedHook(g_addr_rope_climb, &repl_rope_climb, &g_orig_rope_climb);
        if (!g_rope_hook.installed())
            pal::logf(pal::LogLevel::Error, "abilities: failed to hook Player::RopeClimbStart");
    }
    else
        pal::logf(pal::LogLevel::Warn, "abilities: Player::RopeClimbStart not resolved; rope gating disabled");

    if (g_addr_bounce_plant != 0)
    {
        g_puff_hook = pal::ScopedHook(g_addr_bounce_plant, &repl_bounce_plant, &g_orig_bounce_plant);
        if (!g_puff_hook.installed())
            pal::logf(pal::LogLevel::Error, "abilities: failed to hook BouncePlant::CollideWith");
    }
    else
        pal::logf(pal::LogLevel::Warn, "abilities: BouncePlant::CollideWith not resolved; puff gating disabled");

    if (g_addr_bounce_launch != 0)
    {
        g_launch_hook = pal::ScopedHook(g_addr_bounce_launch, &repl_bounce_launch, &g_orig_bounce_launch);
        if (!g_launch_hook.installed())
            pal::logf(pal::LogLevel::Error, "abilities: failed to hook BouncePlant::BounceLaunch");
    }
    else
        pal::logf(pal::LogLevel::Warn, "abilities: BouncePlant::BounceLaunch not resolved; ground-puff gating disabled");

    if (g_addr_on_bounce != 0) // #168: universal launch gate (bounce bushes and any other non-plant surface)
    {
        g_on_bounce_hook = pal::ScopedHook(g_addr_on_bounce, &repl_on_bounce, &g_orig_on_bounce);
        if (!g_on_bounce_hook.installed())
            pal::logf(pal::LogLevel::Error, "abilities: failed to hook Player::OnBounce");
    }
    else
        pal::logf(pal::LogLevel::Warn, "abilities: Player::OnBounce not resolved; bounce-bush gating disabled");

    if (g_addr_spring != 0)
    {
        g_spring_hook = pal::ScopedHook(g_addr_spring, &repl_spring, &g_orig_spring);
        if (!g_spring_hook.installed())
            pal::logf(pal::LogLevel::Error, "abilities: failed to hook SpringBellows::CollideWith");
    }
    else
        pal::logf(pal::LogLevel::Warn, "abilities: SpringBellows::CollideWith not resolved; spring gating disabled");

    // On a build with a listener body, that copy is the one physics dispatches to and the exported one
    // above is unreachable (#188).
    if (pal::layout::kHasSpringListenerBody)
    {
        if (g_addr_spring_listener != 0)
        {
            g_spring_listener_hook = pal::ScopedHook(g_addr_spring_listener, &repl_spring_listener, &g_orig_spring_listener);
            if (!g_spring_listener_hook.installed())
                pal::logf(pal::LogLevel::Error, "abilities: failed to hook SpringBellows::CollideWith (listener body)");
        }
        else
            pal::logf(pal::LogLevel::Warn, "abilities: SpringBellows::CollideWith listener body not resolved; springs launch ungated");
    }

    if (g_addr_pickup != 0)
    {
        g_carry_hook = pal::ScopedHook(g_addr_pickup, &repl_pickup, &g_orig_pickup);
        if (!g_carry_hook.installed())
            pal::logf(pal::LogLevel::Error, "abilities: failed to hook Player::PickUpAnyNearbyCarryableObject");
    }
    else
        pal::logf(pal::LogLevel::Warn, "abilities: Player::PickUpAnyNearbyCarryableObject not resolved; carry gating disabled");

    if (g_addr_train_npc != 0)
    {
        g_train_hook = pal::ScopedHook(g_addr_train_npc, &repl_train_npc, &g_orig_train_npc);
        if (!g_train_hook.installed())
            pal::logf(pal::LogLevel::Error, "abilities: failed to hook TrainAuthority::OnNPCEvent");
    }
    else
        pal::logf(pal::LogLevel::Warn, "abilities: TrainAuthority::OnNPCEvent not resolved; train gating disabled");

    patch_train_destination_menu(); // make un-ticketed Coltrane Peak (line 99) non-selectable (#98)

    if (g_addr_burrow_jump != 0) // #56: OnBurrowJump: suppress emerge under a carryable
    {
        g_burrow_jump_hook = pal::ScopedHook(g_addr_burrow_jump, &repl_burrow_jump, &g_orig_burrow_jump);
        if (!g_burrow_jump_hook.installed())
            pal::logf(pal::LogLevel::Warn, "abilities: failed to hook Mina::OnBurrowJump (#56)");
    }
    else
        pal::logf(pal::LogLevel::Warn, "abilities: Mina::OnBurrowJump not resolved; carry emerge-suppress disabled");

    const bool any = g_burrow_hook.installed() || g_rope_hook.installed() || g_puff_hook.installed() || g_launch_hook.installed() ||
                     g_on_bounce_hook.installed() || g_spring_hook.installed() || g_spring_listener_hook.installed() || g_carry_hook.installed() ||
                     g_train_hook.installed() || g_burrow_jump_hook.installed();
    if (any)
        pal::logf(pal::LogLevel::Info, "abilities: ability gating hooks installed");
    else
        g_ability_block = nullptr;
    return any;
}

void remove_ability_hooks()
{
    for (pal::ScopedHook *hook : {&g_burrow_hook, &g_rope_hook, &g_puff_hook, &g_launch_hook, &g_on_bounce_hook, &g_spring_hook, &g_spring_listener_hook,
                                  &g_carry_hook, &g_train_hook, &g_burrow_jump_hook})
        hook->reset();
    g_ability_block = nullptr;
    g_burrow_commit = nullptr;
    g_burrow_emerge = nullptr;
}

void set_burrow_observers(BurrowCommitFn on_commit, BurrowEmergeFn on_emerge)
{
    g_burrow_commit = std::move(on_commit);
    g_burrow_emerge = std::move(on_emerge);
}

int burrow_water_state(void *player)
{
    if (player == nullptr || !mod::water_api_available())
        return -1;
    void *wl = pal::at<void *>(player, kPlayerWaterListenerOff);
    if (wl == nullptr)
        return -1;
    return mod::water_is_in_deep_water(wl, false) ? 1 : 0;
}

bool force_burrow_emerge(void *player)
{
    if (player == nullptr || g_orig_burrow_jump == nullptr)
        return false;
    g_orig_burrow_jump(player); // the original, not the detour: this emerge is forced, not player-initiated
    return true;
}

void enforce_train_presence(bool blocked)
{
    if (!blocked)
        return; // arrival event re-shows the train
    void *slot = pal::active_save_slot();
    if (slot == nullptr)
        return;
    pal::at<char>(slot, kSaveTrainPresentOff) = 0;
}

void enforce_train_boarding()
{
    void *slot = pal::active_save_slot();
    if (slot == nullptr)
        return;
    // Require the generic Train Pass (#98): while +0x1c0 (pass owned) is 0, keep the train hidden so it
    // cannot be boarded. Once the pass is received (OnPickupDone sets +0x1c0) leave the story-set presence.
    if (pal::at<std::uint8_t>(slot, kSaveTrainPassOwnedOff) == 0)
        pal::at<char>(slot, kSaveTrainPresentOff) = 0;
}

void set_train_destination_gate(std::uint32_t granted_mask, bool rando_active)
{
    g_train_granted_mask = granted_mask;
    g_train_rando_gate = rando_active;
}

} // namespace mth
