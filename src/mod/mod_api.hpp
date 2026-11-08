#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct MinaModAPI; // MinaModAPI.h is included only in mod_api.cpp

namespace mod
{

// Store the live MinaModAPI* (from MinaMod_Init) or a fake (in tests). nullptr clears it. Returns the pointer it replaced. The loader calls MinaMod_Init
// on the game thread, so the calling thread is recorded as the one on_game_thread() matches.
MinaModAPI *set_api(MinaModAPI *api);
[[nodiscard]] bool on_game_thread();

// False when the game's mod API is unusable, which leaves every named mod hook inert.
[[nodiscard]] bool api_available();

// False when the game's API struct version differs from the one this mod was built against, so field offsets may not line up. The gate only reports it.
[[nodiscard]] bool api_version_matches();
[[nodiscard]] std::uint32_t expected_api_version();

// Game revision ("r-number") via MinaModAPI::GetGameRevision; 0 if unavailable.
std::uint32_t game_revision();

// Current game-state enum value via MinaModAPI::GetCurrentGameState; -1 if unavailable.
int current_game_state();

// Items::IsItemCollected override via the "IsItemCollected" hook, which also fires from inlined copies. Capacity-upgrade locations (itemTypes 0x44..0x48)
// share the SaveSlot bitfield apply_upgrades uses as a counter, so a vanilla query reports them collected and boss rose-reward spawns skip (#8).
// query returns -1 to pass through or 0/1 to force the result. ownership_query is param5, true when the weapon-swap chest asks whether an item is owned
// and false for location queries; the mth side needs it to keep a received weapon visible to that chest. query runs only on the game thread: the save
// thread also asks while SaveSlot::Upgrade migrates an old save, and gets the vanilla answer.
using ItemCollectedFn = int (*)(int loc_idx, bool ownership_query);
bool install_item_collected_hook(ItemCollectedFn query);

// "WorldUpdate" hook at the top of World::Update, the spawn window where grants and lock seeds must run to avoid update-queue hangs. It fires once for
// each running World, including the popup, dialogue and overlay Worlds, so callers filter on world. on_pre runs on the game thread; install returns false
// without the modding API.
using WorldUpdatePreFn = void (*)(void *world);
bool install_world_update_hook(WorldUpdatePreFn on_pre);

// "WorldUpdateEnd" hook after the other systems have run. The ctx's World* is the only route to a menu world's scene graph, since PlayerGetWorld() is null
// without a player. on_end runs on the game thread and world may be null; install returns false without the modding API.
using WorldUpdateEndFn = void (*)(void *world);
bool install_world_update_end_hook(WorldUpdateEndFn on_end);

// "WorldDestroy" hook (a gamestate change, an overlay ending, a system prompt closing, shutdown): drop cached per-world pointers such as the live
// Player* here before the game frees that World. on_destroy runs on the game thread; install returns false without the modding API.
using WorldDestroyFn = void (*)(void *world);
bool install_world_destroy_hook(WorldDestroyFn on_destroy);

// Vial (health-flask) capacity and count through the native Player accessors, which find the live player and slot themselves. Capacity writes set the
// vial bitfield UpdateStats re-reads, so they stick. No-op until a player exists; available() is false when the build lacks the vial setters.
bool vial_api_available();
int player_max_vials();
int player_vials();
void set_player_max_vials(int n);
void set_player_vials(int n);

// Bone (money) count through the native Player accessors; the wallet cap (#112) clamps it each frame. No-op until a player exists; available() is false
// when the build lacks the bone setters.
bool bones_api_available();
int player_bones();
void set_player_bones(int n);

// Boss-defeated bitfield of the game's active slot, so a slot must already be bound. 0 when unavailable.
[[nodiscard]] bool bosses_api_available();
[[nodiscard]] std::uint64_t player_bosses_defeated();

// Raw player position; the trackable is the camera target, with shake, a lerp and a nose offset. False without the API; {0,0,0} with no live player.
[[nodiscard]] bool player_position(float out[3]);

// ---- Save slots. All no-op or return a failure sentinel without the modding API. ----

// True when every entry the save takeover depends on is present.
bool save_api_available();
int active_save_slot(); // -1 when unavailable or no slot is bound (the game reports 10, not -1)
bool set_active_save_slot_contents(const char *ycdata);
bool set_save_slot_contents(unsigned int slot, const char *ycdata); // writes a slot without activating it
std::string active_save_slot_contents();                            // "" on failure
void player_restore_from_save();
void set_save_write_enabled(bool on);
bool save_write_enabled();

// ---- Named hooks. They fire on both platforms and from every inlined copy of the target.
//
// InstallHook returns a valid handle for any name, so only unfired_hooks() shows whether a build dispatches one.

// Items::OnPickup. Armor upgrades (0x4f/0x50) apply their effect here, before OnPickupDone, so they are suppressed here (#71). True skips the original.
using ItemsOnPickupFn = bool (*)(int slot, int item_type, void *player);
bool install_items_on_pickup_hook(ItemsOnPickupFn on_pickup);

// Items::OnPickupDone: suppresses the vanilla grant for an AP location. Inbound grants still need the function's address, which a hook cannot provide.
using ItemsOnPickupDoneFn = bool (*)(int slot, int item_type, void *player);
bool install_items_on_pickup_done_hook(ItemsOnPickupDoneFn on_done);

// Pickup::OnPickup with the real Pickup*; a detour on MSVC receives the PickupListener base.
using PickupOnPickupFn = void (*)(void *pickup);
bool install_pickup_on_pickup_hook(PickupOnPickupFn on_pickup);

// ShopItem::Refresh: rewrites the slot's stock count so checked AP locations read as sold out.
using ShopItemRefreshFn = void (*)(void *shop_item);
bool install_shop_item_refresh_hook(ShopItemRefreshFn on_refresh);

// Chest ctor tail with the real Chest*. It is the only notice that a chest exists, so the kear-lock clear keeps a registry of these.
using ChestConstructFn = void (*)(void *chest);
bool install_chest_construct_hook(ChestConstructFn on_construct);

// Engine frame delta from the top of Game::FixedUpdate. The mod also detours that function for its post-original tick, since the hook fires before the
// original and the detour cannot see the delta (the function takes void). True only means InstallHook took the name; watch the callback for dispatch.
using FixedUpdateDeltaFn = void (*)(float elapsed);
bool set_fixed_update_delta_hook(FixedUpdateDeltaFn cb);

// Zeroes keyboard and controller state while set_input_suppressed(true) holds, via the "ycKeyboardUpdate" and "ycControllerUpdate" hooks. Both fire
// between gathering raw input and committing it, so menus and gameplay see the zeroed state. Mouse is left alone: no menu confirm uses it, and zeroing
// it needs YC_TOUCH_COUNT, which upstream does not publish. input_suppress_hooks_fired() shows dispatch.
bool install_input_suppress_hooks();

// Game thread only: ycGameThread::ThreadMain dispatches both hooks right after Game::FixedUpdate.
void set_input_suppressed(bool suppressed);

// Whether both input hooks have dispatched. They run every input frame, so false past startup means the build does not know the names.
bool input_suppress_hooks_fired();

// Drops every named hook above and its callback. Tests use it to reset between cases; App is leaked and never unhooks.
void remove_all_hooks();

// Installed hook names that have never fired.
std::vector<const char *> unfired_hooks();

// Entity teardown through the guarded ycWorld::QueueDestroy. Idempotent: a repeat call stops at the entity's already-queued flag.
bool queue_destroy_available();
bool queue_destroy_entity(void *world, void *entity, bool depth_first);

// Durable collected-bit write. Null collection/slot means the game's own active ones.
bool set_item_collected_available();
bool set_item_collected(int index, bool collected, void *collection, void *slot);

// rgba is packed like mth::banner_color (r in the low byte), the same byte order as MM_Color.
bool text_color_available();
bool set_text_color(void *text_component, std::uint32_t rgba);

// ycPaletteTexture. Text is palette-mapped (see mth::layout kText*PaletteOff), so an arbitrary color renders as entry 0 unless written into a cloned
// palette. Detach the clone with group -1 or it keeps resolving through the shared group chain. rgba is packed like mth::banner_color.
bool palette_api_available();
void *clone_palette(void *source);
void palette_set_group(void *palette, std::int32_t group);
void palette_write_index(void *palette, std::int32_t index, std::uint32_t rgba);
std::int32_t palette_get_index(void *palette, std::uint32_t rgba);
std::uint32_t palette_get_width(void *palette);

// Text-widget contents; the widget pointer the menus hand over is the ycTextComponent. text_of() returns null when the string is unreadable, which means
// "no original to restore".
bool set_text(void *text_component, const char *utf8);
const char *text_of(void *text_component);

// Swim-vs-land check off a player's WaterListener; ignore_enabled skips the listener's enable flag. available() is false when the build lacks the getter,
// which callers must tell apart from "not in deep water".
bool water_api_available();
bool water_is_in_deep_water(void *water_listener, bool ignore_enabled);

// Read-only halves of the game's carryable-grab query. The box is six floats, center.xyz then half-extent.xyz, matching the game's AABB; the API evens out
// the per-platform argument order. physics_get_aabb leaves out_aabb untouched on failure. closest_carryable returns null both for "nothing in range" and
// for a build without the entry.
bool physics_get_aabb(void *physics_component, float out_aabb[6], bool local, unsigned shape_flags);
void *closest_carryable(void *carry_manager, const float box[6], int collision_layer, float max_dist, int *out_overlap_count,
                        std::uint64_t collide_mask_ignore);

// Recomputes the live player's maxima (HP, magic, spark, vials, trinkets) from the save's owned-bit fields. Check available() before writing those fields,
// since they do nothing until this runs.
bool player_stats_api_available();
bool player_update_stats();

// Address of a game symbol by its plain source-level name. Null when the API does not expose that
// name, which leaves the caller on its own resolver. May return data rather than code.
void *sym_addr(const char *name);

// Live room index off the game's RoomManager global. Negative when no room is bound or the entry is unavailable.
int room_index();

// Menu-world scene root. Null when unavailable or world is null.
bool world_menu_root_api_available();
void *world_menu_root_entity(void *world);

// Rebuilds the runtime cheat mirror from the active slot's mask; the native entry resolves g_cheatManager itself. False when the entry is absent.
bool cheat_manager_activate_save_cheats();

// Sets or clears one modifier bit on the active save slot. The entry inlines the write, so it skips the mod's CheatManager::SetCheatApplied lockdown
// detour. Refused outside 0..0xfd: >= 0xfe diverts into ApplyBackerCheats and a negative index no-ops game-side. The entry does not null-check the slot,
// so one must be bound. False when unavailable.
bool cheat_manager_set_cheat_applied(int cheat, bool active);

// Whether the active save has that modifier set in its own mask. Also false on older builds without the entry, so a trap then arms without that check.
bool cheat_manager_is_cheat_applied(int cheat);

// Kills the player via the native PlayerDie (deathlink apply). False when the modding API or PlayerDie is unavailable.
bool player_die();

// The game's live-Player pointer, set when the Player is built and nulled in Player::~Player. A Player* captured in the ctor detour outlives the free and
// faults on the next write (#157). Null when there is no live player; callers skip the tick then.
bool player_component_available();
void *player_component();

// Player health. Deathlink re-arms its broadcast only on a respawn (health > 0), since the death-guard byte pulses during the death sequence. available()
// is false when the build lacks the getter; player_health() then returns 0.
bool health_api_available();
float player_health();

// Current Spark (the sub-weapon meter); deathlink broadcasts only a sparkless death. Without the getter, available() is false and player_spark() is 0.
bool spark_api_available();
int player_spark();

// The live player's world, resolved from the game's live-Player global; null when there is no player yet.
void *player_world();

// One of the world's per-type entity lists by game type name ("KeyBlock", "KeyBlockChain", ...); entries are the components, as a member detour's `this`.
// Returns the total count even when the buffer is too small, so a null/0 call sizes it; 0 when the build lacks the entry. There is no "Chest" list.
std::size_t world_entity_list(void *world, const char *list, void **out, std::size_t cap);

// Scene-graph root and walk, for component types world_entity_list does not name (e.g. "TicketMachine"). entity_children returns direct children only, an
// entity among them, so recurse on the ones component_isa reports as ycEntity; it returns the total count like world_entity_list. component_isa runs the
// game's virtual Isa; ids are the hashlittle2 of the type name (mth::rtti). Too slow to walk every frame. false/null/0 when the build lacks the entries.
bool entity_walk_api_available();
void *world_game_root_entity(void *world);
std::size_t entity_children(void *entity, void **out, std::size_t cap);
bool component_isa(void *component, std::uint64_t type_id);

// Weak references the game clears when the target dies, so a registry can outlive a room without dangling. create() returns null when the API is
// unavailable, get() returns null once the target is gone, and every non-null handle must be destroyed.
bool weak_ptr_api_available();
void *weak_ptr_create(void *component_or_entity);
void *weak_ptr_get(void *weak);
void weak_ptr_destroy(void *weak);

// True while a menu pauses the live world; World::Update then runs only the pause queue. False with no live world or when the build lacks the accessors.
bool world_is_paused();

// Seconds the current room has run, a time-scaled accumulator that stalls when the world's update queues stop, so it sees the game-level pause that
// world_is_paused() misses. available() is false when the build lacks the getter.
bool room_time_api_available();
float room_time();

// modHookCtx that Items::IsItemCollected passes to RunHooks("IsItemCollected"); the layout must match the game's struct exactly.
struct IsItemCollectedCtx
{
    void *collection;             // ItemCollection*
    void *save_slot;              // SaveSlot*
    std::int32_t index;           // location index
    bool include_pawn_shop;       // param4
    bool include_early_collected; // param5: the "ownership query" flag (weapon-swap chest reads true)
    bool mod_handled;             // out: true makes the game return mod_ret_val
    bool mod_ret_val;             // out
};

} // namespace mod
