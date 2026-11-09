#include "mod/mod_api.hpp"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <thread>
#include <utility>

#include "MinaModAPI.h"
#include "MinaModHooks.h"
#include "pal/pal_log.hpp"
#include "pal/pal_module.hpp"

// mod::IsItemCollectedCtx mirrors the game's hook context so the public header stays free of game headers; this checks it against upstream's struct.
static_assert(sizeof(mod::IsItemCollectedCtx) == sizeof(::IsItemCollectedCtx));
static_assert(offsetof(mod::IsItemCollectedCtx, collection) == offsetof(::IsItemCollectedCtx, collection));
static_assert(offsetof(mod::IsItemCollectedCtx, save_slot) == offsetof(::IsItemCollectedCtx, saveSlot));
static_assert(offsetof(mod::IsItemCollectedCtx, index) == offsetof(::IsItemCollectedCtx, index));
static_assert(offsetof(mod::IsItemCollectedCtx, include_pawn_shop) == offsetof(::IsItemCollectedCtx, includePawnShop));
static_assert(offsetof(mod::IsItemCollectedCtx, include_early_collected) == offsetof(::IsItemCollectedCtx, includeEarlyCollected));
static_assert(offsetof(mod::IsItemCollectedCtx, mod_handled) == offsetof(::IsItemCollectedCtx, modHandled));
static_assert(offsetof(mod::IsItemCollectedCtx, mod_ret_val) == offsetof(::IsItemCollectedCtx, modRetVal));

static_assert(sizeof(mod::Transform) == sizeof(MM_Transform));
static_assert(offsetof(mod::Transform, r) == offsetof(MM_Transform, r));
static_assert(offsetof(mod::Transform, s) == offsetof(MM_Transform, s));
static_assert(offsetof(mod::Transform, t) == offsetof(MM_Transform, t));

// The AABB wrappers memcpy MM_AABB as six floats, center.xyz then half-extent.xyz.
static_assert(sizeof(MM_AABB) == 6 * sizeof(float));
static_assert(offsetof(MM_AABB, center) == 0);
static_assert(offsetof(MM_AABB, extents) == 3 * sizeof(float));

