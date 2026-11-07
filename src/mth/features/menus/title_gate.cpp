#include "mth/features/menus/title_gate.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

#include "mod/mod_api.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/core/game/game_symbols.hpp"
#include "mth/core/game/title_menu.hpp"
#include "pal/pal_layout.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"
#include "pal/pal_module.hpp"
#include "pal/scoped_hook.hpp"

namespace
{

// TitleScreen::UpdateState(): while disconnected, keep the menu cursor off index 0 ("Start Game")
// so the wrap behaves as a two-option menu. The game has no disabled-option concept to set.
std::function<bool()> g_title_gate_fn; // true when AP is connected, i.e. when "Start Game" is selectable
[[clang::no_destroy]] pal::ScopedHook g_title_gate_hook;
void (*g_orig_title_update)(void *) = nullptr;

// Desired "Start Game" label, re-applied every UpdateState because the localization refresh re-sets it. Empty restores the cached original.
std::string g_title_start_text;
std::mutex g_title_start_text_mutex;

// The localized "Start Game" from first sight, so restoring it after a reconnect keeps the player's language.
std::string g_title_start_original;
bool g_title_start_original_warned = false;
// Last label we wrote while disconnected, so the restore path undoes only our own substitution and leaves a real change (e.g. language) alone.
std::string g_title_start_last_substituted;

// Re-applies the desired Start Game text (or the cached original once connected) only when the current text differs. A language switch while connected yields a
// string matching neither our cache nor our substitution, so it is left alone.
void apply_title_start_text(void *base)
{
    if (base == nullptr)
        return;

    void *block = pal::at<void *>(base, mth::layout::kTitleOptionBlockOff);
    if (!pal::pointer_looks_valid(block))
        return;
    void *widget = static_cast<char *>(block) + mth::layout::kTitleOptionStartGame * mth::layout::kTitleOptionStride;

    const char *cur = mod::text_of(widget);
    const bool cur_valid = pal::pointer_looks_valid(cur);
    const std::string current = cur_valid ? std::string(cur) : std::string();

    if (g_title_start_original.empty())
    {
        if (cur_valid)
            g_title_start_original = current;
        else if (!g_title_start_original_warned)
        {
            g_title_start_original_warned = true;
            pal::logf(pal::LogLevel::Warn, "title: could not read the Start Game option's original text; label will not be restored");
        }
    }

    std::string want;
    {
        std::lock_guard<std::mutex> lk(g_title_start_text_mutex);
        want = g_title_start_text;
    }

    if (!want.empty())
    {
        if (current != want && mod::set_text(widget, want.c_str()))
            g_title_start_last_substituted = want;
    }
    else if (!g_title_start_original.empty() && current != g_title_start_original && current == g_title_start_last_substituted)
    {
        mod::set_text(widget, g_title_start_original.c_str());
    }
}

// Latches on an implausible selected index (what a wrong secondary-base adjust reads). A bad read can look plausible later, so one failure disables the gate
// for good.
bool g_title_base_disabled = false;

void repl_title_update_state(void *self)
{
    // The detour's `this` sits pal::layout::kTitleSecondaryBaseAdjust past the TitleScreen base that
    // mth::layout::kTitle* are relative to (0 on Linux).
    void *base = self != nullptr ? static_cast<char *>(self) - pal::layout::kTitleSecondaryBaseAdjust : nullptr;
    int *idx = base != nullptr ? &pal::at<int>(base, mth::layout::kTitleSelectedIndexOff) : nullptr;
    const bool base_ok = idx != nullptr && *idx >= 0 && *idx < mth::layout::kTitleOptionCount;
    if (idx != nullptr && !base_ok && !g_title_base_disabled)
    {
        g_title_base_disabled = true;
        pal::logf(pal::LogLevel::Warn, "title: selected-index=%d out of [0,%d) at base adjust -0x%x; cursor gate/label permanently disabled", *idx,
                  mth::layout::kTitleOptionCount, static_cast<unsigned>(pal::layout::kTitleSecondaryBaseAdjust));
    }
    const int previous = base_ok ? *idx : 0;

    if (g_orig_title_update)
        g_orig_title_update(self);

    if (!base_ok || g_title_base_disabled)
        return; // guard tripped now or earlier: no writes through an unverified base

    if (g_title_gate_fn && !g_title_gate_fn())
    {
        // After the game's own wrap, and direction-aware: see mth::skip_gated_option.
        *idx = mth::skip_gated_option(previous, *idx);
    }

    apply_title_start_text(base);
}

bool install_title_gate_hook(std::function<bool()> connected)
{
    g_title_gate_fn = std::move(connected);
    const std::uintptr_t addr = pal::resolve_game_symbol(mth::sym::title_screen_update_state);
    if (addr == 0)
    {
        pal::logf(pal::LogLevel::Warn, "title: TitleScreen::UpdateState not resolved; menu gating off");
        g_title_gate_fn = nullptr;
        return false;
    }
    g_title_gate_hook = pal::ScopedHook(addr, &repl_title_update_state, &g_orig_title_update);
    if (!g_title_gate_hook.installed())
    {
        pal::logf(pal::LogLevel::Error, "title: failed to hook TitleScreen::UpdateState");
        g_title_gate_fn = nullptr;
        return false;
    }
    pal::logf(pal::LogLevel::Info, "title: hooked TitleScreen::UpdateState at %p", g_title_gate_hook.target());
    return true;
}

// Sets the "Start Game" label. Applied from the UpdateState detour because the localization refresh re-sets all three option strings. Empty or null restores
// the cached original.
void set_title_start_option_text(const char *text)
{
    std::lock_guard<std::mutex> lk(g_title_start_text_mutex);
    g_title_start_text = (text != nullptr) ? text : "";
}

// TitleScreen::StartGame(): backstop to the cursor gate. UpdateState writes the cursor and dispatches the confirm in one call, so a cursor fix after the
// original returns is too late; StartGame is suppressed outright instead.
std::function<bool()> g_start_game_suppress_fn;
[[clang::no_destroy]] pal::ScopedHook g_start_game_hook;
void (*g_orig_title_start_game)(void *) = nullptr;

void repl_title_start_game(void *self)
{
    if (g_start_game_suppress_fn && g_start_game_suppress_fn())
        return; // disconnected, or a launch SaveTakeover did not claim
    if (g_orig_title_start_game)
        g_orig_title_start_game(self);
}

bool install_start_game_suppress_hook(std::function<bool()> suppress)
{
    g_start_game_suppress_fn = std::move(suppress);
    const std::uintptr_t addr = pal::resolve_game_symbol(mth::sym::title_screen_start_game);
    if (addr == 0)
    {
        pal::logf(pal::LogLevel::Warn, "title: TitleScreen::StartGame not resolved; start-game backstop off");
        g_start_game_suppress_fn = nullptr;
        return false;
    }
    g_start_game_hook = pal::ScopedHook(addr, &repl_title_start_game, &g_orig_title_start_game);
    if (!g_start_game_hook.installed())
    {
        pal::logf(pal::LogLevel::Error, "title: failed to hook TitleScreen::StartGame");
        g_start_game_suppress_fn = nullptr;
        return false;
    }
    pal::logf(pal::LogLevel::Info, "title: hooked TitleScreen::StartGame at %p", g_start_game_hook.target());
    return true;
}

} // namespace

namespace mth
{

void install_title_gate(std::function<bool()> connected, std::function<bool()> claim_start)
{
    // Copied, not moved: the cursor hook below takes it too.
    auto suppress = [connected, claim_start = std::move(claim_start)]()
    {
        if (!connected())
            return true;
        // A claimed launch must let the vanilla StartGame run, since its substate builds the profile-select menu. An unclaimed one is blocked so it cannot
        // reach a vanilla save.
        return !(claim_start && claim_start());
    };
    auto gate = [connected = std::move(connected)]()
    {
        const bool up = connected();
        set_title_start_option_text(up ? "" : "Disconnected"); // empty restores the localized label
        return up;
    };
    if (!install_title_gate_hook(std::move(gate)))
        pal::logf(pal::LogLevel::Warn, "title: gating inactive; Start Game stays selectable");
    if (!install_start_game_suppress_hook(std::move(suppress)))
        pal::logf(pal::LogLevel::Warn, "title: start-game backstop inactive; Start Game not blocked if reached");
}

void remove_title_gate()
{
    g_start_game_hook.reset();
    g_start_game_suppress_fn = nullptr;
    g_title_gate_hook.reset();
    g_title_gate_fn = nullptr;
}

} // namespace mth
