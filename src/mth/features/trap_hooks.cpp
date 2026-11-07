#include "mth/features/trap_hooks.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>

#include "mod/mod_api.hpp"
#include "mth/core/data/cheat_mask.hpp"
#include "mth/core/data/game_state_ids.hpp"
#include "mth/core/data/trap_table.hpp"
#include "mth/core/death_broadcast_gate.hpp" // kMaxFixedUpdateHz, for the no-delta fallback
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"

namespace mth
{

namespace
{

// Written from the named FixedUpdate hook at the top of the frame and read by tick() after the
// original has run. Same thread today, and an atomic costs nothing for the guarantee.
std::atomic<float> g_frame_delta{0.0f};
std::atomic<bool> g_frame_delta_seen{false};

void on_fixed_update_delta(float elapsed)
{
    g_frame_delta.store(elapsed, std::memory_order_relaxed);
    g_frame_delta_seen.store(true, std::memory_order_relaxed);
}

// Only reached when the named hook never fires. FixedUpdate runs at 60 or 120 Hz with nothing to
// tell them apart, so assume the faster rate: a trap that overstays beats one cut in half.
constexpr float kFallbackDelta = 1.0f / static_cast<float>(kMaxFixedUpdateHz);

// The runtime cheat mirror (CheatManager) behind the modifier traps. It is reached through the mod
// API's g_cheatManager symbol rather than a save-manager offset, so nothing here is per-platform; the
// save-mask half of modifier control is in modifier_hooks.cpp, which reads the g_saveManager slot offsets
// from pal::layout.

// CheatManager layout (distinct from the SaveSlot mask in modifier_hooks.cpp). +0x01 is an "activated" bool that
// every read site checks before consulting the mask, and +0x20 holds a u64[4] covering indices
// 0..63, 64..127, 128..191, 192..255. CheatManager::ActivateSaveCheats writes that byte to 1 at the
// top of its body and is its only writer, so a set byte means the game has activated a save slot.
constexpr std::size_t kCheatMgrActivatedOff = 0x01;
constexpr std::size_t kCheatMgrMaskOff = 0x20;

void *g_cheat_mgr_sym = nullptr;
bool g_cheat_mgr_outcome_logged = false; // one-shot: the manager resolved and validated
bool g_cheat_mgr_miss_logged = false;    // one-shot: symbol not (yet) resolved

// A validated CheatManager, resolved once so a caller needing both the activated byte and the mask
// does not walk +0x20 twice. A null base means there is no usable manager.
struct CheatManagerView
{
    std::uint8_t *base = nullptr;
    std::uint64_t *mask = nullptr;
};

// A candidate CheatManager* is trusted only if it looks like a real, naturally aligned pointer, its
// "activated" byte reads as an actual bool, and the mask pointer it carries at +0x20 is itself a
// valid, aligned pointer.
// pal::pointer_looks_valid is a range test, not a mapping check (pal/pal_mem.hpp), so alignment and
// the layout's own field values catch a candidate that merely landed in the valid range.
std::uint64_t *cheat_mask_at(void *candidate)
{
    const auto addr = reinterpret_cast<std::uintptr_t>(candidate);
    if (!pal::pointer_looks_valid(candidate) || (addr & 7u) != 0)
        return nullptr;
    auto *base = reinterpret_cast<std::uint8_t *>(candidate);
    if (base[kCheatMgrActivatedOff] > 1)
        return nullptr;
    auto *mask = *reinterpret_cast<std::uint64_t **>(base + kCheatMgrMaskOff);
    const auto mask_addr = reinterpret_cast<std::uintptr_t>(mask);
    if (!pal::pointer_looks_valid(mask) || (mask_addr & 7u) != 0)
        return nullptr;
    return mask;
}

// Resolves g_cheatManager through the game's own GetSymAddr. Only a successful resolve is cached:
// a miss is retried on every call, because a caller asking this from its own constructor can race
// the App constructor body publishing the game's text range (mod::sym_addr fails closed until
// then), and caching that transient miss would make traps look permanently unavailable for the rest
// of the session. The "not yet available" log is still one-shot, so a slow race does not spam it.
void ensure_cheat_mgr_symbol()
{
    if (g_cheat_mgr_sym != nullptr)
        return;
    g_cheat_mgr_sym = mod::sym_addr("g_cheatManager");
    if (g_cheat_mgr_sym != nullptr)
    {
        pal::logf(pal::LogLevel::Info, "traps: g_cheatManager resolved");
        return;
    }
    if (!g_cheat_mgr_miss_logged)
    {
        g_cheat_mgr_miss_logged = true;
        pal::logf(pal::LogLevel::Warn, "traps: g_cheatManager NOT available (yet)");
    }
}

// GetSymAddr hands back the CheatManager itself, already dereferenced: the symbol table's entry for
// g_cheatManager loads the global rather than taking its address. The validation below still earns
// its place, since cheat_mask_at is all that stands between a resolve gone wrong and a write into
// arbitrary game memory. The outcome logs once, since a human reads that line in-game.
CheatManagerView cheat_manager()
{
    ensure_cheat_mgr_symbol();
    if (g_cheat_mgr_sym == nullptr)
        return {};
    std::uint64_t *mask = cheat_mask_at(g_cheat_mgr_sym);
    if (mask == nullptr)
        return {};
    if (!g_cheat_mgr_outcome_logged)
    {
        g_cheat_mgr_outcome_logged = true;
        pal::logf(pal::LogLevel::Info, "traps: g_cheatManager resolved (mask at +0x20 looked valid)");
    }
    return {reinterpret_cast<std::uint8_t *>(g_cheat_mgr_sym), mask};
}

// True once g_cheatManager resolves through the game's own GetSymAddr. It is not in the Linux
// symbol table, so resolve_game_symbol cannot find it. A miss is not cached: resolution can depend
// on the game's text range being published, which may postdate the first ask, so only a successful
// resolve sticks and a false here is worth asking again rather than treated as final.
bool runtime_modifiers_available()
{
    ensure_cheat_mgr_symbol();
    return g_cheat_mgr_sym != nullptr;
}

// True once the cheat manager resolves and its activated byte is set. The game sets that byte only
// in ActivateSaveCheats, so it doubles as "a save slot has been activated". The native cheat entries
// resolve the live save slot themselves and do not null-check it, so a caller has to clear this
// before reaching one. Session state, where runtime_modifiers_available above is a property of the
// build.
bool runtime_modifier_ready()
{
    const CheatManagerView mgr = cheat_manager();
    return mgr.base != nullptr && mgr.base[kCheatMgrActivatedOff] != 0;
}

// Set/clear a modifier bit in the runtime cheat mask at CheatManager+0x20 and nowhere else. The
// save mask is never touched, so nothing persists across a crash and the save's achievement byte is
// never burned. Gameplay re-reads that mask every frame, so the effect lands without a room change.
// Game-thread only. False means "not right now" (no cheat manager bound, or the activated byte is
// still clear), which the caller should retry rather than treat as a permanent failure.
bool set_runtime_modifier(int idx, bool on)
{
    if (idx < 0 || idx >= 254)
        return false;
    const CheatManagerView mgr = cheat_manager();
    if (mgr.base == nullptr)
        return false;
    // Every read site checks this byte before the mask, so a bit written while it is clear does
    // nothing at all. Only the game's own ActivateSaveCheats sets it, so a clear byte means no save
    // is active yet, and the write is worth attempting again only after the next activation.
    if (mgr.base[kCheatMgrActivatedOff] == 0)
        return false;
    cheat_mask_set(mgr.mask, idx, on);
    return true;
}

} // namespace

TrapHooks::TrapHooks()
{
    mod::set_fixed_update_delta_hook(&on_fixed_update_delta);
}

TrapHooks::~TrapHooks()
{
    clear();
}

TrapArm TrapHooks::arm(int modifier_index, float seconds_override)
{
    const TrapDef *def = trap_for_modifier(modifier_index);
    if (def == nullptr)
    {
        pal::logf(pal::LogLevel::Warn, "traps: modifier %d is not a trap; nothing to arm", modifier_index);
        return TrapArm::Unavailable;
    }
    if (!runtime_modifiers_available())
    {
        pal::logf(pal::LogLevel::Warn, "traps: \"%s\" (idx=%d) cannot be armed (g_cheatManager unavailable on this build); consumed", def->label,
                  modifier_index);
        return TrapArm::Unavailable;
    }
    if (!is_gameplay_game_state(mod::current_game_state()))
    {
        // runtime_modifier_ready() below is a one-way latch (the game never clears it once a save has
        // been activated), so it cannot tell the title screen from an active run. The gamestate range
        // can, and RoomTracker already keys the same distinction on it.
        if (notready_warned_.insert(modifier_index).second)
            pal::logf(pal::LogLevel::Warn, "traps: \"%s\" (idx=%d) deferred until the player is in a room; retrying", def->label, modifier_index);
        return TrapArm::NotReady;
    }
    if (!runtime_modifier_ready())
    {
        // The native read below resolves the live save slot itself and does not null-check it, so
        // asking before a save is activated would fault. The connection binds at the title screen,
        // so a trap really can arrive with no save loaded.
        if (notready_warned_.insert(modifier_index).second)
            pal::logf(pal::LogLevel::Warn, "traps: \"%s\" (idx=%d) deferred until a save is active; retrying", def->label, modifier_index);
        return TrapArm::NotReady;
    }
    if (mod::cheat_manager_is_cheat_applied(modifier_index))
    {
        // Arming over a modifier the player chose would be invisible for the duration and would then
        // "expire" by taking their setting away, with ActivateSaveCheats fighting to restore it.
        pal::logf(pal::LogLevel::Info, "traps: \"%s\" (idx=%d) already on by the player's own choice; consumed without arming", def->label, modifier_index);
        return TrapArm::Skipped;
    }
    if (!set_runtime_modifier(modifier_index, true))
    {
        // Logged once per stall rather than every retry tick: the granter calls arm() again next
        // tick regardless, so a warning here would repeat until the write finally goes through.
        // Traps can stall on distinct indices at once, so this is a set rather than one scalar;
        // insert() only warns the first time a given index joins the stalled set.
        if (notready_warned_.insert(modifier_index).second)
            pal::logf(pal::LogLevel::Warn, "traps: \"%s\" (idx=%d) write refused this frame; retrying", def->label, modifier_index);
        return TrapArm::NotReady;
    }
    notready_warned_.erase(modifier_index); // the write went through, so a future stall on this index warns again
    // A prior lift for this same index may still be sitting in the retry list; a fresh arm wins over
    // it; otherwise the next tick's retry would turn this brand-new trap straight back off.
    std::erase(pending_off_, modifier_index);
    const float seconds = seconds_override > 0.0f ? seconds_override : def->seconds;
    state_.arm(modifier_index, seconds);
    pal::logf(pal::LogLevel::Info, "traps: \"%s\" (idx=%d) armed for %.1fs", def->label, modifier_index, static_cast<double>(seconds));
    return TrapArm::Armed;
}

void TrapHooks::queue_arm(int modifier_index, float seconds_override)
{
    std::lock_guard<std::mutex> lk(mtx_);
    pending_arm_.emplace_back(modifier_index, seconds_override);
}

void TrapHooks::tick()
{
    // Drained unconditionally: arm() itself refuses outside gameplay, so a queued console trap just
    // comes back NotReady rather than piling up here.
    drain_pending_arm();
    // Retried before the gameplay check below: a lift the game refused earlier has to keep trying to
    // land even while the player is at a menu, or the modifier stays forced on indefinitely.
    retry_pending_off();
    if (!is_gameplay_game_state(mod::current_game_state()))
    {
        // Leaving gameplay ends every running trap outright rather than freezing its countdown: the
        // room clock that ages it has stalled here too, so a suspend-and-resume would just leave the
        // bit stuck on with nothing left to lift it. Guarded on non-empty so an idle menu does not
        // call clear() every frame: clear() also resets notready_warned_, and doing that every tick
        // would defeat its dedup and make arm()'s "deferred until the player is in a room" warning
        // (logged right after this by grants_->tick()) spam once a frame instead of once per stall.
        if (!state_.empty())
            clear();
        publish_active();
        return;
    }
    advance_frame();
    publish_active();
}

void TrapHooks::drain_pending_arm()
{
    std::vector<std::pair<int, float>> batch;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        batch.swap(pending_arm_);
    }
    for (auto [idx, seconds] : batch)
        arm(idx, seconds);
}