namespace
{

constexpr int kSaveSlotCount = 10;

MinaModAPI *g_mod_api = nullptr;
std::thread::id g_game_thread;

// An entry read past the end of a shorter struct holds whatever static follows the API table, so a non-null pointer may still be garbage.
template <class Fn> bool usable(Fn fn)
{
    return fn != nullptr && pal::in_game_text(reinterpret_cast<const void *>(fn));
}

// Newest game build known to predate the 2026-08-05 appended API entries.
constexpr std::uint32_t kNewestRevisionWithoutAppendedApi = 148905;

// Test before reading any appended field. On older builds those fields lie past the struct over more relocated function pointers, which pass an address
// test, so the read itself must be avoided. Upstream did not bump MinaModAPI_Version for them, so the revision is the only signal.
bool appended_api_possible()
{
    return g_mod_api != nullptr && usable(g_mod_api->GetGameRevision) && g_mod_api->GetGameRevision() > kNewestRevisionWithoutAppendedApi;
}

// For an appended entry after appended_api_possible(). Unlike usable(), it fails closed when no text range is published.
template <class Fn> bool usable_appended(Fn fn)
{
    return fn != nullptr && pal::text_range_published() && pal::in_game_text(reinterpret_cast<const void *>(fn));
}

// True when the API table is set and the entry is callable; the pointer to member lets call sites name the entry once.
template <class Fn> bool has(Fn MinaModAPI::*entry)
{
    return g_mod_api != nullptr && usable(g_mod_api->*entry);
}

// Same for an appended entry; the revision gate runs first so the field is never read on a build whose struct ends before it.
template <class Fn> bool has_appended(Fn MinaModAPI::*entry)
{
    return appended_api_possible() && usable_appended(g_mod_api->*entry);
}

// Every named hook the mod installs. InstallHook returns a valid handle for any string and RunHooks ignores unknown hashes, so only firing proves a hook
// exists; each trampoline records it for unfired_hooks().
enum HookId
{
    kHookIsItemCollected,
    kHookWorldUpdate,
    kHookWorldDestroy,
    kHookItemsOnPickup,
    kHookItemsOnPickupDone,
    kHookPickupOnPickup,
    kHookShopItemRefresh,
    kHookChestConstruct,
    kHookWorldUpdateEnd,
    kHookKeyboardUpdate,
    kHookControllerUpdate,
    kHookFixedUpdate,
    kHookCount,
};

constexpr const char *kHookNames[kHookCount] = {
    "IsItemCollected", "WorldUpdate",    "WorldDestroy",   "ItemsOnPickup",    "ItemsOnPickupDone",  "PickupOnPickup",
    "ShopItemRefresh", "ChestConstruct", "WorldUpdateEnd", "ycKeyboardUpdate", "ycControllerUpdate", "FixedUpdate",
};

std::atomic<bool> g_hook_fired[kHookCount];
void *g_hook_handles[kHookCount];

void note_fired(HookId id)
{
    if (!g_hook_fired[id].exchange(true))
        pal::logf(pal::LogLevel::Info, "mod hook: \"%s\" fired on this build", kHookNames[id]);
}

// Null when the API is unusable. A non-null handle says nothing about whether the name dispatches.
void *install_named(HookId id, MM_HookCallback cb)
{
    if (!has(&MinaModAPI::InstallHook))
        return nullptr;
    g_hook_handles[id] = g_mod_api->InstallHook(kHookNames[id], 0, cb);
    return g_hook_handles[id];
}

// Log lines for one hook. A null unavailable skips the up-front API check, so a missing API logs the failed line; a null ok logs nothing on success.
struct InstallLogs
{
    const char *unavailable;
    const char *failed;
    const char *ok;
};

// Stores cb in slot before installing so a hook that fires at once finds it, and clears it again if
// the install fails.
template <class Fn> bool install_hook(HookId id, MM_HookCallback trampoline, Fn &slot, Fn cb, const InstallLogs &logs)
{
    if (logs.unavailable != nullptr && !has(&MinaModAPI::InstallHook))
    {
        pal::logf(pal::LogLevel::Warn, "%s", logs.unavailable);
        return false;
    }
    slot = cb;
    if (install_named(id, trampoline) == nullptr)
    {
        pal::logf(pal::LogLevel::Warn, "%s", logs.failed);
        slot = nullptr;
        return false;
    }
    if (logs.ok != nullptr)
        pal::logf(pal::LogLevel::Info, "%s", logs.ok);
    return true;
}

mod::ItemCollectedFn g_item_collected_cb = nullptr;

void is_item_collected_trampoline(void *pctx)
{
    note_fired(kHookIsItemCollected);
    auto *c = static_cast<mod::IsItemCollectedCtx *>(pctx);
    // SaveSlot::Upgrade asks from the save thread about the slot it is migrating. That slot's own bits are the right answer there, and the callback reads
    // game-thread-only state.
    if (g_item_collected_cb == nullptr || c == nullptr || c->index < 0 || !mod::on_game_thread())
        return;
    const int ov = g_item_collected_cb(c->index, c->include_early_collected); // -1 pass through, 0/1 force
    if (ov >= 0)
    {
        c->mod_handled = true;
        c->mod_ret_val = (ov != 0);
    }
}

mod::WorldUpdatePreFn g_world_update_cb = nullptr;

// The World hooks fire for every running World, so the callbacks get the ctx's World* to tell them apart. It is also the route to a menu world's scene
// graph, where PlayerGetWorld() is null.
void world_update_trampoline(void *pctx)
{
    note_fired(kHookWorldUpdate);
    auto *c = static_cast<WorldUpdateCtx *>(pctx);
    if (g_world_update_cb != nullptr)
        g_world_update_cb(c != nullptr ? c->world : nullptr, c != nullptr ? c->elapsed : 0.0f);
}

mod::WorldUpdateEndFn g_world_update_end_cb = nullptr;

// WorldUpdateEnd shares WorldUpdateCtx with WorldUpdate.
void world_update_end_trampoline(void *pctx)
{
    note_fired(kHookWorldUpdateEnd);
    auto *c = static_cast<WorldUpdateCtx *>(pctx);
    if (g_world_update_end_cb != nullptr)
        g_world_update_end_cb(c != nullptr ? c->world : nullptr);
}

mod::WorldDestroyFn g_world_destroy_cb = nullptr;

void world_destroy_trampoline(void *pctx)
{
    note_fired(kHookWorldDestroy);
    auto *c = static_cast<WorldDestroyCtx *>(pctx);
    if (g_world_destroy_cb != nullptr)
        g_world_destroy_cb(c != nullptr ? c->world : nullptr);
}

mod::ItemsOnPickupFn g_items_on_pickup_cb = nullptr;

void items_on_pickup_trampoline(void *pctx)
{
    note_fired(kHookItemsOnPickup);
    auto *c = static_cast<ItemsOnPickupCtx *>(pctx);
    if (g_items_on_pickup_cb == nullptr || c == nullptr || c->collectionIndex == nullptr || c->itemType == nullptr)
        return;
    void *player = (c->ppPlayer != nullptr) ? static_cast<void *>(*c->ppPlayer) : nullptr;
    if (g_items_on_pickup_cb(*c->collectionIndex, *c->itemType, player))
        c->modHandled = true;
}

mod::ItemsOnPickupDoneFn g_items_on_pickup_done_cb = nullptr;

void items_on_pickup_done_trampoline(void *pctx)
{
    note_fired(kHookItemsOnPickupDone);
    auto *c = static_cast<ItemsOnPickupDoneCtx *>(pctx);
    if (g_items_on_pickup_done_cb == nullptr || c == nullptr || c->collectionIndex == nullptr || c->itemType == nullptr)
        return;
    void *player = (c->ppPlayer != nullptr) ? static_cast<void *>(*c->ppPlayer) : nullptr;
    if (g_items_on_pickup_done_cb(*c->collectionIndex, *c->itemType, player))
        c->modHandled = true;
}

mod::PickupOnPickupFn g_pickup_on_pickup_cb = nullptr;

void pickup_on_pickup_trampoline(void *pctx)
{
    note_fired(kHookPickupOnPickup);
    auto *c = static_cast<PickupOnPickupCtx *>(pctx);
    // The ctx carries the real Pickup*, so no per-platform subobject fixup is needed here.
    if (g_pickup_on_pickup_cb != nullptr && c != nullptr && c->pickup != nullptr)
        g_pickup_on_pickup_cb(static_cast<void *>(c->pickup));
}

mod::ShopItemRefreshFn g_shop_item_refresh_cb = nullptr;

void shop_item_refresh_trampoline(void *pctx)
{
    note_fired(kHookShopItemRefresh);
    auto *c = static_cast<ShopItemRefreshCtx *>(pctx);
    if (g_shop_item_refresh_cb != nullptr && c != nullptr && c->shopItem != nullptr)
        g_shop_item_refresh_cb(static_cast<void *>(c->shopItem));
}

bool g_input_suppressed = false;

// keysDown is sized [(YC_KEY_COUNT+31)/32] and upstream publishes no YC_KEY_COUNT. The engine's pack and unpack loops cover keys 0..136 (YC_KEY_META, the
// highest in MinaModEnums.h), which ends in bit-word 4.
constexpr std::size_t kKeyWordCount = 5;

void keyboard_update_trampoline(void *pctx)
{
    note_fired(kHookKeyboardUpdate);
    auto *c = static_cast<ycKeyboardUpdateCtx *>(pctx);
    if (c == nullptr || !g_input_suppressed)
        return;
    if (c->keysDown != nullptr)
        std::memset(c->keysDown, 0, kKeyWordCount * sizeof(std::uint32_t));
    if (c->keysDownFirstFrame != nullptr)
        std::memset(c->keysDownFirstFrame, 0, kKeyWordCount * sizeof(std::uint32_t));
}

void controller_update_trampoline(void *pctx)
{
    note_fired(kHookControllerUpdate);
    auto *c = static_cast<ycControllerUpdateCtx *>(pctx);
    if (c == nullptr || !g_input_suppressed)
        return;
    // exists is left alone: clearing it would read as an unplugged pad and swap the on-screen prompts.
    if (c->buttonDown != nullptr)
        *c->buttonDown = 0;
    for (std::int16_t *axis : {c->leftStickX, c->leftStickY, c->rightStickX, c->rightStickY, c->triggerLeft, c->triggerRight})
    {
        if (axis != nullptr)
            *axis = 0;
    }
}

mod::ChestConstructFn g_chest_construct_cb = nullptr;

void chest_construct_trampoline(void *pctx)
{
    note_fired(kHookChestConstruct);
    auto *c = static_cast<ChestConstructCtx *>(pctx);
    // The ctx carries the real Chest*, so no per-platform subobject fixup is needed here.
    if (g_chest_construct_cb != nullptr && c != nullptr && c->chest != nullptr)
        g_chest_construct_cb(static_cast<void *>(c->chest));
}

// Mirrors MinaModHooks.h. Fires at the top of Game::FixedUpdate with the frame delta, which the detour cannot see since the function takes void.
struct FixedUpdateCtx
{
    float elapsed;
};

mod::FixedUpdateDeltaFn g_fixed_update_delta_cb = nullptr;

void fixed_update_trampoline(void *pctx)
{
    note_fired(kHookFixedUpdate);
    auto *c = static_cast<FixedUpdateCtx *>(pctx);
    if (g_fixed_update_delta_cb != nullptr && c != nullptr)
        g_fixed_update_delta_cb(c->elapsed);
}

} // namespace

