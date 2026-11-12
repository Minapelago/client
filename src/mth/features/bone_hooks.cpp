#include "mth/features/bone_hooks.hpp"

#include <cstdint>
#include <cstdio>

#include "mod/mod_api.hpp"
#include "mth/core/ap/rando_bridge.hpp"
#include "mth/core/game/bone_locations.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/features/name_key.hpp"
#include "mth/features/room_tracker.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"

namespace mth::bone_hooks
{

namespace
{
void *g_marked = nullptr; // the last bone pickup to run OnPickup; ends at its grant, the next pickup, that pickup's re-Init or the tick

// Packed like mth::banner_color, r in the low byte.
constexpr std::uint32_t kLabelIncluded = 0xff50e6ffu; // yellow
constexpr std::uint32_t kLabelExcluded = 0xff6060ffu; // red
constexpr std::uint32_t kLabelChecked = 0xff969696u;  // grey
constexpr std::uint32_t kLabelShadow = 0xff000000u;
constexpr float kLabelSize = 0.6f; // world units; one level pixel is 0.1
constexpr float kMarkerSize = 0.8f;
constexpr float kShadowOffset = 0.005f;

bool g_labels = false;
bool g_font_tried = false;
bool g_font = false;
} // namespace

void on_pickup(void *pickup)
{
    const bool bone = is_bone_pickup(pal::at<int>(pickup, layout::kPickupLocIdxOff), pal::at<int>(pickup, layout::kPickupItemTypeOff));
    g_marked = bone ? pickup : nullptr;
}

int take_grant(int slot, int item_type)
{
    // Pickup::OnPickup calls Items::OnPickup only on its collect path, after the SaveSlot writes, so a buried bone's dig-up call never gets here.
    const int live_type = g_marked != nullptr ? pal::at<int>(g_marked, layout::kPickupItemTypeOff) : -1;
    if (!is_bone_grant(slot, item_type, live_type))
        return -1;
    void *pickup = g_marked;
    g_marked = nullptr;
    const BoneRow *row = find_bone(component_name_key(pickup));
    return row != nullptr ? bone_location_id(*row) : -1;
}

void on_pickup_init(void *pickup)
{
    if (g_marked == pickup)
        g_marked = nullptr;
}

void tick()
{
    g_marked = nullptr;
}

void set_labels(bool on)
{
    g_labels = on;
}

void draw_labels(const RandoBridge &bridge)
{
    if (!g_labels)
        return;
    if (!g_font_tried)
    {
        g_font_tried = true;
        g_font = mod::debug_draw_load_font();
        pal::logf(g_font ? pal::LogLevel::Info : pal::LogLevel::Error, "bones: label font %s", g_font ? "loaded" : "FAILED to load, no labels");
    }
    void *dd = g_font ? mod::debug_draw("World") : nullptr;
    float player[3];
    std::uint32_t screen;
    if (dd == nullptr || !mod::player_position(player) || !room_tracker::current_screen(&screen))
        return;
    // ponytail: linear scan of every row per frame while labels are on; index by screen if it shows in a profile.
    for (const BoneRow &row : bone_rows())
    {
        float w[2];
        if (row.screen != screen || !mod::level_to_world(static_cast<float>(row.x), static_cast<float>(row.y), w))
            continue;
        const int id = bone_location_id(row);
        const std::uint32_t color = bridge.is_checked(id) ? kLabelChecked : row.excluded ? kLabelExcluded : kLabelIncluded;
        // Level files carry no depth, so the label takes the player's.
        const float at[3] = {w[0], w[1], player[2]};
        mod::debug_draw_rect(dd, at, kMarkerSize, kMarkerSize, color);
        char text[16];
        std::snprintf(text, sizeof(text), "%d", id);
        const float pos[3] = {w[0] - kMarkerSize / 2, w[1] + kMarkerSize / 2, player[2]};
        const float shadow[3] = {pos[0] + kShadowOffset, pos[1] - kShadowOffset, player[2]};
        mod::debug_draw_text(dd, text, shadow, kLabelSize, kLabelShadow);
        mod::debug_draw_text(dd, text, pos, kLabelSize, color);
    }
}

} // namespace mth::bone_hooks
