#include "mth/features/modifier_hooks.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

#include "mod/mod_api.hpp"
#include "mth/core/data/game_symbols.hpp"
#include "mth/core/data/modifier_table.hpp"
#include "pal/pal_game.hpp"
#include "pal/pal_layout.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"
#include "pal/pal_module.hpp"
#include "pal/scoped_hook.hpp"

// The game-side modifier ("cheat") control. Windows has a single save slot behind g_saveManager where
// Linux has an apply-path slot and a live slot; pal::layout gives Windows the same offset for both, so
// the paths below that handle two slots collapse to one there.
namespace
{

constexpr std::ptrdiff_t kCheatMaskOff = 0xcb0; // SaveSlot: 8 u32 words, the per-save enable bitmask

std::uintptr_t g_addr_activate_slot = 0;
std::uintptr_t g_addr_toggle = 0;
std::uintptr_t g_addr_set_applied = 0;
bool g_mod_resolved = false;
bool g_mod_ok = false;

std::function<void(int slot_index, std::uint32_t *words)> g_seed_fn;
std::function<void()> g_save_loaded_fn;
std::function<bool(int idx)> g_block_fn;

[[clang::no_destroy]] pal::ScopedHook g_activate_slot_hook;
void (*g_orig_activate_slot)(void *, bool) = nullptr;
[[clang::no_destroy]] pal::ScopedHook g_toggle_hook;
void (*g_orig_toggle)(void *, int, bool, void *, bool, int) = nullptr;
[[clang::no_destroy]] pal::ScopedHook g_set_applied_hook;
void (*g_orig_set_applied)(void *, int, bool, void *) = nullptr;

void *mod_slot(std::ptrdiff_t off)
{
    const std::uintptr_t mgr = pal::save_manager_global();
    if (mgr == 0)
        return nullptr;
    return pal::at<void *>(mgr, off);
}

void set_mask_bit(void *slot, int idx, bool on)
{
    if (!pal::pointer_looks_valid(slot))
        return;
    auto *mask = &pal::at<std::uint32_t>(slot, kCheatMaskOff);
    const std::uint32_t bit = 1u << (static_cast<unsigned>(idx) & 31u);
    if (on)
        mask[idx >> 5] |= bit;
    else
        mask[idx >> 5] &= ~bit;
}

void repl_activate_slot(void *self, bool flag)
{
    void *aslot = mod_slot(pal::layout::kSaveManagerApplySlotOff);
    void *lslot = mod_slot(pal::layout::kSaveManagerLiveSlotOff);
    const std::uintptr_t mgr = pal::save_manager_global();
    const int slot_index = (mgr != 0) ? pal::at<int>(mgr, pal::layout::kSaveManagerSlotIndexOff) : -1;
    pal::logf(pal::LogLevel::Debug, "modifiers: ActivateSaveSlot flag=%d slot_index=%d apply=%p live=%p", static_cast<int>(flag), slot_index, aslot, lslot);
    // flag is true only on a real load; title/profile-menu re-activations pass false.
    if (g_seed_fn && flag)
    {
        void *slots[2] = {aslot, (lslot != aslot) ? lslot : nullptr};
        for (void *slot : slots)
        {
            if (!pal::pointer_looks_valid(slot))
                continue;
            auto *mask = &pal::at<std::uint32_t>(slot, kCheatMaskOff);
            std::uint32_t words[8];
            for (int i = 0; i < 8; ++i)
                words[i] = mask[i];
            g_seed_fn(slot_index, words);
            for (int i = 0; i < 8; ++i)
                mask[i] = words[i];
            // Diagnostic (#46): warp_home=121 -> word3 bit25, landing(ossex)=128 -> word4 bit0. Confirms
            // the force-on baseline landed in the mask (if inert in-game, it was seeded but not re-applied).
            pal::logf(pal::LogLevel::Debug, "modifiers: seeded cheat mask on slot=%p (slot_index=%d) warp_home[121]=%d landing[128]=%d", slot, slot_index,
                      (mask[3] >> 25) & 1, mask[4] & 1);
        }
    }
    if (g_orig_activate_slot)
        g_orig_activate_slot(self, flag);
    // Post-original: the slot's contents are the run's by now. flag=false is a title/profile-menu
    // re-activation and must not fire, or the notify lands before the save data does.
    if (flag && g_save_loaded_fn)
        g_save_loaded_fn();
}

// ToggleCheat (menu toggle) writes the runtime mirror and persists; blocking it keeps a gameplay modifier
// off. The checkbox flips transiently but re-syncs on cursor-over. idx = 2nd arg (edx).
void repl_toggle(void *self, int idx, bool enable, void *slot, bool b, int i)
{
    const bool blocked = g_block_fn && g_block_fn(idx);
    pal::logf(pal::LogLevel::Debug, "modifiers: ToggleCheat idx=%d enable=%d -> %s", idx, static_cast<int>(enable), blocked ? "BLOCKED" : "allowed");
    if (blocked)
        return;
    if (g_orig_toggle)
        g_orig_toggle(self, idx, enable, slot, b, i);
}

// SetCheatApplied: the persist writer the typed cheat-code path reaches directly (the menu goes
// through ToggleCheat above). idx = 2nd arg.
void repl_set_applied(void *self, int idx, bool applied, void *slot)
{
    if (g_block_fn && g_block_fn(idx))
    {
        pal::logf(pal::LogLevel::Debug, "modifiers: blocked SetCheatApplied idx=%d (locked)", idx);
        return;
    }
    if (g_orig_set_applied)
        g_orig_set_applied(self, idx, applied, slot);
}

// True once every modifier symbol (ActivateSaveSlot, ToggleCheat, SetCheatApplied, g_saveManager)
// resolves. Resolves and caches on the first call.
bool modifiers_available()
{
    if (g_mod_resolved)
        return g_mod_ok;
    g_mod_resolved = true;
    const std::uintptr_t mgr = pal::save_manager_global();
    g_addr_activate_slot = pal::resolve_game_symbol(mth::sym::activate_save_slot);
    g_addr_toggle = pal::resolve_game_symbol(mth::sym::toggle_cheat);
    g_addr_set_applied = pal::resolve_game_symbol(mth::sym::set_cheat_applied);
    g_mod_ok = mgr != 0 && g_addr_activate_slot != 0 && g_addr_toggle != 0 && g_addr_set_applied != 0;
    if (!g_mod_ok)
        pal::logf(pal::LogLevel::Warn, "modifiers: symbols unresolved (mgr=0x%llx slot=0x%llx toggle=0x%llx set=0x%llx); feature disabled",
                  static_cast<unsigned long long>(mgr), static_cast<unsigned long long>(g_addr_activate_slot), static_cast<unsigned long long>(g_addr_toggle),
                  static_cast<unsigned long long>(g_addr_set_applied));
    return g_mod_ok;
}

// Installs the ActivateSaveSlot hook. `seed` runs on the game thread before the original, with the
// active 0-based save-slot index and the slot's 8 cheat-mask words (in/out).
void set_new_game_modifier_seed(std::function<void(int slot_index, std::uint32_t *words)> seed)
{
    if (!modifiers_available())
        return;
    g_seed_fn = std::move(seed);
    g_activate_slot_hook = pal::ScopedHook(g_addr_activate_slot, &repl_activate_slot, &g_orig_activate_slot);
    if (!g_activate_slot_hook.installed())
        pal::logf(pal::LogLevel::Error, "modifiers: ActivateSaveSlot seed hook FAILED (new-game seeding disabled)");
    else
        pal::logf(pal::LogLevel::Info, "modifiers: seed hooks installed");
}

// Installs the ToggleCheat (options menu: mirror and persist) and SetCheatApplied (typed cheat code)
// lockdown hooks. `block(idx)` returns true to suppress that player change.
void set_modifier_lockdown(std::function<bool(int idx)> block)
{
    if (!modifiers_available())
        return;
    g_block_fn = std::move(block);
    g_toggle_hook = pal::ScopedHook(g_addr_toggle, &repl_toggle, &g_orig_toggle);
    g_set_applied_hook = pal::ScopedHook(g_addr_set_applied, &repl_set_applied, &g_orig_set_applied);
    if (!g_toggle_hook.installed())
        pal::logf(pal::LogLevel::Error, "modifiers: ToggleCheat hook FAILED (menu lockdown disabled)");
    if (!g_set_applied_hook.installed())
        pal::logf(pal::LogLevel::Error, "modifiers: SetCheatApplied hook FAILED (cheat-code lockdown disabled)");
    if (g_toggle_hook.installed() && g_set_applied_hook.installed())
        pal::logf(pal::LogLevel::Info, "modifiers: lockdown hooks installed");
}

// Sets or clears a modifier's enable bit on the live slot(s) and rebuilds the runtime mirror so the
// effect is live. Game-thread only (calls ActivateSaveCheats). False if unavailable or idx invalid.
bool apply_live_modifier(int idx, bool on)
{
    if (!modifiers_available() || idx < 0 || idx >= 254)
        return false;
    void *aslot = mod_slot(pal::layout::kSaveManagerApplySlotOff);
    void *lslot = mod_slot(pal::layout::kSaveManagerLiveSlotOff);
    if (!pal::pointer_looks_valid(aslot) && !pal::pointer_looks_valid(lslot))
    {
        pal::logf(pal::LogLevel::Warn, "modifiers: live set idx=%d failed (no valid save slot active)", idx);
        return false;
    }
    // The native entry covers the live slot only (it resolves the live slot itself, and stamps the extra
    // save-file byte the four special indices need) and does NOT null-check it, so it is only safe once
    // that slot is known good. The apply-path slot has no native path either way and keeps the raw write;
    // a pre-appended-API build falls back to the raw write for both.
    const bool native = pal::pointer_looks_valid(lslot) && mod::cheat_manager_set_cheat_applied(idx, on);
    if (!native)
        set_mask_bit(lslot, idx, on);
    if (aslot != lslot)
        set_mask_bit(aslot, idx, on);
    if (!pal::pointer_looks_valid(aslot) || !pal::pointer_looks_valid(lslot))
        pal::logf(pal::LogLevel::Warn, "modifiers: live set idx=%d partial (apply=%p live=%p)", idx, aslot, lslot);
    // ActivateSaveCheats reads the apply-path slot internally, so only rebuild when it is valid.
    if (!pal::pointer_looks_valid(aslot) || !mod::cheat_manager_activate_save_cheats())
        pal::logf(pal::LogLevel::Warn, "modifiers: live set idx=%d bit written but mirror NOT rebuilt (apply slot/native entry unavailable)", idx);
    pal::logf(pal::LogLevel::Info, "modifiers: live set idx=%d on=%d apply=%p live=%p", idx, static_cast<int>(on), aslot, lslot);
    return true;
}

void remove_modifier_hooks()
{
    for (pal::ScopedHook *hook : {&g_activate_slot_hook, &g_toggle_hook, &g_set_applied_hook})
        hook->reset();
    g_seed_fn = nullptr;
    g_save_loaded_fn = nullptr;
    g_block_fn = nullptr;
}

} // namespace