namespace mod
{

MinaModAPI *set_api(MinaModAPI *api)
{
    MinaModAPI *const previous = g_mod_api;
    g_mod_api = api;
    g_game_thread = std::this_thread::get_id();
    return previous;
}

bool on_game_thread()
{
    return std::this_thread::get_id() == g_game_thread;
}

bool api_available()
{
    return has(&MinaModAPI::InstallHook);
}

bool api_version_matches()
{
    return g_mod_api != nullptr && g_mod_api->APIVersion == MinaModAPI_Version;
}

std::uint32_t expected_api_version()
{
    return static_cast<std::uint32_t>(MinaModAPI_Version);
}

std::uint32_t game_revision()
{
    return (has(&MinaModAPI::GetGameRevision)) ? g_mod_api->GetGameRevision() : 0;
}

int current_game_state()
{
    return (has(&MinaModAPI::GetCurrentGameState)) ? g_mod_api->GetCurrentGameState() : -1;
}

bool install_item_collected_hook(ItemCollectedFn query)
{
    return install_hook(kHookIsItemCollected, &is_item_collected_trampoline, g_item_collected_cb, query,
                        {"items: modding API unavailable; IsItemCollected override disabled",
                         "items: InstallHook(IsItemCollected) returned null; override disabled", "items: IsItemCollected override installed (modding hook)"});
}

bool install_world_update_hook(WorldUpdatePreFn on_pre)
{
    return install_hook(kHookWorldUpdate, &world_update_trampoline, g_world_update_cb, on_pre,
                        {"world: modding API unavailable; World::Update pre-hook disabled", "world: InstallHook(WorldUpdate) returned null; pre-hook disabled",
                         "world: World::Update pre-hook installed (modding hook)"});
}

bool install_world_update_end_hook(WorldUpdateEndFn on_end)
{
    return install_hook(kHookWorldUpdateEnd, &world_update_end_trampoline, g_world_update_end_cb, on_end,
                        {"world: modding API unavailable; World::Update post-hook disabled",
                         "world: InstallHook(WorldUpdateEnd) returned null; post-hook disabled", "world: World::Update post-hook installed (modding hook)"});
}

bool install_world_destroy_hook(WorldDestroyFn on_destroy)
{
    return install_hook(kHookWorldDestroy, &world_destroy_trampoline, g_world_destroy_cb, on_destroy,
                        {"world: modding API unavailable; World::Destroy hook disabled",
                         "world: InstallHook(WorldDestroy) returned null; teardown hook disabled", "world: World::Destroy hook installed (modding hook)"});
}

bool vial_api_available()
{
    return has(&MinaModAPI::PlayerGetMaxVials) && has(&MinaModAPI::PlayerSetMaxVials) && has(&MinaModAPI::PlayerGetVials) && has(&MinaModAPI::PlayerSetVials);
}

int player_max_vials()
{
    return vial_api_available() ? g_mod_api->PlayerGetMaxVials() : 0;
}

int player_vials()
{
    return vial_api_available() ? g_mod_api->PlayerGetVials() : 0;
}

void set_player_max_vials(int n)
{
    if (vial_api_available())
        g_mod_api->PlayerSetMaxVials(n);
}

void set_player_vials(int n)
{
    if (vial_api_available())
        g_mod_api->PlayerSetVials(n);
}

bool bones_api_available()
{
    return has(&MinaModAPI::PlayerGetBones) && has(&MinaModAPI::PlayerSetBones);
}

int player_bones()
{
    return bones_api_available() ? g_mod_api->PlayerGetBones() : 0;
}

void set_player_bones(int n)
{
    if (bones_api_available())
        g_mod_api->PlayerSetBones(n);
}

bool bosses_api_available()
{
    return has(&MinaModAPI::PlayerGetBossesDefeated);
}

std::uint64_t player_bosses_defeated()
{
    return bosses_api_available() ? g_mod_api->PlayerGetBossesDefeated() : 0;
}

bool player_position(float out[3])
{
    if (out == nullptr || !has(&MinaModAPI::PlayerGetWorld) || !has(&MinaModAPI::PlayerGetPos3))
        return false;
    // PlayerGetPos3 checks only the outer component pointer and faults or returns {0,0,0} when the entity level is unwired. PlayerGetWorld walks the
    // same chain checking both levels, so null here flags that case.
    if (g_mod_api->PlayerGetWorld() == nullptr)
        return false;
    const MM_Vec3 v = g_mod_api->PlayerGetPos3();
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    return true;
}

bool player_die()
{
    if (!has(&MinaModAPI::PlayerDie))
        return false;
    g_mod_api->PlayerDie();
    return true;
}

bool player_component_available()
{
    return has(&MinaModAPI::PlayerGetComponent);
}

void *player_component()
{
    return player_component_available() ? static_cast<void *>(g_mod_api->PlayerGetComponent()) : nullptr;
}

bool health_api_available()
{
    return has(&MinaModAPI::PlayerGetHealth);
}

float player_health()
{
    return health_api_available() ? g_mod_api->PlayerGetHealth() : 0.0f;
}

bool spark_api_available()
{
    return has(&MinaModAPI::PlayerGetSpark);
}

int player_spark()
{
    return spark_api_available() ? g_mod_api->PlayerGetSpark() : 0;
}

void *player_world()
{
    if (!has(&MinaModAPI::PlayerGetWorld))
        return nullptr;
    return static_cast<void *>(g_mod_api->PlayerGetWorld());
}

std::size_t world_entity_list(void *world, const char *list, void **out, std::size_t cap)
{
    if (world == nullptr || list == nullptr || !has_appended(&MinaModAPI::WorldGetEntityList))
        return 0;
    // out may be null with cap 0: that is the sizing call.
    return g_mod_api->WorldGetEntityList(static_cast<World *>(world), list, reinterpret_cast<GameComponent **>(out), cap);
}

bool entity_walk_api_available()
{
    return has(&MinaModAPI::WorldGetGameRootEntity) && has(&MinaModAPI::EntityGetChildren) && has(&MinaModAPI::ComponentIsa);
}

void *world_game_root_entity(void *world)
{
    if (world == nullptr || !entity_walk_api_available())
        return nullptr;
    return static_cast<void *>(g_mod_api->WorldGetGameRootEntity(static_cast<World *>(world)));
}

std::size_t entity_children(void *entity, void **out, std::size_t cap)
{
    if (entity == nullptr || !entity_walk_api_available())
        return 0;
    // out may be null with cap 0: that is the sizing call.
    return g_mod_api->EntityGetChildren(static_cast<ycEntity *>(entity), reinterpret_cast<ycComponent **>(out), cap);
}

bool component_isa(void *component, std::uint64_t type_id)
{
    if (component == nullptr || !entity_walk_api_available())
        return false;
    // Tail-jumps to the object's own virtual Isa (vtable+0x18) with no validation of its own, so the
    // caller has to have established that this is a real polymorphic game object first.
    const MM_Rtti rtti{type_id};
    return g_mod_api->ComponentIsa(static_cast<ycComponent *>(component), rtti);
}

bool weak_ptr_api_available()
{
    return has(&MinaModAPI::CreateWeakPtr) && has(&MinaModAPI::WeakPtrGet) && has(&MinaModAPI::DestroyWeakPtr);
}

void *weak_ptr_create(void *component_or_entity)
{
    if (component_or_entity == nullptr || !weak_ptr_api_available())
        return nullptr;
    return static_cast<void *>(g_mod_api->CreateWeakPtr(component_or_entity));
}

void *weak_ptr_get(void *weak)
{
    if (weak == nullptr || !weak_ptr_api_available())
        return nullptr;
    return g_mod_api->WeakPtrGet(static_cast<MM_WeakPtr *>(weak));
}

void weak_ptr_destroy(void *weak)
{
    if (weak != nullptr && weak_ptr_api_available())
        g_mod_api->DestroyWeakPtr(static_cast<MM_WeakPtr *>(weak));
}

bool world_is_paused()
{
    if (!has(&MinaModAPI::PlayerGetWorld) || !has(&MinaModAPI::WorldIsPaused))
        return false;
    void *w = player_world(); // null with no live player; WorldIsPaused would dereference it
    return w != nullptr && g_mod_api->WorldIsPaused(static_cast<World *>(w));
}

bool room_time_api_available()
{
    return has(&MinaModAPI::GetRoomTime);
}

float room_time()
{
    return room_time_api_available() ? g_mod_api->GetRoomTime() : 0.0f;
}

bool save_api_available()
{
    // Only what the takeover calls; requiring more would fail a launch that works.
    return has(&MinaModAPI::SetSaveSlotContents) && has(&MinaModAPI::GetActiveSaveSlotContents) && has(&MinaModAPI::Free) &&
           has(&MinaModAPI::SetSaveWriteEnabled) && has(&MinaModAPI::IsSaveWriteEnabled);
}

int active_save_slot()
{
    if (!has(&MinaModAPI::GetActiveSaveSlot))
        return -1;
    const int slot = g_mod_api->GetActiveSaveSlot();
    // The game's no-slot value is one past the last slot.
    return (slot >= 0 && slot < kSaveSlotCount) ? slot : -1;
}

bool set_active_save_slot_contents(const char *ycdata)
{
    if (ycdata == nullptr || !has(&MinaModAPI::SetActiveSaveSlotContents))
        return false;
    return g_mod_api->SetActiveSaveSlotContents(ycdata);
}

bool set_save_slot_contents(unsigned int slot, const char *ycdata)
{
    if (ycdata == nullptr || !has(&MinaModAPI::SetSaveSlotContents))
        return false;
    return g_mod_api->SetSaveSlotContents(slot, ycdata);
}

std::string active_save_slot_contents()
{
    if (!has(&MinaModAPI::GetActiveSaveSlotContents))
        return {};
    char *raw = g_mod_api->GetActiveSaveSlotContents();
    if (raw == nullptr)
        return {};
    std::string out(raw);
    if (has(&MinaModAPI::Free))
        g_mod_api->Free(raw); // the API contract requires freeing through the game's allocator
    return out;
}

void player_restore_from_save()
{
    if (has(&MinaModAPI::PlayerRestoreFromSave))
        g_mod_api->PlayerRestoreFromSave();
}

bool install_items_on_pickup_hook(ItemsOnPickupFn on_pickup)
{
    return install_hook(kHookItemsOnPickup, &items_on_pickup_trampoline, g_items_on_pickup_cb, on_pickup,
                        {nullptr, "items: InstallHook(ItemsOnPickup) failed; armor-upgrade suppression disabled", nullptr});
}

bool install_items_on_pickup_done_hook(ItemsOnPickupDoneFn on_done)
{
    return install_hook(kHookItemsOnPickupDone, &items_on_pickup_done_trampoline, g_items_on_pickup_done_cb, on_done,
                        {nullptr, "items: InstallHook(ItemsOnPickupDone) failed; grant suppression disabled", nullptr});
}

bool install_pickup_on_pickup_hook(PickupOnPickupFn on_pickup)
{
    return install_hook(kHookPickupOnPickup, &pickup_on_pickup_trampoline, g_pickup_on_pickup_cb, on_pickup,
                        {nullptr, "locations: InstallHook(PickupOnPickup) failed; pickup redirect disabled", nullptr});
}

bool install_shop_item_refresh_hook(ShopItemRefreshFn on_refresh)
{
    return install_hook(kHookShopItemRefresh, &shop_item_refresh_trampoline, g_shop_item_refresh_cb, on_refresh,
                        {nullptr, "shop: InstallHook(ShopItemRefresh) failed; stock suppression disabled", nullptr});
}

bool install_chest_construct_hook(ChestConstructFn on_construct)
{
    return install_hook(kHookChestConstruct, &chest_construct_trampoline, g_chest_construct_cb, on_construct,
                        {nullptr, "chest: InstallHook(ChestConstruct) failed; kear-lock clearing disabled", nullptr});
}

bool set_fixed_update_delta_hook(FixedUpdateDeltaFn cb)
{
    g_fixed_update_delta_cb = cb;
    if (install_named(kHookFixedUpdate, &fixed_update_trampoline) == nullptr)
    {
        pal::logf(pal::LogLevel::Warn, "mod hook: FixedUpdate unavailable; frame delta falls back to a fixed tick estimate");
        return false;
    }
    return true;
}

bool install_input_suppress_hooks()
{
    const bool keyboard = install_named(kHookKeyboardUpdate, &keyboard_update_trampoline) != nullptr;
    const bool controller = install_named(kHookControllerUpdate, &controller_update_trampoline) != nullptr;
    if (!keyboard || !controller)
        pal::logf(pal::LogLevel::Warn, "input: InstallHook(keyboard=%d controller=%d) failed; input cannot be swallowed", keyboard, controller);
    return keyboard && controller;
}

void set_input_suppressed(bool suppressed)
{
    g_input_suppressed = suppressed;
}

bool input_suppress_hooks_fired()
{
    return g_hook_fired[kHookKeyboardUpdate].load() && g_hook_fired[kHookControllerUpdate].load();
}

void remove_all_hooks()
{
    g_input_suppressed = false;
    g_items_on_pickup_cb = nullptr;
    g_items_on_pickup_done_cb = nullptr;
    g_pickup_on_pickup_cb = nullptr;
    g_shop_item_refresh_cb = nullptr;
    g_chest_construct_cb = nullptr;
    if (!has(&MinaModAPI::RemoveHook))
        return;
    for (void *&h : g_hook_handles)
    {
        if (h != nullptr)
            g_mod_api->RemoveHook(h);
        h = nullptr;
    }
}

std::vector<const char *> unfired_hooks()
{
    std::vector<const char *> out;
    for (int i = 0; i < kHookCount; ++i)
        if (g_hook_handles[i] != nullptr && !g_hook_fired[i].load())
            out.push_back(kHookNames[i]);
    return out;
}

bool queue_destroy_available()
{
    return has_appended(&MinaModAPI::WorldQueueDestroyEntity);
}

bool queue_destroy_entity(void *world, void *entity, bool depth_first)
{
    if (world == nullptr || entity == nullptr || !queue_destroy_available())
        return false;
    g_mod_api->WorldQueueDestroyEntity(static_cast<World *>(world), static_cast<ycEntity *>(entity), depth_first);
    return true;
}

bool set_item_collected_available()
{
    return has_appended(&MinaModAPI::ItemsSetItemCollected);
}

bool set_item_collected(int index, bool collected, void *collection, void *slot)
{
    if (index < 0 || !set_item_collected_available())
        return false;
    g_mod_api->ItemsSetItemCollected(index, collected, static_cast<ItemCollection *>(collection), static_cast<SaveSlot *>(slot));
    return true;
}

bool text_color_available()
{
    return has_appended(&MinaModAPI::TextComponentSetColor);
}

namespace
{

MM_Color to_mm_color(std::uint32_t rgba)
{
    MM_Color c{};
    c.r = static_cast<std::uint8_t>(rgba & 0xFFu);
    c.g = static_cast<std::uint8_t>((rgba >> 8) & 0xFFu);
    c.b = static_cast<std::uint8_t>((rgba >> 16) & 0xFFu);
    c.a = static_cast<std::uint8_t>((rgba >> 24) & 0xFFu);
    return c;
}

} // namespace

bool set_text_color(void *text_component, std::uint32_t rgba)
{
    if (text_component == nullptr || !text_color_available())
        return false;
    // MM_Color is four uint8 channels in r,g,b,a memory order, byte-identical to the packed word on little-endian.
    g_mod_api->TextComponentSetColor(static_cast<ycComponent *>(text_component), to_mm_color(rgba));
    return true;
}

bool palette_api_available()
{
    return has_appended(&MinaModAPI::ClonePalette) && has_appended(&MinaModAPI::PaletteSetGroup) && has_appended(&MinaModAPI::PaletteWriteIndex) &&
           has_appended(&MinaModAPI::PaletteGetIndex) && has_appended(&MinaModAPI::PaletteGetWidth);
}

void *clone_palette(void *source)
{
    if (source == nullptr || !palette_api_available())
        return nullptr;
    return g_mod_api->ClonePalette(static_cast<ycPaletteTexture *>(source));
}

void palette_set_group(void *palette, std::int32_t group)
{
    if (palette == nullptr || !palette_api_available())
        return;
    g_mod_api->PaletteSetGroup(static_cast<ycPaletteTexture *>(palette), group);
}

void palette_write_index(void *palette, std::int32_t index, std::uint32_t rgba)
{
    if (palette == nullptr || !palette_api_available())
        return;
    g_mod_api->PaletteWriteIndex(static_cast<ycPaletteTexture *>(palette), index, to_mm_color(rgba));
}

std::int32_t palette_get_index(void *palette, std::uint32_t rgba)
{
    if (palette == nullptr || !palette_api_available())
        return -1;
    return g_mod_api->PaletteGetIndex(static_cast<ycPaletteTexture *>(palette), to_mm_color(rgba));
}

std::uint32_t palette_get_width(void *palette)
{
    if (palette == nullptr || !palette_api_available())
        return 0;
    return g_mod_api->PaletteGetWidth(static_cast<ycPaletteTexture *>(palette));
}

bool set_text(void *text_component, const char *utf8)
{
    if (text_component == nullptr || utf8 == nullptr || !has_appended(&MinaModAPI::TextComponentSetText))
        return false;
    g_mod_api->TextComponentSetText(static_cast<ycComponent *>(text_component), utf8);
    return true;
}

const char *text_of(void *text_component)
{
    if (text_component == nullptr || !has_appended(&MinaModAPI::TextComponentGetText))
        return nullptr;
    return g_mod_api->TextComponentGetText(static_cast<ycComponent *>(text_component));
}

bool water_api_available()
{
    return has_appended(&MinaModAPI::WaterListenerIsInDeepWater);
}

bool water_is_in_deep_water(void *water_listener, bool ignore_enabled)
{
    if (water_listener == nullptr || !water_api_available())
        return false;
    return g_mod_api->WaterListenerIsInDeepWater(static_cast<WaterListener *>(water_listener), ignore_enabled);
}

bool physics_get_aabb(void *physics_component, float out_aabb[6], bool local, unsigned shape_flags)
{
    if (physics_component == nullptr || out_aabb == nullptr || !has_appended(&MinaModAPI::PhysicsComponentGetAABB))
        return false;
    MM_AABB box{};
    g_mod_api->PhysicsComponentGetAABB(static_cast<PhysicsComponent *>(physics_component), &box, local, shape_flags);
    std::memcpy(out_aabb, &box, sizeof(box));
    return true;
}

void *closest_carryable(void *carry_manager, const float box[6], int collision_layer, float max_dist, int *out_overlap_count, std::uint64_t collide_mask_ignore)
{
    if (carry_manager == nullptr || box == nullptr || !has_appended(&MinaModAPI::CarryManagerGetClosestCarryableObject))
        return nullptr;
    MM_AABB b{};
    std::memcpy(&b, box, sizeof(b));
    return g_mod_api->CarryManagerGetClosestCarryableObject(static_cast<CarryManager *>(carry_manager), b, collision_layer, max_dist, out_overlap_count,
                                                            collide_mask_ignore);
}

bool player_stats_api_available()
{
    return has_appended(&MinaModAPI::PlayerUpdateStats);
}

bool player_update_stats()
{
    if (!player_stats_api_available())
        return false;
    g_mod_api->PlayerUpdateStats();
    return true;
}

void *sym_addr(const char *name)
{
    if (name == nullptr || !has_appended(&MinaModAPI::GetSymAddr))
        return nullptr;
    // Not range-checked: the supported names include data symbols that do not live in .text.
    return g_mod_api->GetSymAddr(name);
}

int room_index()
{
    // The field is signed and negative means "no room bound"; the API returns it as uint32_t.
    return has(&MinaModAPI::GetRoomIndex) ? static_cast<int>(static_cast<std::int32_t>(g_mod_api->GetRoomIndex())) : -1;
}

bool world_menu_root_api_available()
{
    return has(&MinaModAPI::WorldGetMenuRootEntity);
}

void *world_menu_root_entity(void *world)
{
    if (world == nullptr || !has(&MinaModAPI::WorldGetMenuRootEntity))
        return nullptr;
    return g_mod_api->WorldGetMenuRootEntity(static_cast<World *>(world));
}

bool cheat_manager_activate_save_cheats()
{
    if (!has_appended(&MinaModAPI::CheatManagerActivateSaveCheats))
        return false;
    g_mod_api->CheatManagerActivateSaveCheats();
    return true;
}

bool cheat_manager_set_cheat_applied(int cheat, bool active)
{
    if (cheat < 0 || cheat > 0xfd)
        return false;
    if (!has_appended(&MinaModAPI::CheatManagerSetCheatApplied))
        return false;
    g_mod_api->CheatManagerSetCheatApplied(cheat, active, static_cast<std::uint32_t>(-1));
    return true;
}

bool cheat_manager_is_cheat_applied(int cheat)
{
    if (cheat < 0 || cheat > 0xfd)
        return false;
    if (!has_appended(&MinaModAPI::CheatManagerIsCheatApplied))
        return false;
    return g_mod_api->CheatManagerIsCheatApplied(cheat, static_cast<std::uint32_t>(-1));
}

void *spawn_entity(void *parent, std::uint32_t type, const float pos[3], std::int32_t collision_layer)
{
    if (parent == nullptr || pos == nullptr || !has(&MinaModAPI::SpawnEntity2))
        return nullptr;
    const MM_Vec3 at{pos[0], pos[1], pos[2]};
    const MM_Vec3 scale{1.0f, 1.0f, 1.0f};
    return g_mod_api->SpawnEntity2(static_cast<ycEntity *>(parent), type, at, scale, 0.0f, collision_layer, nullptr, nullptr, nullptr);
}

void *component_parent(void *component)
{
    if (component == nullptr || !has(&MinaModAPI::ComponentGetParent))
        return nullptr;
    return g_mod_api->ComponentGetParent(static_cast<ycComponent *>(component));
}

bool entity_world_transform(void *entity, Transform &out)
{
    if (entity == nullptr || !has(&MinaModAPI::EntityGetWorldTransform))
        return false;
    const MM_Transform t = g_mod_api->EntityGetWorldTransform(static_cast<ycEntity *>(entity));
    std::memcpy(&out, &t, sizeof(out));
    return true;
}

void set_entity_local_transform(void *entity, const Transform &transform)
{
    if (entity != nullptr && has(&MinaModAPI::EntitySetLocalTransform))
        g_mod_api->EntitySetLocalTransform(static_cast<ycEntity *>(entity), reinterpret_cast<const MM_Transform *>(&transform));
}

bool spawn_anim_api_available()
{
    return has(&MinaModAPI::SpawnEntity2) && has(&MinaModAPI::ComponentGetParent) && has(&MinaModAPI::EntityGetWorldTransform) &&
           has(&MinaModAPI::EntitySetLocalTransform) && has(&MinaModAPI::GameAnimPlay) && has(&MinaModAPI::GameAnimGetSeqName) &&
           has(&MinaModAPI::GameAnimGetSeqFrameIdx) && has_appended(&MinaModAPI::GameAnimGetNumSeqFrames) &&
           has_appended(&MinaModAPI::GameAnimSetSeqFrameIdx) && has_appended(&MinaModAPI::GameAnimIsVisible) && has_appended(&MinaModAPI::GameAnimSetVisible) &&
           has_appended(&MinaModAPI::GameAnimSetMulColor) && has_appended(&MinaModAPI::GameAnimGetLocalTransform) &&
           has_appended(&MinaModAPI::GameAnimSetLocalTransform);
}

void game_anim_play(void *anim, const char *seq, std::int32_t loops, float speed, bool force)
{
    if (anim != nullptr && seq != nullptr && has(&MinaModAPI::GameAnimPlay))
        g_mod_api->GameAnimPlay(static_cast<ycComponent *>(anim), seq, loops, speed, force);
}

std::string_view game_anim_seq_name(void *anim)
{
    if (game_anim_num_seq_frames(anim) == 0 || !has(&MinaModAPI::GameAnimGetSeqName))
        return {};
    const MM_StringRef name = g_mod_api->GameAnimGetSeqName(static_cast<ycComponent *>(anim));
    return name.str != nullptr ? std::string_view(name.str, name.len) : std::string_view{};
}

std::uint32_t game_anim_num_seq_frames(void *anim)
{
    if (anim == nullptr || !has_appended(&MinaModAPI::GameAnimGetNumSeqFrames))
        return 0;
    return g_mod_api->GameAnimGetNumSeqFrames(static_cast<ycComponent *>(anim));
}

std::uint32_t game_anim_seq_frame_idx(void *anim)
{
    if (anim == nullptr || !has(&MinaModAPI::GameAnimGetSeqFrameIdx))
        return 0;
    return g_mod_api->GameAnimGetSeqFrameIdx(static_cast<ycComponent *>(anim));
}

void game_anim_set_seq_frame_idx(void *anim, std::uint32_t idx)
{
    if (anim != nullptr && has_appended(&MinaModAPI::GameAnimSetSeqFrameIdx))
        g_mod_api->GameAnimSetSeqFrameIdx(static_cast<ycComponent *>(anim), idx);
}

bool game_anim_is_visible(void *anim)
{
    return anim != nullptr && has_appended(&MinaModAPI::GameAnimIsVisible) && g_mod_api->GameAnimIsVisible(static_cast<ycComponent *>(anim));
}

void game_anim_set_visible(void *anim, bool visible)
{
    if (anim != nullptr && has_appended(&MinaModAPI::GameAnimSetVisible))
        g_mod_api->GameAnimSetVisible(static_cast<ycComponent *>(anim), visible);
}

void game_anim_set_mul_color(void *anim, std::uint32_t rgba)
{
    if (anim != nullptr && has_appended(&MinaModAPI::GameAnimSetMulColor))
        g_mod_api->GameAnimSetMulColor(static_cast<ycComponent *>(anim), to_mm_color(rgba));
}

bool game_anim_local_transform(void *anim, Transform &out)
{
    if (anim == nullptr || !has_appended(&MinaModAPI::GameAnimGetLocalTransform))
        return false;
    g_mod_api->GameAnimGetLocalTransform(static_cast<ycComponent *>(anim), reinterpret_cast<MM_Transform *>(&out));
    return true;
}

void set_game_anim_local_transform(void *anim, const Transform &transform)
{
    if (anim != nullptr && has_appended(&MinaModAPI::GameAnimSetLocalTransform))
        g_mod_api->GameAnimSetLocalTransform(static_cast<ycComponent *>(anim), reinterpret_cast<const MM_Transform *>(&transform));
}

void set_save_write_enabled(bool on)
{
    if (has(&MinaModAPI::SetSaveWriteEnabled))
        g_mod_api->SetSaveWriteEnabled(on);
}

bool save_write_enabled()
{
    return (has(&MinaModAPI::IsSaveWriteEnabled)) ? g_mod_api->IsSaveWriteEnabled() : false;
}

} // namespace mod
