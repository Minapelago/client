#include "mth/features/ability_hooks.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "mod/mod_api.hpp"
#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/ap/ap_state.hpp"
#include "mth/features/ability_detours.hpp"
#include "pal/pal_game.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"

// The Player state machine and SaveSlot train fields below match on both builds; the g_saveManager deref is behind pal::active_save_slot. The burrow observers,
// forced emerge and the train-presence and boarding writes live in ability_detours.cpp because they touch the detour statics or share offsets with the
// TrainAuthority detour.
namespace
{

// Player state machine (#163): current / requested / pending-request. Requesting kStateDeepWaterFall runs the game's on-foot deep-water fall (fall, respawn at
// shore, pit damage on its own timer). kStateMax is the InitState/UpdateState jump-table bound, a sanity check on the offset.
constexpr std::ptrdiff_t kPlayerStateCurOff = 0x254;
constexpr std::ptrdiff_t kPlayerStateReqOff = 0x25c;
constexpr std::ptrdiff_t kPlayerStatePendOff = 0x260;
constexpr int kStateBurrow = 0x1c; // current state while burrowing (ground or water)
constexpr int kStateDeepWaterFall = 0xf1;
constexpr int kStateMax = 0xf5;

// SaveSlot unlocked-train-lines bitfield (5 low bits, one per destination), set by the TrainAuthority ctor on station footfall and clamped by the client to the
// granted-ticket mask.
constexpr std::ptrdiff_t kSaveTrainUnlockedLinesOff = 0x1e0;
// SaveSlot donation-machine progress (u32 uTicketProgress). The machine reads it fresh every tick and never resets it, and its only readers are the goal
// compare, the progress bar and the deposit dial, so raising it lowers the donation cost.
constexpr std::ptrdiff_t kSaveTicketProgressOff = 0x1bc;

// True only while the player is in the burrow state. A burrow also ends via damage, get-hit, death, the pit check and the land/idle transitions, none of which
// reach the emerge commit, so callers confirm the arm with this each tick: forcing an emerge on an unburrowed player runs a carryable pickup, rewrites facing
// and can teleport them. Reads the state field because the burrow-medium offset drifts. Game thread only.
bool player_is_burrowing(void *player)
{
    if (player == nullptr)
        return false;
    const int cur = pal::at<int>(player, kPlayerStateCurOff);
    return cur == kStateBurrow;
}

// Requests the game's on-foot deep-water fall state, which then drives the whole recovery (animation and sound, shore respawn, pit damage and whether it is
// fatal). The teleport and damage live in later states on the state machine's timer, so requesting the state is the only way in. Game thread only.
bool request_deep_water_fall(void *player)
{
    if (player == nullptr)
        return false;
    const int cur = pal::at<int>(player, kPlayerStateCurOff);
    // Backstop for a caller that skipped player_is_burrowing(): out of the dispatch table's range, this is not the state field on this build.
    if (cur < 0 || cur > kStateMax)
    {
        pal::logf(pal::LogLevel::Warn, "abilities: player state reads %d (out of range); deep-water fall not requested", cur);
        return false;
    }
    if (cur == kStateDeepWaterFall)
        return true; // already falling
    pal::at<int>(player, kPlayerStateReqOff) = kStateDeepWaterFall;
    pal::at<std::uint8_t>(player, kPlayerStatePendOff) = 1; // the game writes this as a byte
    return true;
}

// Clamps the SaveSlot unlocked-train-lines bitfield to line_mask (bit N = line N travelable), undoing the station-visit auto-unlock (#98) each frame. This only
// hides boxes: the menu builder always shows lines 95/99, so the OnNPCEvent destination gate is what refuses the warp. Game thread only.
void enforce_train_destinations(std::uint32_t line_mask)
{
    void *slot = pal::active_save_slot();
    if (slot == nullptr)
        return;
    // The footfall unlock only ORs bits in, so writing the granted mask each frame clears any line it set.
    pal::at<std::uint8_t>(slot, kSaveTrainUnlockedLinesOff) = static_cast<std::uint8_t>(line_mask & 0xffu);
}

// Raises the donation machine's progress counter to `seed`, never lowering it. The completion goal is compiled in, so pre-paying part of it lowers the cost,
// and the game caps the dial-in at what is still owed (#162). seed 0 keeps the vanilla cost. Game thread only.
void seed_ticket_machine_progress(std::uint32_t seed)
{
    if (seed == 0)
        return;
    void *slot = pal::active_save_slot();
    if (slot == nullptr)
        return;
    // Raise only: a player past the seed keeps their progress, and a deposit in flight is not undone.
    auto *progress = &pal::at<std::uint32_t>(slot, kSaveTicketProgressOff);
    if (*progress < seed)
        *progress = seed;
}

} // namespace

