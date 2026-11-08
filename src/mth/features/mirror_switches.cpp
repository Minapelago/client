#include "mth/features/mirror_switches.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <string>

#include "mod/mod_api.hpp"
#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/ap/ap_state.hpp"
#include "mth/core/game/component_types.hpp"
#include "mth/core/game/game_state_ids.hpp"
#include "mth/features/scene_gates/scene_walk.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"

namespace
{

// SwitchOre fields, carved from its Linux constructor. Its second vptr at +0x178 (CombatCoreImplementor sub-object) is where MSVC and Itanium layouts can
// disagree, so these are unverified on Windows; check the `switches` probe output there first.
constexpr std::ptrdiff_t kStateOff = 0x194;      // int; 0 = on, 2 = off
constexpr std::ptrdiff_t kWorldOff = 0x168;      // ptr; the SwitchManager hangs off it
constexpr std::ptrdiff_t kTimerOff = 0x200;      // float; < 0 = untimed, > 0 = auto-off after this long
constexpr std::ptrdiff_t kSwitchIdOff = 0x204;   // int; indexes the SwitchManager's state array
constexpr std::ptrdiff_t kColorOff = 0x208;      // uint 0..4
constexpr std::ptrdiff_t kCanTurnOffOff = 0x210; // bool; cleared by the CantTurnOff level property

// SwitchManager, reached as *(switch + kWorldOff) + kManagerOff. Its state array is the live authority: the switch and everything keyed to the same SwitchID
// (the colored platforms too) poll their entry and follow it. Writing the entry is the whole feature, and the switch's own state does the vanilla persist.
constexpr std::ptrdiff_t kManagerOff = 0x3a8;
constexpr std::ptrdiff_t kStateArrayOff = 0x470;
constexpr int kMaxSwitchId = 0x40; // the game's own bound on SwitchID, and the length of the array

// Ticks before a failed walk is retried, as in the other scene walks. An established list is reused every frame without walking.
constexpr int kRetryCooldownTicks = 60;

// Each shortcut switch's SwitchID by game color, from the Mirrors End level data (r149150). IDs are per level and every room builds a fresh manager seeded
// from each switch's saved state, so purple platforms in the switch room follow the purple switch alone in the Coltrane Peak mirror room. apply() therefore
// writes every color by ID, not only the switches the room holds.
constexpr std::array<int, mth::kAstralPlatformCount> kSwitchIdByColor{
    5, // yellow
    3, // green
    4, // blue
    6, // purple
    2, // red
};

// Level-data order, which is what the palette table pins the Color property to.
const char *color_name(std::uint32_t c)
{
    switch (c)
    {
    case 0:
        return "yellow";
    case 1:
        return "green";
    case 2:
        return "blue";
    case 3:
        return "purple";
    case 4:
        return "red";
    default:
        return "?";
    }
}

std::atomic<bool> g_probe_requested{false};
std::atomic<bool> g_console_override{false};
std::uint32_t g_logged_granted{0xffffffffu}; // impossible mask, so the first pass always logs
bool g_logged_inert{false};
// One switch in the room, through which apply() reaches the SwitchManager all of the room's switches share. Held through the game's weak pointer so its death
// with the room is reported.
void *g_anchor{nullptr};
void *g_managed_world{nullptr}; // the world g_anchor was walked from; null means no switch is anchored yet
int g_retry_cooldown{0};
bool g_warned_api{false};
mth::SceneWalker g_walker{"switches", "a rainbow switch", " (#28)"};

void forget()
{
    if (g_anchor != nullptr)
        mod::weak_ptr_destroy(g_anchor);
    g_anchor = nullptr;
    g_managed_world = nullptr;
}

int switch_id(const void *component)
{
    return *reinterpret_cast<const int *>(static_cast<const char *>(component) + kSwitchIdOff);
}

std::uint32_t switch_color(const void *component)
{
    return *reinterpret_cast<const std::uint32_t *>(static_cast<const char *>(component) + kColorOff);
}

// The switch's entry in its world's SwitchManager, or null when the chain does not look walkable.
int *manager_entry(const void *component, int id)
{
    if (id < 0 || id >= kMaxSwitchId)
        return nullptr;
    const char *sw = static_cast<const char *>(component);
    void *world = *reinterpret_cast<void *const *>(sw + kWorldOff);
    // Vtable range check: a drifted layout would otherwise carry a plausible value from this hop into a raw offset read.
    if (!mth::looks_like_component(world))
        return nullptr;
    void *mgr = *reinterpret_cast<void *const *>(static_cast<const char *>(world) + kManagerOff);
    if (!pal::pointer_looks_valid(mgr)) // SwitchManager is not known to be polymorphic, so no vtable test
        return nullptr;
    return reinterpret_cast<int *>(static_cast<char *>(mgr) + kStateArrayOff + id * 4);
}

void dump_switch(void *component, int n)
{
    const char *sw = static_cast<const char *>(component);
    const int id = switch_id(component);
    const std::uint32_t color = switch_color(component);
    const int *entry = manager_entry(component, id);
    pal::logf(pal::LogLevel::Info, "switches: [%d] %p id=%d color=%u(%s) state=%d live=%d canTurnOff=%d timer=%.1f", n, component, id, color, color_name(color),
              *reinterpret_cast<const int *>(sw + kStateOff), entry != nullptr ? *entry : -1,
              *reinterpret_cast<const std::uint8_t *>(sw + kCanTurnOffOff) != 0 ? 1 : 0, static_cast<double>(*reinterpret_cast<const float *>(sw + kTimerOff)));
}

// Drives every color's entry to the granted set through the anchor's manager. false means the anchor no longer describes the room, so the caller drops it and
// walks again.
bool apply(std::uint32_t granted)
{
    void *component = mod::weak_ptr_get(g_anchor);
    if (component == nullptr)
        return false; // the switch died with its room
    for (std::uint32_t c = 0; c < static_cast<std::uint32_t>(mth::kAstralPlatformCount); ++c)
    {
        const int id = kSwitchIdByColor[c];
        int *entry = manager_entry(component, id);
        if (entry == nullptr)
            return false;
        const int want = ((granted >> c) & 1u) != 0 ? 1 : 0;
        if (*entry == want)
            continue;
        // Only a real transition logs: the grant, or a hand-hit switch going back to shut.
        *entry = want;
        pal::logf(pal::LogLevel::Info, "switches: %s switch (id %d) forced %s (#28)", color_name(c), id, want != 0 ? "open" : "shut");
    }
    return true;
}

// The set as a log line. Logging an empty mask tells a switch that never opens apart from a broken clamp.
std::string granted_names(std::uint32_t mask)
{
    if (mask == 0)
        return "nothing";
    std::string out;
    for (std::uint32_t c = 0; c < static_cast<std::uint32_t>(mth::kAstralPlatformCount); ++c)
        if (((mask >> c) & 1u) != 0)
            out += (out.empty() ? "" : ", ") + std::string(color_name(c));
    return out;
}

std::uint32_t granted_colors(const mth::ApState &state)
{
    std::uint32_t mask = 0;
    for (const auto &it : state.received_items())
        if (mth::is_astral_platform_item(it.item_id))
            mask |= 1u << static_cast<unsigned>(mth::astral_platform_color(it.item_id));
    return mask;
}

// Called for every switch the walk finds in a room not yet anchored. Any switch can be the anchor. A shortcut switch, whose ID is its color's entry in
// kSwitchIdByColor, is logged as it comes under AP control, so a room that logs none shows a drifted layout or ID table without the probe.
void take(void *component)
{
    const int id = switch_id(component);
    const std::uint32_t color = switch_color(component);
    if (color < static_cast<std::uint32_t>(mth::kAstralPlatformCount) && id == kSwitchIdByColor[color])
        pal::logf(pal::LogLevel::Info, "switches: driving the %s switch (id %d) from AP (#28)", color_name(color), id);
    if (g_anchor != nullptr)
        return;
    g_anchor = mod::weak_ptr_create(component);
    if (g_anchor == nullptr)
        pal::logf(pal::LogLevel::Warn, "switches: no weak pointer for switch id %d; trying the next (#28)", id);
}

} // namespace

