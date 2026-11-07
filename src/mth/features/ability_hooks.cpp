#include "mth/features/ability_hooks.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "mod/mod_api.hpp"
#include "mth/features/ability_detours.hpp"
#include "pal/pal_game.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"

// The Player state machine and the SaveSlot train fields below have the same layout on both builds, and
// the one platform-divergent read, the g_saveManager deref, is behind pal::active_save_slot. The burrow
// observers, the forced emerge and the train-presence and boarding writes live in ability_detours.cpp:
// they touch the ability-detour statics or share offsets with the TrainAuthority detour.
namespace
{

// Player state machine (#163): current / requested / pending-request. Requesting kStateDeepWaterFall runs the
// game's own on-foot deep-water fall, which chains fall -> respawn-at-shore -> pit damage on its own timer.
// kStateMax is the InitState/UpdateState jump-table bound, used only as a sanity check on the offset.
constexpr std::ptrdiff_t kPlayerStateCurOff = 0x254;
constexpr std::ptrdiff_t kPlayerStateReqOff = 0x25c;
constexpr std::ptrdiff_t kPlayerStatePendOff = 0x260;
constexpr int kStateBurrow = 0x1c; // current state while burrowing (ground or water)
constexpr int kStateDeepWaterFall = 0xf1;
constexpr int kStateMax = 0xf5;

// SaveSlot unlocked-train-lines bitfield (5 low bits, one per destination). Set by the TrainAuthority ctor
// on station footfall; the AP client clamps it to the granted-ticket mask. Same layout on both platforms.
constexpr std::ptrdiff_t kSaveTrainUnlockedLinesOff = 0x1e0;
// SaveSlot donation-machine progress (u32 uTicketProgress). Read fresh by the machine every tick and never
// reset, with exactly three readers (the state machine's goal compare, the progress bar, the deposit dial),
// so raising it is a complete way to lower the donation cost. Same layout on both platforms.
constexpr std::ptrdiff_t kSaveTicketProgressOff = 0x1bc;

// True only while the player is actually in the burrow state. The commit/emerge observers are not enough on
// their own: a burrow also ends via damage, get-hit, death, the pit check and the land/idle transitions, none
// of which reach the emerge commit. Callers confirm the arm with this every tick, because forcing an emerge on
// a player who is NOT burrowed is destructive (it runs a carryable pickup, rewrites facing, and can teleport
// them). Reads the state field, not the burrow-medium field whose offset drifts. Game-thread only.
bool player_is_burrowing(void *player)
{
    if (player == nullptr)
        return false;
    const int cur = pal::at<int>(player, kPlayerStateCurOff);
    return cur == kStateBurrow;
}

// Hands the player to the game's OWN on-foot deep-water fall by requesting that player state: the game then
// drives the whole recovery itself (fall animation and sound, respawn at the shore, pit damage, and whether
// that damage is fatal). Requesting the state is the only sound way in, since the teleport and the damage
// live in later states driven by the state machine's own timer. The drift guard is the caller's
// player_is_burrowing() check; the range check here only covers a caller that skipped it. Game-thread only.
bool request_deep_water_fall(void *player)
{
    if (player == nullptr)
        return false;
    const int cur = pal::at<int>(player, kPlayerStateCurOff);
    // Backstop for a caller that did not confirm the burrow state first: outside the dispatch table's range
    // this is not the state field on this build, so leave the player alone.
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

// Clamps the SaveSlot unlocked-train-lines bitfield to line_mask (bit N = destination line N travelable).
// The game auto-unlocks a line just by visiting its station (#98), so AP-only gating overwrites the field
// with the granted-ticket mask each frame. This is box-hiding UX only: the menu builder always shows lines
// 95/99 regardless, so the warp is refused by the OnNPCEvent destination gate. Game-thread only.
void enforce_train_destinations(std::uint32_t line_mask)
{
    void *slot = pal::active_save_slot();
    if (slot == nullptr)
        return;
    // Unlocked-lines bitfield is a byte at +0x1e0 (5 low bits); the footfall unlock only ORs bits in, so
    // writing the granted mask each frame clears any line the game auto-unlocked on a station visit.
    pal::at<std::uint8_t>(slot, kSaveTrainUnlockedLinesOff) = static_cast<std::uint8_t>(line_mask & 0xffu);
}

// Raises the station donation machine's progress counter to `seed`, never lowering it. The machine's
// completion goal is compiled in, so pre-paying part of it is what makes the donation cost less; the game
// also clamps how much the player can dial in to what is still owed, so this bounds the charge too (#162).
// seed 0 leaves the counter alone (vanilla cost). Game-thread only.
void seed_ticket_machine_progress(std::uint32_t seed)
{
    if (seed == 0)
        return;
    void *slot = pal::active_save_slot();
    if (slot == nullptr)
        return;
    // Raise only: a player already past the seed keeps their own progress, and re-running this each tick
    // must not undo a deposit in flight.
    auto *progress = &pal::at<std::uint32_t>(slot, kSaveTicketProgressOff);
    if (*progress < seed)
        *progress = seed;
}

} // namespace

namespace mth
{

AbilityHooks::AbilityHooks(std::function<bool(std::int64_t item_id)> is_granted) : is_granted_(std::move(is_granted))
{
    const bool installed = install_ability_hooks([this](Ability a) { return gate_.blocked(a, AbilityGate::GrantQuery{enforce_, is_granted_}); });
    if (!installed)
        pal::logf(pal::LogLevel::Warn, "AbilityHooks: no ability chokepoints resolved; gating disabled");

    // Track the burrow lifetime from the two commit sites the mod already hooks, so the boundary poll knows
    // which mode the player is in without reading the mode field (its offset drifts between builds) (#163).
    set_burrow_observers([this](bool deep) { burrow_.arm(deep); }, [this] { burrow_.disarm(); });
}

AbilityHooks::~AbilityHooks()
{
    remove_ability_hooks();
}

void AbilityHooks::set_randomized(Ability a, bool on)
{
    gate_.set_randomized(a, on);
}

void AbilityHooks::set_enforce(bool on)
{
    enforce_ = on;
}

void AbilityHooks::set_train_gate(bool rando_active, std::uint32_t line_mask)
{
    train_rando_active_ = rando_active;
    train_mask_ = line_mask;
    set_train_destination_gate(line_mask, rando_active); // published to the OnNPCEvent detour
}

void AbilityHooks::set_ticket_machine(bool is_ap_location, bool checked, std::uint32_t progress_seed)
{
    // A machine that is a live check keeps its prompt; one that is not, or has already been donated to,
    // loses it. Reporting the location collected instead would read as "the pass exists" to the conductor,
    // which activates it without the pass ever being granted.
    ticket_machine_suppress_ = !is_ap_location || checked;
    ticket_machine_seed_ = progress_seed;
}

void AbilityHooks::enforce_train_tick()
{
    // train_rando: boarding requires the generic Train Pass (#98), and each destination is gated on its AP
    // ticket - the +0x1e0 clamp undoes the footfall auto-unlock (box UX), while the OnNPCEvent detour (fed
    // by set_train_gate) refuses the warp for un-granted lines. Otherwise fall back to the whole-train
    // ability gate (console-driven Train ability), which hides the conductor while blocked.
    if (train_rando_active_)
    {
        enforce_train_boarding();
        enforce_train_destinations(train_mask_);
        // The donation machine hands out that same pass (#162): while it is a live check the seed makes it
        // affordable, and once it is not the gate stops it prompting at all.
        //
        // The two are deliberately gated differently. The seed is a DURABLE write, so it waits for the bound
        // AP save - seeding an unrelated one would leave a near-free train pass behind in vanilla play. The
        // disable is in-memory and dies with the room, and failing to apply it is the worse outcome of the
        // two: an interactible machine takes 10000 bones and (via the backstop) returns nothing. So it runs
        // unbound on purpose. Do not "even these up".
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
            // Otherwise slot_data's train_pass_cost is silently ignored and the machine keeps asking vanilla
            // price, with nothing in the log to say why.
            warned_seed_withheld_ = true;
            pal::logf(pal::LogLevel::Warn, "train: donation cost left at vanilla; the live save is not the bound AP slot (#162)");
        }
    }
    else
    {
        enforce_train_presence(gate_.blocked(Ability::Train, AbilityGate::GrantQuery{enforce_, is_granted_}));
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
    if (mod::pause_api_available() && mod::world_is_paused())
        return; // frozen frame: hold the confirmation rather than advancing it on a stale reading
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

    const AbilityGate::GrantQuery q{enforce_, is_granted_};
    switch (burrow_.observe(water == 1, gate_.blocked(Ability::Burrow, q), gate_.blocked(Ability::Swim, q)))
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
    ticket_machine_.on_world_destroy(); // the next room's machine should not stay live for a walk cadence
}

} // namespace mth
