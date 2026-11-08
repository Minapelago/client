#include "mth/features/location_hooks.hpp"

#include <bit>
#include <cstdint>
#include <functional>
#include <set>
#include <vector>

#include "mod/mod_api.hpp"
#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/ap/broadcast.hpp"
#include "mth/core/ap/rando_bridge.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/core/game/game_symbols.hpp"
#include "mth/features/game_table_access.hpp"
#include "mth/features/menus/shop_menu.hpp"
#include "pal/pal_game.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"

namespace
{

mth::RandoBridge *g_bridge = nullptr;
const std::map<int, mth::ScoutInfo> *g_scout = nullptr;

// Kear key gating (slot_data "kear_rando"), game thread only. Neutralize runs in every AP mode: a kear pickup is an AP location, so the free key its collect
// grants is cancelled. Suppress (pin usable keys to 0 via reconcile) runs only in the AP-item modes; vanilla mode credits received Universal Kears instead
// (credit_kear_key).
bool g_kear_neutralize = false;
bool g_kear_suppress = false;

// Live Player* accessor (set by App via HookManager). Kear key edits move the Player+0x11b0 spent mirror with SaveSlot+0x1f8, or the spend gate discards a
// SaveSlot-only write on the next lock.
std::function<void *()> g_kear_player_getter;

constexpr int kKearStorageKind = 8; // s_rItems kind 8: kear/key items, whose collected bit is the usable-key bit (+0x1f0)

// Offset self-checks (build drift): cleared on first mismatch, disabling the feature loudly.
bool g_pickup_offsets_ok = true;
bool g_shop_offsets_ok = true;

// Latched by the first vanilla small-treasure payout, to keep the rest at Debug (#148).
bool g_small_treasure_seen = false;

// Kear offset self-check for the usable-key u64 bitfield (+0x1f0) and its spent counter (+0x1f8). An implausible read means they drifted, so kear writes are
// disabled.
bool g_kear_offsets_ok = true;

[[nodiscard]] bool kear_fields_plausible(int spent)
{
    // The vanilla credit ledger drives spent negative (usable = popcount - spent, and a received kear lowers spent below the 0 floor), bounded by the game's 50
    // kears. A garbage read (a pointer or float) lands far outside this window.
    return spent >= -64 && spent <= 64;
}

[[nodiscard]] int &pickup_loc_idx(void *self)
{
    return pal::at<int>(self, mth::layout::kPickupLocIdxOff);
}
[[nodiscard]] int &pickup_item_type(void *self)
{
    return pal::at<int>(self, mth::layout::kPickupItemTypeOff);
}

// A kear collect sets its bit in the SaveSlot+0x1f0 bitfield, which doubles as the usable-key count (usable = popcount(Player+0x11a8) - spent(+0x1f8)). The
// server awards kear-location keys, so each bit SetItemCollected newly set bumps spent to cancel the free key. The spend gate snaps SaveSlot+0x1f8 =
// Player+0x11b0 + 1 on the next lock, so the Player mirror moves too. Delta-based, so re-collecting a set bit is a no-op.
void neutralize_kear_grant(int loc_idx, void *slot, std::uint64_t before)
{
    if (!g_kear_offsets_ok)
        return;
    auto &bits = pal::at<std::uint64_t>(slot, mth::layout::kSaveKearBitsOff);
    const std::uint64_t new_bits = bits & ~before;
    if (new_bits == 0)
        return; // bit already collected; no key was granted, nothing to neutralize

    int &spent = pal::at<int>(slot, mth::layout::kSaveKearSpentOff);
    void *player = g_kear_player_getter ? g_kear_player_getter() : nullptr;
    int *player_spent = player != nullptr ? &pal::at<int>(player, mth::layout::kPlayerKearSpentOff) : nullptr;
    if (!kear_fields_plausible(spent) || (player_spent != nullptr && !kear_fields_plausible(*player_spent)))
    {
        g_kear_offsets_ok = false;
        pal::logf(pal::LogLevel::Error,
                  "kear_rando: SaveSlot/Player spent read implausible (popcount=%d save=%d player=%d); offset may have shifted, kear writes DISABLED",
                  std::popcount(bits), spent, player_spent != nullptr ? *player_spent : 0);
        return;
    }

    const int n = std::popcount(new_bits);
    spent += n;
    if (player_spent != nullptr)
        *player_spent += n; // keep the Player mirror in lockstep
    pal::logf(pal::LogLevel::Info, "kear_rando: neutralized kear grant locIdx=%d new_bits=%d spent+=%d (player_mirror=%d)", loc_idx, n, n,
              player_spent != nullptr);
}

// Reload-durable counterpart to neutralize_kear_grant: a reload rebuilds only the +0x1f0 bitfield, not the collect-time spent bump, so AP-collected kears read
// as usable keys again ("one kear on load"). Each tick under kear_rando this raises +0x1f8 to popcount(+0x1f0), never lowering it, so real lock-spends survive.
// Only the SaveSlot counter is written.
void reconcile_kear_keys()
{
    if (!g_kear_suppress)
        return;
    void *slot = pal::active_save_slot();
    if (slot == nullptr)
        return;
    if (!g_kear_offsets_ok)
        return;
    const std::uint64_t bits = pal::at<std::uint64_t>(slot, mth::layout::kSaveKearBitsOff);
    int &spent = pal::at<int>(slot, mth::layout::kSaveKearSpentOff);
    if (!kear_fields_plausible(spent))
    {
        g_kear_offsets_ok = false;
        pal::logf(pal::LogLevel::Error,
                  "kear_rando: SaveSlot+0x1f0/+0x1f8 read implausible (popcount=%d spent=%d); offset may have shifted, kear writes DISABLED",
                  std::popcount(bits), spent);
        return;
    }
    const int reconciled = mth::tables::kear_reconciled_spent(bits, spent);
    if (reconciled == spent)
        return; // already balanced; no leaked key
    pal::logf(pal::LogLevel::Info, "kear_rando: reconciled spent %d -> %d (popcount=%d) on slot=%p", spent, reconciled, std::popcount(bits), slot);
    spent = reconciled;
}

// Vanilla kear mode (#130): a received Universal Kear grants one usable key. No collected bit matches an AP receipt (the bitfield is per-location), so spent
// drops by one instead (usable = popcount(Player+0x11a8) - spent). The spend gate snaps SaveSlot+0x1f8 = Player+0x11b0 + 1 on the next lock, so both mirrors
// move together. false (caller retries) until a save slot and player are live; InboundGranter's ApSaveState marker guards once-per-receipt.
bool credit_kear_key(void *player)
{
    if (!g_kear_offsets_ok || player == nullptr)
        return false;
    void *slot = pal::active_save_slot();
    if (slot == nullptr)
        return false;

    int &save_spent = pal::at<int>(slot, mth::layout::kSaveKearSpentOff);
    int &player_spent = pal::at<int>(player, mth::layout::kPlayerKearSpentOff);
    if (!kear_fields_plausible(save_spent) || !kear_fields_plausible(player_spent))
    {
        g_kear_offsets_ok = false;
        pal::logf(pal::LogLevel::Error, "kear credit: spent read implausible (save=%d player=%d); offset may have shifted, kear writes DISABLED", save_spent,
                  player_spent);
        return false;
    }
    save_spent -= 1;
    player_spent -= 1;
    pal::logf(pal::LogLevel::Info, "kear credit: granted usable key (spent now save=%d player=%d)", save_spent, player_spent);
    return true;
}

// Writes the native durable collected bit where SetItemCollected is side-effect-free (is_durable_bit_kind: 8=key/kear, 12=bonestone, 19=fish), so the game's
// reload check treats the location like a live collect: the chest spawns opened and the pickup does not respawn. false for other kinds or if SetItemCollected
// is unresolved. Under kear_rando a kear's usable key is neutralized. Idempotent: a set bit is a no-op and the kear neutralize acts only on new bits.
bool apply_native_collected(int loc_idx)
{
    const int kind = mth::tables::native_location_kind(loc_idx);
    if (!mod::set_item_collected_available() || !mth::tables::is_durable_bit_kind(kind))
        return false;

    // Kear (kind 8): capture the bitfield before the write so neutralization targets exactly the new bit.
    const bool neutralize = g_kear_neutralize && kind == kKearStorageKind;
    void *slot = neutralize ? pal::active_save_slot() : nullptr;
    const std::uint64_t before = slot != nullptr ? pal::at<std::uint64_t>(slot, mth::layout::kSaveKearBitsOff) : 0;

    mod::set_item_collected(loc_idx, true, nullptr, nullptr);

    if (neutralize && slot != nullptr)
        neutralize_kear_grant(loc_idx, slot, before); // kear collected bit == usable key; cancel it
    else if (neutralize)
        pal::logf(pal::LogLevel::Warn, "kear_rando: no active save slot; kear grant NOT neutralized (locIdx=%d)", loc_idx);
    return true;
}

// Shared collect path for pickups and shop buys: record/send the check, then write the durable native
// collected-bit where SetItemCollected is side-effect-free.
void collect_ap_location(int loc_idx)
{
    g_bridge->on_location_collected(loc_idx); // mark_checked, persist, send if connected
    if (apply_native_collected(loc_idx))
        pal::logf(pal::LogLevel::Info, "outbound: SetItemCollected(locIdx=%d kind=%d) -> native reload suppression", loc_idx,
                  mth::tables::native_location_kind(loc_idx));
}

// Server-collected (Collect/coop) locations only get .state marked, never the native durable bit, so a durable-bit chest (keys/bonestone/fish) spawns closed.
// Once a save is active this re-asserts the bit for every checked durable-bit slot, plus seed-removed locations so a pruned dungeon's kears, bonestones and
// fish count toward the game's own tallies and all-of-a-kind rewards. Re-runs only when the combined set's size changes or reset_native_bit_enforcement re-arms
// it. Waits for a resolved collection and a bound save slot; the title/profile-select screen already binds one, so this can run before a profile is chosen.
// Game thread, per tick.
int g_native_last_checked_count = -1;
// Log filter only: the re-arm runs on every save load and every door or warp, and replays the same set each time.
std::set<int> g_native_applied_last;

void enforce_checked_native_bits()
{
    if (g_bridge == nullptr || !mod::set_item_collected_available() || !mth::tables::collection_resolved())
        return;
    if (pal::active_save_slot() == nullptr)
        return; // no active save yet (e.g. connected at the title screen); retry next tick

    const std::set<int> *checked = g_bridge->checked_slots();
    const std::set<std::int64_t> &removed = g_bridge->removed_slots();
    // Removed locations apply before a save state attaches too, so a null checked set counts as empty.
    const int total = static_cast<int>((checked != nullptr ? checked->size() : 0) + removed.size());
    if (total == g_native_last_checked_count)
        return; // nothing new since the last pass

    std::set<int> applied;
    if (checked != nullptr)
        for (int slot : *checked)
            if (apply_native_collected(slot))
                applied.insert(slot);
    for (std::int64_t id : removed)
        if (id >= 0 && id < mth::layout::kLocationCount && apply_native_collected(static_cast<int>(id)))
            applied.insert(static_cast<int>(id));

    g_native_last_checked_count = total;
    if (!applied.empty() && applied != g_native_applied_last)
        pal::logf(pal::LogLevel::Info, "collect: applied native collected-bit to %d durable-bit location(s)", static_cast<int>(applied.size()));
    g_native_applied_last = std::move(applied);
}

// Re-arms the enforcement for the next tick: a save reload clears our in-memory s_rItemCollection writes, and a world teardown signals one may have happened.
void reset_native_bit_enforcement()
{
    g_native_last_checked_count = -1;
}

void (*g_orig_pickup_init)(void *, int, int, bool) = nullptr;

void repl_pickup_init(void *self, int item_type, int loc_idx, bool flag)
{
    if (g_orig_pickup_init)
        g_orig_pickup_init(self, item_type, loc_idx, flag);

    // Offset self-check: a stored loc_idx that differs from the argument means the layout shifted, so the redirect is disabled. A vanilla small-treasure payout
    // is the exception, since the pickup is no longer this AP location (#148).
    if (g_pickup_offsets_ok && pickup_loc_idx(self) != loc_idx)
    {
        const int stored_loc = pickup_loc_idx(self);
        if (mth::tables::is_small_treasure_collect_rewrite(stored_loc, pickup_item_type(self)))
        {
            // Info once per session: drift that happened to read as this pair would take this return on every pickup, and Debug alone would hide it in a user's
            // log.
            pal::logf(g_small_treasure_seen ? pal::LogLevel::Debug : pal::LogLevel::Info,
                      "Pickup::Init locIdx=%d -> vanilla collected-treasure payout (small bones)", loc_idx);
            g_small_treasure_seen = true;
            return;
        }
        g_pickup_offsets_ok = false;
        pal::logf(pal::LogLevel::Error, "Pickup offset check FAILED (stored=%d arg=%d); outbound redirect disabled", stored_loc, loc_idx);
        return;
    }
    if (!g_pickup_offsets_ok || g_bridge == nullptr)
        return;

    // #67: the WeaponMerchant (Legovich) forge spawns the forged-weapon Pickup with loc_idx == the pending weapon index SaveSlot+0xc70. The mod suppresses that
    // grant, so vanilla OnPickupDone's `if(c70==loc) c70=-1` reset never runs and the merchant stays uninteractable. Cleared here, after g_orig_pickup_init
    // built the pickup, because clearing c70 before that spawn freezes with itemType 0.
    void *wsave = pal::active_save_slot();
    const int weapon_idx = wsave != nullptr ? pal::at<int>(wsave, mth::layout::kSaveWeaponIndexOff) : -1;
    if (weapon_idx >= 0 && loc_idx == weapon_idx)
    {
        pal::at<int>(wsave, mth::layout::kSaveWeaponIndexOff) = -1;
        pal::at<unsigned char>(wsave, mth::layout::kSaveWeaponMoldLatchOff) = 0;
        pal::logf(pal::LogLevel::Info, "legovich: forge pickup loc=%d -> reset weapon mold to re-arm merchant (#67)", loc_idx);
    }

    // Respawn suppression (fallback): already-checked location; tear down via QueueDestroy.
    if (mod::queue_destroy_available() && g_bridge->is_checked(loc_idx))
    {
        void *ent = pal::at<void *>(self, mth::layout::kComponentEntityOff);
        void *world = ent != nullptr ? pal::at<void *>(ent, mth::layout::kEntityWorldOff) : nullptr;
        if (ent != nullptr && world != nullptr)
        {
            pal::at<unsigned>(self, mth::layout::kPickupKilledFlagOff) |= 1u;
            mod::queue_destroy_entity(world, ent, false);
            pal::logf(pal::LogLevel::Info, "outbound: already-checked AP location locIdx=%d -> QueueDestroy", loc_idx);
        }
        return;
    }

    // #93: for a have-bit location (kinds 1/9/11) the game self-kills the box when IsItemCollected reports the item owned, which any AP grant of it does while
    // the location is unchecked. The IsItemCollected hook redirects that read, but MSVC inlines it into the Pickup ctor's self-kill (GCC keeps a real call, so
    // Linux is unaffected). The ctor arms the kill via Pickup+0x3ac, so clearing it skips the inlined check. Only for unchecked AP locations: a checked one
    // wants the vanilla self-kill, the only suppression left when QueueDestroy is unresolved.
    if (g_bridge->is_ap_location(loc_idx) && !g_bridge->is_checked(loc_idx) &&
        mth::tables::is_item_keyed_collected_kind(mth::tables::native_location_kind(loc_idx)))
        pal::at<unsigned char>(self, mth::layout::kPickupSaveTrackedFlagOff) = 0;

    // The dummy-itemType redirect waits for the PickupOnPickup hook. The ctor's surprise-spawn emerge block runs after Init and reads s_rItems[storedType].kind
    // to decide whether to detach the revealed pickup from the dying wall (SpawnPoint::DisownParent); the dummy's kind 0 would leave a hidden-room kear
    // attached and invisible until a room reload (#86). Init builds the sprite from the real itemType, so no visual changes. locIdx < 0 is an untracked pickup,
    // by far the commonest case, and is not logged.
    if (g_bridge->is_ap_location(loc_idx))
        pal::logf(pal::LogLevel::Debug, "Pickup::Init locIdx=%d itemType=%d -> AP location (dummy redirect deferred to collect)", loc_idx, item_type);
    else if (loc_idx >= 0)
        pal::logf(pal::LogLevel::Debug, "Pickup::Init locIdx=%d itemType=%d -> vanilla (location not in this seed)", loc_idx, item_type);
}

// Start of Pickup::OnPickup. The hook ctx carries the real Pickup*, so no per-platform subobject recovery is needed. The original runs afterward and re-reads
// the itemType live, so rewriting it here makes OnPickupDone see the dummy.
void on_pickup_collected(void *pickup)
{
    if (!g_pickup_offsets_ok || g_bridge == nullptr)
        return;
    const int loc_idx = pickup_loc_idx(pickup);
    if (loc_idx >= 0)
        pal::logf(pal::LogLevel::Debug, "Pickup::OnPickup locIdx=%d", loc_idx);
    if (!g_bridge->is_ap_location(loc_idx))
        return;
    pal::logf(pal::LogLevel::Info, "outbound: collected AP location locIdx=%d", loc_idx);
    collect_ap_location(loc_idx);
    // Deferred from Pickup::Init (#86); kind 0 means no vanilla grant.
    pickup_item_type(pickup) = mth::layout::kApDummyItemType;
}

// Shared shop-purchase AP logic; the PAL owns the per-platform hook and field reads. Returns the itemType to store: the dummy to suppress the vanilla grant
// where the platform redirects, else unchanged.
int on_shop_buy(int loc_idx, int item_type)
{
    if (!g_shop_offsets_ok || g_bridge == nullptr)
        return item_type;

    // Offset self-check: garbage reads mean the ShopMenu layout shifted, so the check is disabled.
    if (loc_idx < -1 || loc_idx >= mth::layout::kLocationCount || item_type < 0 || item_type >= mth::layout::kItemTypeCount)
    {
        g_shop_offsets_ok = false;
        pal::logf(pal::LogLevel::Error, "shop offset check FAILED (locIdx=%d itemType=%d); shop check disabled", loc_idx, item_type);
        return item_type;
    }
    if (g_bridge->is_ap_location(loc_idx))
    {
        pal::logf(pal::LogLevel::Info, "outbound: bought AP shop item locIdx=%d", loc_idx);
        collect_ap_location(loc_idx);
        // Linux suppresses the vanilla grant via this dummy itemType. Windows cannot redirect, so the OnPickupDone hook skips the grant for AP locations; the
        // real item arrives via AP inbound.
        return mth::layout::kApDummyItemType;
    }
    return item_type;
}

// ShopItem::Refresh level classifier. 0 = not an AP location, 1 = AP location not yet checked,
// 2 = AP location checked.
int on_shop_stock(int loc_idx)
{
    if (g_bridge == nullptr || !g_bridge->is_ap_location(loc_idx))
        return 0;
    return g_bridge->is_checked(loc_idx) ? 2 : 1;
}

// ShopItem::Refresh. The suppressed vanilla grant is what normally zeroes a slot's stock, so stock is replayed from AP state: a slot sells out only when every
// level is checked (#48). ShopMenu::SetupBoxes already advanced the active variant (ShopItem+0xf8) past bought levels, so advancing it again would skip a level
// per buy (#94). A slot with no AP-location levels is untouched.
void on_shop_item_refresh(void *shop_item)
{
    void *def = pal::at<void *>(shop_item, mth::layout::kShopItemDefOff);
    if (def == nullptr)
        return;

    void *first_unbought = nullptr;
    int remaining = 0; // AP levels not yet checked
    bool any_ap = false;
    for (void *v = def; v != nullptr; v = pal::at<void *>(v, mth::layout::kShopDefNextOff))
    {
        const int loc_idx = pal::at<int>(v, mth::layout::kShopDefLocOff);
        const int state = loc_idx >= 0 ? on_shop_stock(loc_idx) : 0;
        if (state != 0)
            any_ap = true;
        if (state != 2) // not a bought level
        {
            if (first_unbought == nullptr)
                first_unbought = v;
            if (state == 1)
                ++remaining;
        }
    }

    auto *stock = &pal::at<int>(shop_item, mth::layout::kShopItemStockOff);
    if (first_unbought == nullptr)
        *stock = 0; // all levels bought -> sold out
    else if (any_ap)
        *stock = remaining;
}

// Flattening is active while the AP bridge is attached (the LocationHooks lifetime, i.e. a connected session). Polled per Shop::Get.
bool on_shop_flatten()
{
    return g_bridge != nullptr;
}

// ShopMenu::SetCursor post-hook: scouts any unscouted AP box location, then rewrites the selected box's name and description from the scout map, leaving
// vanilla text otherwise. The enumerate sink is captureless because g_bridge/g_scout are file-scope globals.
void on_shop_set_cursor(void *shop_menu)
{
    if (g_bridge == nullptr || g_scout == nullptr)
        return;

    // Scout every box location missing from the scout map. One whose reply has not landed is re-requested on the next cursor move; the server dedups the scout
    // and hint, so retrying is cheap.
    std::vector<int> unknown;
    mth::shop_enumerate_locs(
        shop_menu,
        [](int loc, void *ctx)
        {
            auto *u = static_cast<std::vector<int> *>(ctx);
            if (loc >= 0 && g_bridge->is_ap_location(loc) && !g_scout->contains(loc))
                u->push_back(loc);
        },
        &unknown);
    if (!unknown.empty())
        g_bridge->request_scouts(unknown);

    const int loc = mth::shop_selected_loc(shop_menu);
    if (loc < 0)
        return;
    const mth::ScoutInfo *si = mth::find_scout(*g_scout, loc);
    if (si == nullptr)
        return; // not scouted yet / not AP -> leave vanilla text

    void *name_w = mth::shop_name_widget(shop_menu);
    void *desc_w = mth::shop_desc_widget(shop_menu);
    if (name_w != nullptr)
    {
        const std::uint32_t color = mth::banner_color("item_id", "", si->item_flags, 0, si->is_self);
        mod::set_text(name_w, si->item_name.c_str());
        // SetCursor rebinds the widget's palette on every cursor move, so this re-applies each time. SetColor goes last because it raises the dirty flag that
        // forces the re-resolve, and only after a successful swap, since an untouched palette resolves the color to entry 0.
        if (mth::shop_apply_name_palette(name_w, color))
            mod::set_text_color(name_w, color);
    }
    if (desc_w != nullptr)
        mod::set_text(desc_w, mth::format_scout_desc(*si).c_str());
}

// SaveSlot+0xc70: the WeaponMerchant's pending weapon-upgrade index (-1 = none / save unavailable).
[[nodiscard]] int current_weapon_index()
{
    void *slot = pal::active_save_slot();
    return slot != nullptr ? pal::at<int>(slot, mth::layout::kSaveWeaponIndexOff) : -1;
}

// Depth counter set while Shop::IsOutOfStock runs (game thread). Lets the IsItemCollected override tell the WeaponMerchant's stock tally from other readers of
// the same weapon locations, chiefly the weapon-swap chest.
int g_shop_oos_depth = 0;
std::uint64_t (*g_orig_shop_oos)(void *, void *) = nullptr;

std::uint64_t repl_shop_is_out_of_stock(void *shop_def, void *out_info)
{
    // SaveSlot::Upgrade reaches IsOutOfStock on the save thread, whose item queries skip the override anyway, so only the game thread moves the depth.
    if (!mod::on_game_thread())
        return g_orig_shop_oos != nullptr ? g_orig_shop_oos(shop_def, out_info) : 0;
    ++g_shop_oos_depth;
    const std::uint64_t r = g_orig_shop_oos != nullptr ? g_orig_shop_oos(shop_def, out_info) : 0;
    --g_shop_oos_depth;
    return r;
}

// Items::IsItemCollected override for AP locations whose vanilla collected state aliases item ownership, so the game reports them collected before the chest is
// opened: capacity-upgrade pieces (#8), whose bit is the mod's AP capacity counter (a boss then skips the rose reward), and have-item kinds 1/9/11 (#61), keyed
// on the item's type, so an out-of-order AP grant opens the chest with no check sent. Those report the AP checked state and everything else passes through
// (-1); cheap kind/itemType checks reject most hot-path queries first. `ownership_query` is IsItemCollected's b5: the weapon-swap chest reads weapon ownership
// with b5=true and must see the real have-item bit, or a weapon received from another player stays hidden. See should_redirect_collected_query.
int on_item_collected_query(int loc_idx, bool ownership_query)
{
    if (g_bridge == nullptr)
        return -1;
    // Legovich (#67): the WeaponMerchant's out-of-stock tally must count AP purchases, not weapons received from the multiworld, or Armand arms early. His
    // slots report AP checked state only under IsOutOfStock, so the weapon-swap chest falls through below. The slot being forged (SaveSlot+0xc70 == loc) is
    // excluded: vanilla's "+1 for pending" term already counts it, and it is AP-checked at buy-confirm, so counting it here would arm Armand after one
    // purchase.
    if (g_shop_oos_depth > 0 && mth::is_legovich_location(loc_idx) && g_bridge->is_ap_location(loc_idx))
        return (g_bridge->is_checked(loc_idx) && loc_idx != current_weapon_index()) ? 1 : 0;
    const bool is_cap = mth::tables::is_capacity_upgrade_location(loc_idx);
    const int kind = is_cap ? -1 : mth::tables::native_location_kind(loc_idx); // kind unused when capacity
    if (!mth::tables::should_redirect_collected_query(is_cap, kind, ownership_query))
        return -1; // not an aliasing location, or a weapon ownership query -> pass through
    if (!g_bridge->is_ap_location(loc_idx))
        return -1;
    return g_bridge->is_checked(loc_idx) ? 1 : 0;
}

} // namespace