namespace mth
{

void request_switch_probe()
{
    g_probe_requested.store(true);
}

void set_mirror_switches_override_flag(bool on)
{
    g_console_override.store(on);
}

void mirror_switches_on_world_destroy()
{
    forget(); // a door or warp rebuilds the World, and a switch does not outlive it
}

void tick_mirror_switches(const ApState &state, bool slot_ok)
{
    const bool probe = g_probe_requested.exchange(false);
    // Confined to the switches' area, so elsewhere this costs a comparison. The clamp drives a state entry the switch persists, so it waits for the bound AP
    // save like every durable write. The console override replaces the session gate, since the seed flag cannot be set by hand offline; it only turns
    // enforcement on. The gamestate test applies either way.
    const bool console = g_console_override.load();
    const bool armed = console || (state.authenticated() && slot_ok && state.config().mirror_switch_rando);
    const bool in_hub = mod::current_game_state() == kGameStateMirrorHub && mod::room_index() != kGameStateMirrorHubPathToAO;
    const bool wanted = armed && in_hub;
    // Standing among undriven switches looks like a broken clamp, and the console override does not survive a restart, so log the missing condition once per
    // visit.
    if (!in_hub)
        g_logged_inert = false;
    else if (!armed && !g_logged_inert)
    {
        g_logged_inert = true;
        pal::logf(pal::LogLevel::Debug, "switches: not driving these switches (authed=%d bound_save=%d seed_flag=%d console=%d) (#28)",
                  state.authenticated() ? 1 : 0, slot_ok ? 1 : 0, state.config().mirror_switch_rando ? 1 : 0, console ? 1 : 0);
    }
    // The probe only needs the walk; driving also needs weak pointers. Warned once so a seed with the switches on an inert modding API says why they stay shut.
    const bool walk_api = mod::entity_walk_api_available();
    const bool enforce = wanted && walk_api && mod::weak_ptr_api_available();
    if (wanted && !enforce && !g_warned_api)
    {
        g_warned_api = true;
        pal::logf(pal::LogLevel::Warn, "switches: modding API unavailable (revision=%u); the shortcut switches are left vanilla (#28)", mod::game_revision());
    }

    if (!enforce)
        forget();
    if (!probe && !enforce)
        return;
    if (!walk_api)
    {
        if (probe) // an explicit console action always gets an answer
            pal::logf(pal::LogLevel::Warn, "switches: the modding entity-walk API is unavailable (revision=%u)", mod::game_revision());
        return;
    }
    void *world = mod::player_world(); // null until a player is live, which is also when no room exists
    if (world == nullptr)
    {
        if (probe)
            pal::logf(pal::LogLevel::Warn, "switches: no live world to walk; stand in a room and try again");
        return;
    }
    if (world != g_managed_world)
        forget();

    bool walk_now = probe;
    if (enforce && g_managed_world == nullptr)
    {
        if (g_retry_cooldown > 0)
            --g_retry_cooldown;
        else
            walk_now = true;
    }

    if (walk_now)
    {
        void *root = mod::world_game_root_entity(world);
        if (root == nullptr)
        {
            if (probe)
                pal::logf(pal::LogLevel::Warn, "switches: the world has no scene root to walk");
            return;
        }
        int found = 0;
        const SceneWalk walk = g_walker.for_each(root, rtti::kSwitchOre,
                                                 [&](void *c)
                                                 {
                                                     if (probe)
                                                         dump_switch(c, found++);
                                                     if (enforce && g_managed_world == nullptr)
                                                         take(c);
                                                 });
        g_walker.report(walk);
        // Latched only once a switch is anchored, so a walk before the room finished building is retried.
        if (enforce && g_managed_world == nullptr)
        {
            if (g_anchor == nullptr)
                g_retry_cooldown = kRetryCooldownTicks;
            else
                g_managed_world = world;
        }
        if (probe)
        {
            const auto screen = (static_cast<std::uint32_t>(mod::current_game_state()) << 16) | (static_cast<std::uint32_t>(mod::room_index()) & 0xffffu);
            pal::logf(pal::LogLevel::Info, "switches: %d found in screen %#x", found, screen);
        }
    }

    if (!enforce)
        return;
    const std::uint32_t granted = granted_colors(state);
    if (granted != g_logged_granted)
    {
        g_logged_granted = granted;
        pal::logf(pal::LogLevel::Info, "switches: AP has granted %s (#28)", granted_names(granted).c_str());
    }
    if (g_managed_world != nullptr && !apply(granted))
    {
        forget(); // the anchor stopped describing the room
        g_retry_cooldown = kRetryCooldownTicks;
    }
}

} // namespace mth
