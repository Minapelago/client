#include "mth/features/trap_hooks.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>

#include "mod/mod_api.hpp"
#include "mth/core/game/game_state_ids.hpp"
#include "mth/core/game/modifiers.hpp"
#include "mth/core/rules/death_broadcast_gate.hpp" // kMaxFixedUpdateHz, for the no-delta fallback
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"

namespace mth
{

namespace
{

// Written by the named FixedUpdate hook at the top of the frame, read by tick() after the original runs. Same thread today; the atomic is free.
std::atomic<float> g_frame_delta{0.0f};
std::atomic<bool> g_frame_delta_seen{false};

void on_fixed_update_delta(float elapsed)
{
    g_frame_delta.store(elapsed, std::memory_order_relaxed);
    g_frame_delta_seen.store(true, std::memory_order_relaxed);
}

// Used only when the named hook never fires. FixedUpdate runs at 60 or 120 Hz with nothing to tell them apart, so assume the faster rate: a trap that overstays
// beats one cut in half.
constexpr float kFallbackDelta = 1.0f / static_cast<float>(kMaxFixedUpdateHz);

// The runtime cheat mirror (CheatManager) behind the traps, reached through the mod API's g_cheatManager symbol, so nothing here is per-platform. The save-mask
// half lives in modifier_hooks.cpp.

// CheatManager layout (not the SaveSlot mask). +0x01 is an "activated" bool every read site checks before the mask; +0x20 holds a u64[4] covering indices
// 0..255. ActivateSaveCheats is the byte's only writer, setting it to 1 at the top of its body, so a set byte means a save slot has been activated.
constexpr std::size_t kCheatMgrActivatedOff = 0x01;
constexpr std::size_t kCheatMgrMaskOff = 0x20;

void *g_cheat_mgr_sym = nullptr;
bool g_cheat_mgr_outcome_logged = false; // one-shot: the manager resolved and validated
bool g_cheat_mgr_miss_logged = false;    // one-shot: symbol not (yet) resolved

// A validated CheatManager, resolved once so a caller needing both the activated byte and the mask walks +0x20 once. A null base means no usable manager.
struct CheatManagerView
{
    std::uint8_t *base = nullptr;
    std::uint64_t *mask = nullptr;
};

// A candidate CheatManager* is trusted only if it is aligned, its activated byte reads as a bool, and its +0x20 mask pointer is valid and aligned.
// pal::pointer_looks_valid is a range test, not a mapping check, so alignment and field values catch a candidate that merely lands in range.
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

// Resolves g_cheatManager through the game's GetSymAddr, caching only a hit. A miss retries every call: a caller in its own constructor can race App publishing
// the game's text range (mod::sym_addr fails closed until then). The miss log is one-shot.
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

// GetSymAddr's entry for g_cheatManager loads the global, so it returns the CheatManager itself. cheat_mask_at still validates it, since it is all that stands
// between a bad resolve and a write into arbitrary game memory. The outcome logs once.
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

// True once g_cheatManager resolves through GetSymAddr (it is not in the Linux symbol table, so resolve_game_symbol cannot find it). A false is not final:
// resolution can wait on the game's text range being published.
bool runtime_modifiers_available()
{
    ensure_cheat_mgr_symbol();
    return g_cheat_mgr_sym != nullptr;
}

// True once the cheat manager resolves and its activated byte is set, i.e. a save slot has been activated (only ActivateSaveCheats sets it). The native cheat
// entries resolve the live save slot without a null check, so callers must pass this first. Session state, where runtime_modifiers_available is a build
// property.
bool runtime_modifier_ready()
{
    const CheatManagerView mgr = cheat_manager();
    return mgr.base != nullptr && mgr.base[kCheatMgrActivatedOff] != 0;
}

// Sets or clears a modifier bit in the runtime cheat mask at CheatManager+0x20 only, so nothing persists across a crash and the save's achievement byte is
// never burned. Gameplay re-reads the mask every frame, so the effect lands without a room change. Game thread only. false means "not right now" (no manager,
// or the activated byte is still clear); retry.
bool set_runtime_modifier(int idx, bool on)
{
    if (idx < 0 || idx >= 254)
        return false;
    const CheatManagerView mgr = cheat_manager();
    if (mgr.base == nullptr)
        return false;
    // Every read site checks this byte before the mask, so a bit written while it is clear does nothing; retry after the next activation.
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
        // runtime_modifier_ready() is a one-way latch that cannot tell the title screen from a run; the gamestate range can (room_tracker uses it too).
        if (notready_warned_.insert(modifier_index).second)
            pal::logf(pal::LogLevel::Warn, "traps: \"%s\" (idx=%d) deferred until the player is in a room; retrying", def->label, modifier_index);
        return TrapArm::NotReady;
    }
    if (!runtime_modifier_ready())
    {
        // The native read below resolves the live save slot without a null check and would fault before a save is active. The connection binds at the title
        // screen, so a trap can arrive with no save loaded.
        if (notready_warned_.insert(modifier_index).second)
            pal::logf(pal::LogLevel::Warn, "traps: \"%s\" (idx=%d) deferred until a save is active; retrying", def->label, modifier_index);
        return TrapArm::NotReady;
    }
    if (mod::cheat_manager_is_cheat_applied(modifier_index))
    {
        // Arming over a modifier the player chose would be invisible, then "expire" by removing their setting while ActivateSaveCheats fights to restore it.
        pal::logf(pal::LogLevel::Info, "traps: \"%s\" (idx=%d) already on by the player's own choice; consumed without arming", def->label, modifier_index);
        return TrapArm::Skipped;
    }
    if (!set_runtime_modifier(modifier_index, true))
    {
        // Warned once per stall per index, since the granter calls arm() again every tick until the write lands.
        if (notready_warned_.insert(modifier_index).second)
            pal::logf(pal::LogLevel::Warn, "traps: \"%s\" (idx=%d) write refused this frame; retrying", def->label, modifier_index);
        return TrapArm::NotReady;
    }
    notready_warned_.erase(modifier_index); // the write went through, so a future stall on this index warns again
    // A fresh arm cancels a pending lift for the same index, or the next retry would turn this new trap straight off.
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
    // Drained unconditionally: arm() refuses outside gameplay, so a queued console trap comes back NotReady.
    drain_pending_arm();
    // Retried before the gameplay check so a refused lift keeps trying at a menu, or the modifier stays forced on.
    retry_pending_off();
    if (!is_gameplay_game_state(mod::current_game_state()))
    {
        // Leaving gameplay ends every running trap, since the room clock that ages it stalls here too and a frozen trap could be stranded on. Guarded on
        // non-empty because clear() resets notready_warned_, and doing that every idle-menu frame would make arm()'s "deferred until the player is in a room"
        // warning repeat each frame.
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
    snapshot_ = state_.traps();
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
    // ActivateSaveCheats rebuilds the runtime mask from the save, so a load, profile select or the options menu would cancel a running trap; re-assert them.
    for (const ActiveTrap &t : state_.traps())
        set_runtime_modifier(t.index, true);
}

void TrapHooks::clear()
{
    notready_warned_.clear(); // a stall carried into the next seed would never warn again
    for (int idx : state_.clear())
    {
        if (set_runtime_modifier(idx, false))
            continue;
        pending_off_.push_back(idx);
        const TrapDef *def = trap_for_modifier(idx);
        pal::logf(pal::LogLevel::Warn, "traps: \"%s\" (idx=%d) lift failed on clear; still forced on, retrying", def != nullptr ? def->label : "?", idx);
    }
}

// Retries every lift the game refused earlier, once per tick: false from set_runtime_modifier means "not right now".
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
                          // The save carries the bit, so the player turned the modifier on since the failed lift; clearing it would take their setting away.
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

// Whether the world's gameplay queues ran this frame, so a trap burns no time in a menu or a room transition.
bool TrapHooks::gameplay_advanced()
{
    // Sampled only while a trap runs, since only movement matters; the first tick after arming compares against a stale reading and can lose that frame.
    return room_clock_.sample() == RoomClock::Tick::Advanced;
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