void TrapHooks::publish_active()
{
    std::lock_guard<std::mutex> lk(mtx_);
    snapshot_ = state_.active_with_remaining();
}

void TrapHooks::advance_frame()
{
    // tick() already ran retry_pending_off() for this frame before reaching here.
    if (state_.empty())
        return;
    const float dt = gameplay_advanced() ? take_delta() : 0.0f;
    for (int idx : state_.advance(dt))
    {
        const TrapDef *def = trap_for_modifier(idx);
        if (set_runtime_modifier(idx, false))
        {
            pal::logf(pal::LogLevel::Info, "traps: \"%s\" (idx=%d) expired", def != nullptr ? def->label : "?", idx);
        }
        else
        {
            pending_off_.push_back(idx);
            pal::logf(pal::LogLevel::Warn, "traps: \"%s\" (idx=%d) expired but the lift failed; still forced on, retrying", def != nullptr ? def->label : "?",
                      idx);
        }
    }
    if (dt <= 0.0f)
        return;
    // ActivateSaveCheats rebuilds the runtime mask from the save, so a load, a profile select, or
    // the player opening the options menu would otherwise cancel a trap that is still running.
    for (int idx : state_.active())
        set_runtime_modifier(idx, true);
}

void TrapHooks::clear()
{
    notready_warned_.clear(); // a stall carried into the next seed would otherwise never warn again
    for (int idx : state_.clear())
    {
        if (set_runtime_modifier(idx, false))
            continue;
        pending_off_.push_back(idx);
        const TrapDef *def = trap_for_modifier(idx);
        pal::logf(pal::LogLevel::Warn, "traps: \"%s\" (idx=%d) lift failed on clear; still forced on, retrying", def != nullptr ? def->label : "?", idx);
    }
}