namespace mth
{

void set_save_loaded(std::function<void()> cb)
{
    g_save_loaded_fn = std::move(cb);
}

ModifierHooks::ModifierHooks(ModifierRequest request)
{
    forced_ = std::move(request.forced);
    for (int idx : request.indices)
    {
        if (is_safe(idx) || forced_.count(idx) != 0)
            enforced_.insert(idx);
        else
            pal::logf(pal::LogLevel::Warn, "modifiers: idx=%d is not live-settable (%s); ignored (prefix force: to override)", idx,
                      class_of(idx) == CheatClass::Grant        ? "grant"
                      : class_of(idx) == CheatClass::Combo      ? "combo"
                      : class_of(idx) == CheatClass::Randomizer ? "randomizer"
                                                                : "invalid");
    }
    if (!modifiers_available())
    {
        pal::logf(pal::LogLevel::Warn, "modifiers: PAL unavailable; ModifierHooks inert");
        return;
    }
    set_new_game_modifier_seed([this](int slot, std::uint32_t *w) { seed(slot, w); });
    set_modifier_lockdown([this](int idx) { return block(idx); });
    installed_ = true;
    pal::logf(pal::LogLevel::Info, "modifiers: ModifierHooks installed (enforced=%zu armed=%d)", enforced_.size(), static_cast<int>(armed_));
}

ModifierHooks::~ModifierHooks()
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        armed_ = false; // disarm first so any in-flight seed/block bails before the hooks are removed
    }
    if (installed_)
        remove_modifier_hooks(); // synchronous removal, same teardown contract as DeathHooks/LocationHooks
    std::lock_guard<std::mutex> lk(mtx_);
    enforced_.clear();
    pending_live_.clear();
}