namespace mth
{

LocationHooks::LocationHooks(RandoBridge &bridge, const std::map<int, ScoutInfo> *scout)
{
    g_bridge = &bridge;
    g_scout = scout;
    tables::resolve();
    tables::repurpose_dummy_item();

    if (!mod::queue_destroy_available())
        pal::logf(pal::LogLevel::Warn, "LocationHooks: WorldQueueDestroyEntity unavailable; checked AP pickups will respawn");
    if (!mod::set_item_collected_available() || !tables::collection_resolved())
        pal::logf(pal::LogLevel::Warn, "LocationHooks: ItemsSetItemCollected/s_rItemCollection unavailable; bitfield-kind reload suppression disabled");
    if (!mod::text_color_available() || !mod::palette_api_available())
        pal::logf(pal::LogLevel::Warn, "LocationHooks: text color/palette entries unavailable; shop item names stay vanilla");

    pickup_init_ = pal::ScopedHook(sym::pickup_init, &repl_pickup_init, &g_orig_pickup_init, "Pickup::Init");
    shop_oos_ = pal::ScopedHook(sym::shop_is_out_of_stock, &repl_shop_is_out_of_stock, &g_orig_shop_oos, "Shop::IsOutOfStock");
    pal::install_shop_purchase_hook(&on_shop_buy);
    install_shop_flatten_hook(&on_shop_flatten);
    install_shop_text_hook(&on_shop_set_cursor);
    mod::install_item_collected_hook(&on_item_collected_query);
    mod::install_pickup_on_pickup_hook(&on_pickup_collected);
    mod::install_shop_item_refresh_hook(&on_shop_item_refresh);
}

void LocationHooks::set_kear_gating(bool neutralize, bool suppress)
{
    g_kear_neutralize = neutralize;
    g_kear_suppress = suppress;
}

void LocationHooks::set_player_getter(std::function<void *()> get_player)
{
    g_kear_player_getter = std::move(get_player);
}

bool LocationHooks::credit_kear_key(void *player)
{
    return ::credit_kear_key(player);
}

void LocationHooks::reconcile_kear_keys()
{
    ::reconcile_kear_keys();
}

void LocationHooks::enforce_native_bits()
{
    enforce_checked_native_bits();
}

void LocationHooks::reset_native_bits()
{
    reset_native_bit_enforcement();
}

} // namespace mth