// Retries every lift the game refused earlier. Run once at the top of every tick rather than
// looped or blocked on here: set_runtime_modifier's false means "not right now", and the next
// tick is the next chance to ask again.
void TrapHooks::retry_pending_off()
{
    if (pending_off_.empty() || !runtime_modifier_ready())
        return; // the cheat-applied read below faults without an activated save slot
    std::erase_if(pending_off_,
                  [](int idx)
                  {
                      const TrapDef *def = trap_for_modifier(idx);
                      if (mod::cheat_manager_is_cheat_applied(idx))
                      {
                          // The save carries that bit now, so between the failed lift and this retry
                          // the player turned the modifier on themselves; clearing it would take
                          // their own setting away.
                          pal::logf(pal::LogLevel::Info, "traps: \"%s\" (idx=%d) is the player's own choice now; lift retry dropped",
                                    def != nullptr ? def->label : "?", idx);
                          return true;
                      }
                      if (!set_runtime_modifier(idx, false))
                          return false;
                      pal::logf(pal::LogLevel::Info, "traps: \"%s\" (idx=%d) lift retry succeeded", def != nullptr ? def->label : "?", idx);
                      return true;
                  });
}

// Whether the world's gameplay queues ran this frame. A trap must not burn its duration while the
// player sits in a menu, and must not lose time to a room transition.
bool TrapHooks::gameplay_advanced()
{
    if (mod::pause_api_available() && mod::world_is_paused())
        return false;
    if (!mod::room_time_api_available())
        return true;
    // Only whether the clock moved matters, not by how much, so sampling it while no trap is running
    // would buy nothing; the first tick after arming compares against a stale reading and can lose
    // that one frame.
    const float now = mod::room_time();
    const bool moved = now != last_room_time_;
    last_room_time_ = now;
    return moved;
}