void ModifierHooks::seed(int slot_index, std::uint32_t words[8])
{
    if (!enforce_live_.load())
        return; // not an AP session (or test mode): never touch a save the mod doesn't own
    std::lock_guard<std::mutex> lk(mtx_);
    const bool scoped = ap_scoped_.load();
    if (scoped)
    {
        // AP session: enforce only the AP game's slot (captured on the first load), never a vanilla one.
        if (ap_slot_ >= 0 && slot_index != ap_slot_)
        {
            pal::logf(pal::LogLevel::Info, "modifiers: load slot_index=%d != AP slot %d; not enforcing", slot_index, ap_slot_);
            return;
        }
        if (ap_slot_ < 0 && slot_index >= 0)
        {
            ap_slot_ = slot_index;
            pal::logf(pal::LogLevel::Info, "modifiers: captured AP-game slot index=%d", ap_slot_);
        }
    }
    if (armed_)
    {
        // Runs on every real load, not only on a new file, so a denied modifier that was set
        // before the save joined the AP session gets cleared again here. Anything the player is
        // allowed to pick survives the reload.
        for (int idx = 0; idx < kCheatCount; ++idx)
            if (is_ap_denied(idx))
                words[idx >> 5] &= ~(1u << (static_cast<unsigned>(idx) & 31u));
        // After the clear, so a force: override still wins over the deny list.
        for (int idx : enforced_)
            words[idx >> 5] |= 1u << (static_cast<unsigned>(idx) & 31u);
    }
    if (scoped) // additive: force the AP baseline on without disturbing the player's other modifiers
        for (int idx : force_on_)
            words[idx >> 5] |= 1u << (static_cast<unsigned>(idx) & 31u);
}

