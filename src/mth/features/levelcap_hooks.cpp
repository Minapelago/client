#include "mth/features/levelcap_hooks.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

#include "mod/mod_api.hpp"
#include "mth/core/ap/ap_state.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/features/scene_gates/scene_walk.hpp"
#include "pal/pal_game.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"

namespace mth
{
namespace
{

// Bone-up description text: the level to display for the cursor-selected stat, or a negative value to
// leave the vanilla text alone.
std::function<int(int stat)> g_boneup_cap;

// Runs at frame rate, so a reading is logged only when it changes. Silent while the menu is closed, since LevelUpMenu ticks with no display all session.
void probe(int state, int stat, const void *display, const void *text, const char *current)
{
    static int last = -1;
    const int kind = current == nullptr ? 0 : (*current == '\0' ? 1 : 2);
    const int key = ((state + 1) * 64) + ((stat + 1) * 4) + kind;
    if (key == last)
        return;
    last = key;
    pal::logf(pal::LogLevel::Debug, "boneup probe: state=%d stat=%d display=%p text=%p str=\"%.40s\"", state, stat, display, text,
              current != nullptr ? current : "<null>");
}

// Called from the per-frame LevelUpMenu wrapper on both platforms; walks to the menu's description
// widget and appends the enforced cap to its first line.
void boneup_annotate_description(void *level_up_menu)
{
    if (!g_boneup_cap || level_up_menu == nullptr)
        return;

    const int state = pal::at<int>(level_up_menu, layout::kLevelUpMenuStateOff);
    const int stat = pal::at<int>(level_up_menu, layout::kLevelUpMenuStatOff);

    // Both offsets are pinned on Linux and Windows r149150, so a mismatch means the game moved. Each hop must carry a vtable in the module's read-only data, so
    // a drifted offset landing on another live pointer is rejected before any write.
    void *display = pal::at<void *>(level_up_menu, layout::kLevelUpMenuDisplayOff);
    void *text = looks_like_component(display) ? pal::at<void *>(display, layout::kLevelUpDisplayDescOff) : nullptr;
    const char *current = looks_like_component(text) ? mod::text_of(text) : nullptr;

    if (display != nullptr || state != 0)
        probe(state, stat, display, text, current);

    if (state != layout::kLevelUpMenuInteractiveState)
        return;
    if (current == nullptr || *current == '\0')
        return; // constructed but not yet filled: the menu intro, and the frame a purchase rebuilds it

    const int display_cap = g_boneup_cap(stat);
    if (display_cap < 0)
        return;

    // Runs every frame the menu is open, so settle the common already-annotated case without allocating.
    char want[16];
    const int want_len = std::snprintf(want, sizeof want, " (%d)", display_cap);
    const char *eol = std::strchr(current, '\n');
    const std::size_t line1 = eol != nullptr ? static_cast<std::size_t>(eol - current) : std::strlen(current);
    if (want_len > 0 && line1 >= static_cast<std::size_t>(want_len) &&
        std::memcmp(current + line1 - static_cast<std::size_t>(want_len), want, static_cast<std::size_t>(want_len)) == 0)
        return;

    const std::string annotated = boneup_with_cap_suffix(current, display_cap);
    if (annotated != current)
    {
        const bool ok = mod::set_text(text, annotated.c_str());
        pal::logf(pal::LogLevel::Debug, "boneup: stat=%d cap=%d set_text=%d line1=\"%.48s\"", stat, display_cap, ok ? 1 : 0, annotated.c_str());
    }
}

} // namespace

LevelCapHooks::LevelCapHooks()
{
    if (!pal::level_cap_available())
    {
        pal::logf(pal::LogLevel::Warn, "levelcap: PAL unavailable; LevelCapHooks inert");
        return;
    }
    pal::set_level_cap_provider([this](int stat, int vanilla) { return provide(stat, vanilla); }, &boneup_annotate_description);
    g_boneup_cap = [this](int stat) { return display_cap_for(stat); };
    pal::logf(pal::LogLevel::Info, "levelcap: LevelCapHooks installed");
}

void LevelCapHooks::recompute(const ApState &state)
{
    caps_.recompute(state);
    max_stat_level_ = state.config().max_stat_level;
}

void LevelCapHooks::set_counts(int attack, int defense, int sidearm)
{
    caps_.set_counts(attack, defense, sidearm);
}

void LevelCapHooks::set_enforce_live(bool on)
{
    enforce_live_.store(on);
}

int LevelCapHooks::provide(int stat, int vanilla_cap)
{
    if (!enforce_live_.load())
        return vanilla_cap; // vanilla play: not connected, not test mode
    // Per-stat ceiling (slot_data max for real stats, native for the bone bank), then cap-up gating.
    return caps_.enforced_cap(stat, stat_cap_ceiling(stat, max_stat_level_, vanilla_cap));
}

int LevelCapHooks::display_cap_for(int stat) const
{
    if (!enforce_live_.load() || stat < 0 || stat >= kStatCount)
        return -1;
    return boneup_display_cap(caps_.enforced_cap(stat, max_stat_level_));
}

std::vector<std::string> LevelCapHooks::status_lines() const
{
    std::vector<std::string> out;
    out.push_back("stat caps (granted): attack=" + std::to_string(caps_.granted(0)) + " defense=" + std::to_string(caps_.granted(1)) +
                  " sidearm=" + std::to_string(caps_.granted(2)) + " max_stat_level=" + std::to_string(max_stat_level_));
    return out;
}

} // namespace mth
