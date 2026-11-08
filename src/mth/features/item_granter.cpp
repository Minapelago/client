#include "mth/features/item_granter.hpp"

#include <cmath>
#include <cstddef>
#include <functional>

#include "mod/mod_api.hpp"
#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/ap/grant_queue.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/core/game/game_symbols.hpp"
#include "mth/features/game_table_access.hpp"
#include "mth/features/player_tracker.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_module.hpp"

namespace
{

struct YcVec3
{
    float x, y, z;
};

std::function<bool(int)> g_is_ap_location;   // RandoBridge::is_ap_location, wired by App
std::function<void(int)> g_report_collected; // RandoBridge::on_location_collected, wired by App
bool g_train_pass_machine_blocked = false;   // train_rando: the donation machine grants nothing (#162)

// Items::OnPickupDone(int slot, int itemType, Player*, ycVec3 const&, int, int, unsigned int, bool). Resolved only to be called: suppression uses the
// ItemsOnPickupDone named hook, which hands back no callable original. Calling it re-enters our hook with slot -1, which suppression passes over.
void (*g_call_on_pickup_done)(int, int, void *, void *, int, int, unsigned int, bool) = nullptr;

mth::GrantQueue g_pending; // grant() enqueues, drain() replays in the engine's update window

// Start of Items::OnPickupDone. Returns true to stop the original running.
bool on_items_pickup_done(int slot, int item_type, void *player)
{
    mth::player_tracker::note_player(player); // refresh the grant-target player for inbound replays

    // These re-seed the vial bitfield behind the AP count (#171); App re-asserts it, so this only traces what fired.
    if (item_type == mth::layout::kItemHealingVialFirst || item_type == mth::layout::kItemVialUpgrade)
        pal::logf(pal::LogLevel::Debug, "vials: OnPickupDone itemType=%#x slot=%d", item_type, slot);

    if (mth::should_suppress_vanilla_grant(slot, item_type, g_is_ap_location && g_is_ap_location(slot)))
    {
        pal::logf(pal::LogLevel::Info, "outbound: suppressed vanilla grant for AP location %d (itemType=%d)", slot, item_type);
        // Grants straight through OnPickupDone (no Pickup entity or ShopMenu, e.g. the train-ticket machine) never reach the pickup/shop hooks, so the check is
        // sent here. The bridge dedups, so the world-pickup and Windows-shop paths cannot double-send.
        if (g_report_collected)
            g_report_collected(slot);
        return true;
    }
    return false;
}

// Items::OnPickup runs before OnPickupDone and ORs armor upgrades (Vitality Vest 0x4f, Damage armor 0x50) into SaveSlot+0xc68 itself, so suppressing only
// OnPickupDone leaks the vanilla armor for an AP shop buy (#71). Only those two itemTypes are suppressed here; the collect report mirrors OnPickupDone.
bool on_items_pickup(int slot, int item_type, void *player)
{
    mth::player_tracker::note_player(player);

    // Backstop to TicketMachineGate: reaching here means the prompt suppression missed. Refusing the grant costs the player the donated bones, so it is only
    // the second line of defence.
    if (g_train_pass_machine_blocked && mth::is_train_pass_machine_grant(slot, item_type))
    {
        pal::logf(pal::LogLevel::Warn, "train: refused the donation machine's Train Pass grant (#162)");
        return true;
    }

    if (slot >= 0 && mth::tables::is_armor_upgrade_itemtype(item_type) && g_is_ap_location && g_is_ap_location(slot))
    {
        pal::logf(pal::LogLevel::Info, "outbound: suppressed vanilla armor upgrade for AP location %d (itemType=%d)", slot, item_type);
        if (g_report_collected)
            g_report_collected(slot);
        return true;
    }
    return false;
}

} // namespace

namespace mth
{

void set_train_pass_machine_blocked(bool on)
{
    g_train_pass_machine_blocked = on;
}

ItemGranter::ItemGranter(std::function<bool(int)> is_ap_location, std::function<void(int)> report_collected)
{
    g_is_ap_location = std::move(is_ap_location);
    g_report_collected = std::move(report_collected);
    g_call_on_pickup_done = reinterpret_cast<void (*)(int, int, void *, void *, int, int, unsigned int, bool)>(pal::resolve_game_symbol(sym::on_pickup_done));
    if (g_call_on_pickup_done == nullptr)
        pal::logf(pal::LogLevel::Warn, "ItemGranter: Items::OnPickupDone not resolved; inbound grants disabled");
    mod::install_items_on_pickup_done_hook(&on_items_pickup_done);
    mod::install_items_on_pickup_hook(&on_items_pickup);
}

bool ItemGranter::grant(int item_type, int receipt)
{
    // Require hook + Player* before accepting; return false to retry next tick.
    if (g_call_on_pickup_done == nullptr || player_tracker::player() == nullptr)
        return false;

    // itemType 0 = engine "None" sentinel; nothing to apply, so ack straight away.
    if (item_type <= 0)
    {
        notify_applied(receipt);
        return true;
    }

    // Reachable from the socket (a seed id in the vanilla segment above the row count) and from giveapitem. Acked so a bad id is dropped, not retried forever.
    if (!is_valid_item_type(item_type))
    {
        pal::logf(pal::LogLevel::Error, "inbound: refused item_type=%d; not a game itemType (max %d)", item_type, layout::kItemTypeCount - 1);
        notify_applied(receipt);
        return true;
    }

    g_pending.push(item_type, receipt);
    return true;
}

// Session change: the queue belongs to the old save. Dropping it un-acked makes the caller re-offer each receipt against the new save (#175).
void ItemGranter::discard_pending()
{
    if (const std::size_t dropped = g_pending.clear(); dropped != 0)
        pal::logf(pal::LogLevel::Info, "inbound: dropped %zu queued grant(s) on session change", dropped);
}

void ItemGranter::drain()
{
    float p[3];
    void *player = nullptr;
    g_pending.drain(
        [&]
        {
            // Position comes from the tracker's cache: PlayerGetPos3 unconditionally dereferences a second-level pointer that is null in the
            // pre-World::Update spawn window. Requeue until cached.
            player = player_tracker::player();
            const bool ready = g_call_on_pickup_done && player != nullptr && player_tracker::position(p);
            return ready && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
        },
        [&](int item_type)
        {
            // locIdx=-1: grant by type only, no location state touched.
            YcVec3 grant_pos{p[0], p[1], p[2]}; // fresh copy per item (ycVec3 const&)
            g_call_on_pickup_done(-1, item_type, player, &grant_pos, 0, 0, 0, false);
            pal::logf(pal::LogLevel::Info, "inbound: granted item_type=%d (kind=%d)", item_type, tables::storage_kind(item_type));
        },
        // Upstream durable state keys off the ack, so a batch that never gets applied is retried (#175).
        [this](int receipt) { notify_applied(receipt); });
}

} // namespace mth