float TrapHooks::take_delta()
{
    const bool have = g_frame_delta_seen.load(std::memory_order_relaxed);
    if (!timing_source_logged_)
    {
        timing_source_logged_ = true;
        if (have)
            pal::logf(pal::LogLevel::Info, "traps: timing from the FixedUpdate frame delta");
        else
            pal::logf(pal::LogLevel::Warn, "traps: no FixedUpdate delta on this build; timing at an assumed %d Hz", kMaxFixedUpdateHz);
    }
    return have ? g_frame_delta.load(std::memory_order_relaxed) : kFallbackDelta;
}

std::vector<std::string> TrapHooks::status_lines() const
{
    std::vector<ActiveTrap> running;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        running = snapshot_;
    }
    std::vector<std::string> out;
    if (running.empty())
    {
        out.push_back("traps: none active");
        return out;
    }
    for (const ActiveTrap &t : running)
    {
        const TrapDef *def = trap_for_modifier(t.index);
        char secs[16];
        std::snprintf(secs, sizeof(secs), "%.1f", static_cast<double>(t.remaining));
        out.push_back(std::string("trap active: ") + (def != nullptr ? def->label : "?") + " (idx " + std::to_string(t.index) + ", " + secs + "s left)");
    }
    return out;
}

} // namespace mth