namespace mth
{

AbilityHooks::AbilityHooks(std::function<bool(std::int64_t item_id)> is_granted) : is_granted_(std::move(is_granted))
{
    const bool installed = install_ability_hooks([this](Ability a) { return gate_.blocked(a, enforce_, is_granted_); });
    if (!installed)
        pal::logf(pal::LogLevel::Warn, "AbilityHooks: no ability chokepoints resolved; gating disabled");

    // Tracks the burrow lifetime from the two hooked commit sites so the boundary poll knows the mode without reading the drifting mode field (#163).
    set_burrow_observers([this](bool deep) { burrow_.arm(deep); }, [this] { burrow_.disarm(); });
}

void AbilityHooks::set_randomized(Ability a, bool on)
{
    gate_.set_randomized(a, on);
}

void AbilityHooks::set_enforce(bool on)
{
    enforce_ = on;
}

void AbilityHooks::set_train_gate(bool rando_active, const ApState &state)
{
    std::uint32_t line_mask = 0;
    for (const auto &it : state.received_items())
        if (is_vanilla_game_item(it.item_id))
            line_mask |= train_ticket_bit(game_item_type(it.item_id));
    train_rando_active_ = rando_active;
    train_mask_ = line_mask;
    set_train_destination_gate(line_mask, rando_active); // published to the OnNPCEvent detour
}

void AbilityHooks::set_ticket_machine(bool is_ap_location, bool checked, std::uint32_t progress_seed)
{
    // A live check keeps its prompt; a non-location or already-donated machine loses it. Reporting the location collected would make the conductor treat the
    // pass as owned and activate the train without it.
    ticket_machine_suppress_ = !is_ap_location || checked;
    ticket_machine_seed_ = progress_seed;
}

void AbilityHooks::enforce_train_tick()
{
    // train_rando: boarding requires the Train Pass (#98) and each destination its AP ticket. The +0x1e0 clamp undoes the footfall auto-unlock and the
    // OnNPCEvent detour (fed by set_train_gate) refuses ungranted warps. Otherwise the console-driven Train ability gate hides the conductor while blocked.
    if (train_rando_active_)
    {
        enforce_train_boarding();
        enforce_train_destinations(train_mask_);
        // The donation machine hands out the same pass (#162): as a live check the seed makes it affordable, otherwise the gate stops its prompt.
        // The seed is a durable write, so it waits for the bound AP save; seeding another save leaves a near-free pass in vanilla play. The disable is
        // in-memory and dies with the room, and missing it is worse (the machine takes 10000 bones and returns nothing), so it runs without a bound save. Keep
        // the two conditions different.
        if (ticket_machine_suppress_)
        {
            ticket_machine_.tick();
        }
        else if (enforce_)
        {
            seed_ticket_machine_progress(ticket_machine_seed_);
        }
        else if (!warned_seed_withheld_)
        {
            // Otherwise slot_data's train_pass_cost is ignored with nothing in the log to say why.
            warned_seed_withheld_ = true;
            pal::logf(pal::LogLevel::Warn, "train: donation cost left at vanilla; the live save is not the bound AP slot (#162)");
        }
    }
    else
    {
        enforce_train_presence(gate_.blocked(Ability::Train, enforce_, is_granted_));
    }
}

void AbilityHooks::enforce_burrow_tick(void *player)
{
    // Any change of player, or none at all, drops the arm: acting on a player who is not burrowing is worse
    // than the sequence break this closes.
    if (player == nullptr || player != last_player_)
    {
        last_player_ = player;
        burrow_.disarm();
        return;
    }
    if (mod::world_is_paused())
        return; // frozen frame: do not advance the confirmation on a stale reading
    const bool burrowing = player_is_burrowing(player);
    burrow_.observe_burrowing(burrowing);
    if (!burrowing || !enforce_ || !burrow_.armed())
        return;
    const int water = burrow_water_state(player);
    if (water < 0)
    {
        burrow_.reading_unavailable();
        return;
    }

    switch (burrow_.observe(water == 1, gate_.blocked(Ability::Burrow, enforce_, is_granted_), gate_.blocked(Ability::Swim, enforce_, is_granted_)))
    {
    case BoundaryAction::FallIn:
        pal::logf(pal::LogLevel::Info, "abilities: burrowed into deep water without Swim -> falling in (%d)", request_deep_water_fall(player) ? 1 : 0);
        break;
    case BoundaryAction::Emerge:
        pal::logf(pal::LogLevel::Info, "abilities: swam onto land without Burrow -> surfacing (%d)", force_burrow_emerge(player) ? 1 : 0);
        break;
    case BoundaryAction::None:
        break;
    }
}

void AbilityHooks::on_world_destroy()
{
    burrow_.disarm();
    last_player_ = nullptr;
    ticket_machine_.on_world_destroy(); // walk the next room's machine on the next tick
}

} // namespace mth