bool ModifierHooks::block(int idx) const
{
    if (!enforce_live_.load())
        return false; // vanilla play (not connected): leave the player's cheat menu alone
    std::lock_guard<std::mutex> lk(mtx_);
    if (ap_scoped_.load() && force_on_.count(idx) != 0)
        return true; // a force-on baseline modifier can't be toggled off
    return armed_ && is_ap_denied(idx);
}

void ModifierHooks::set_enforce_live(bool on)
{
    enforce_live_.store(on);
}

void ModifierHooks::set_ap_scoped(bool on)
{
    ap_scoped_.store(on);
}

void ModifierHooks::set_ossex_start(bool on)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (on)
        force_on_.insert(kCheatLandingDone);
    else
        force_on_.erase(kCheatLandingDone);
}

void ModifierHooks::set_cheaper_boneups(bool on)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (on)
        force_on_.insert(kCheatCheaperBoneUp);
    else
        force_on_.erase(kCheatCheaperBoneUp);
}

void ModifierHooks::set_ap_slot(int slot)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (slot >= 0)
        ap_slot_ = slot; // a recorded slot from a prior session; skip capture
}

int ModifierHooks::captured_ap_slot() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return ap_slot_;
}

void ModifierHooks::set_armed(bool on)
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        armed_ = on;
    }
    pal::logf(pal::LogLevel::Info, "modifiers: %s", on ? "armed (locked)" : "disarmed (unlocked)");
}

void ModifierHooks::set_live(int idx, bool on)
{
    if (!is_safe(idx)) // only continuous modifiers are reversible/live-settable
    {
        pal::logf(pal::LogLevel::Warn, "modifiers: live set idx=%d refused (not a continuous modifier)", idx);
        return;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    pending_live_.emplace_back(idx, on);
}

void ModifierHooks::set_enforced(ModifierRequest request)
{
    const std::size_t requested = request.indices.size();
    std::size_t kept = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        forced_ = std::move(request.forced);
        enforced_.clear();
        for (int idx : request.indices)
            if (is_safe(idx) || forced_.count(idx) != 0)
                enforced_.insert(idx);
        kept = enforced_.size();
    }
    if (kept != requested)
        pal::logf(pal::LogLevel::Warn, "modifiers: set_enforced kept %zu of %zu requested (unsafe/duplicate indices dropped)", kept, requested);
    pal::logf(pal::LogLevel::Info, "modifiers: enforced set replaced (n=%zu)", kept);
}

void ModifierHooks::drain_live()
{
    std::vector<std::pair<int, bool>> batch;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        batch.swap(pending_live_);
    }
    for (auto [idx, on] : batch)
        apply_live_modifier(idx, on);
}

std::vector<std::string> ModifierHooks::status_lines() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> out;
    out.push_back(std::string("modifiers armed: ") + (armed_ ? "yes" : "no"));
    std::string list;
    for (int idx : enforced_)
        list += std::to_string(idx) + " ";
    out.push_back("enforced: " + (list.empty() ? std::string("(none)") : list));
    return out;
}

} // namespace mth
