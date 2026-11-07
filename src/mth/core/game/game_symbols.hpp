#pragma once

namespace mth::sym
{

// Mangled symbol names. Stable across recompiles; verified against the unstripped Linux binary.
// Resolved elsewhere, so not listed: World::Update, Items::OnPickup, Items::IsItemCollected, Pickup::OnPickup, ShopItem::Refresh,
// AreaManager::NewArea and the Chest ctor are named mod hooks; ycWorld::QueueDestroy, Items::SetItemCollected, Player::UpdateStats,
// PhysicsComponent::GetAABB, CarryManager::GetClosestCarryableObject, WaterListener::IsInDeepWaterInternal and the ycTextComponent text/color
// mutators are native API entries; KeyBlock::Update and KeyBlockChain::UpdateState are swept from the world's entity lists; Game::FixedUpdate
// is the tick source.
inline constexpr const char *game_fixed_update = "_ZN4Game11FixedUpdateEv"; // Game::FixedUpdate()
// Items::OnPickupDone(...): resolved for its address only (inbound AP grants call it directly); its pre-hook is the native "ItemsOnPickupDone"
// mod hook.
inline constexpr const char *on_pickup_done = "_ZN5Items12OnPickupDoneEiiP6PlayerRK6ycVec3iijb";
inline constexpr const char *process_sdl_event = "_Z15ProcessSDLEventR9SDL_Event"; // ProcessSDLEvent(SDL_Event&)

// s_rItems: 195-entry item table (stride 0x68, kind at +0x28).
inline constexpr const char *s_r_items = "_ZN12_GLOBAL__N_18s_rItemsE";

// The screen id is polled from the native GetCurrentGameState/GetRoomIndex accessors on the "WorldUpdateEnd" mod hook.

inline constexpr const char *pickup_init = "_ZN6Pickup4InitEiib"; // Pickup::Init(int itemType, int locIdx, bool)

// ShopMenu::ItemPresent(): shop-buy grant funnel; calls Items::OnPickup directly (no Pickup entity).
// Menu stashes locIdx at ShopMenu+0x218 and itemType at +0x21c before this fires.
inline constexpr const char *shop_item_present = "_ZN8ShopMenu11ItemPresentEv"; // ShopMenu::ItemPresent()
// Windows: ShopMenu::ItemPresent is inlined into ShopMenu::InitState; hook InitState there.
inline constexpr const char *shop_init_state = "_ZN8ShopMenu9InitStateEv"; // ShopMenu::InitState()
// Shop::IsOutOfStock(ShopDef const*, OutInfo&): tallies a shop's remaining stock via Items::IsItemCollected
// per slot. Hooked only to bracket a flag so the IsItemCollected override can tell the WeaponMerchant's stock
// query apart from the weapon-swap chest, which reads the same weapon locations (#67).
inline constexpr const char *shop_is_out_of_stock = "_ZN4Shop12IsOutOfStockEPK7ShopDefRNS_7OutInfoE";

// Shop::Get(unsigned long nameHash): linear-searches the static ShopDef table (stride 0x460) and
// returns the matching ShopDef*, consumed by InteractComponent::OpenShop to build the box list.
// Detoured to OR the never-stack bit so stacked slots flatten (one box/level).
inline constexpr const char *shop_get = "_ZN4Shop3GetEm"; // Shop::Get(unsigned long)

// ShopMenu::SetCursor(int index, bool): fires on box selection change; hooked to rewrite the selected
// item's name+description widgets from scouted AP data.
inline constexpr const char *shop_set_cursor = "_ZN8ShopMenu9SetCursorEib"; // ShopMenu::SetCursor(int,bool)

// s_rItemCollection: 361 x 0x50, native itemType at +0x18, maps locIdx to vanilla contents kind.
inline constexpr const char *s_r_item_collection = "_ZN12_GLOBAL__N_117s_rItemCollectionE"; // s_rItemCollection location table

// Active SaveSlot* = *(g_saveManager+0x18); lock-unlocked bits live in a u64 at SaveSlot+0x200.
inline constexpr const char *save_manager = "g_saveManager";

// SaveSlot::Clear(bool): new-file reset; writes the default starting-upgrade fields (region-18 kit).
// Called only at new-file creation, so a post-hook field zero never touches a progressed save.
inline constexpr const char *save_slot_clear = "_ZN8SaveSlot5ClearEb"; // SaveSlot::Clear(bool)
// SaveSlot::InitGamestate(): finishes new-file setup after Clear. Both are called on the same slot
// address; see mth::layout::kSaveSlotArrayOff and pal::layout::kSaveManagerMasterOff.
inline constexpr const char *save_slot_init_gamestate = "_ZN8SaveSlot13InitGamestateEv"; // SaveSlot::InitGamestate()

// SaveManager::WriteSaveData(bool): the "persist a slot" chokepoint, hooked observe-only as the flush trigger for mod-owned saves.
// RequestWriteSaveData has the same cadence but GCC fully inlined it (no symbol, no DWARF).
inline constexpr const char *save_manager_write_save_data = "_ZN11SaveManager13WriteSaveDataEb"; // SaveManager::WriteSaveData(bool)

// Deathlink resolves no game symbols: detection polls the Player+0x1380 death-guard byte edge each tick (DeathBroadcastGate), and apply uses
// the native MinaModAPI PlayerDie.

// Modifiers ("cheats"). Apply hub reads SaveSlot at *(g_saveManager+0x08); live gameplay uses
// *(g_saveManager+0x18). ToggleCheat (menu) + SetCheatApplied (cheat-code) are the only two
// player write paths; the runtime mirror at [CheatManager+0x20] is rebuilt through the native
// CheatManagerActivateSaveCheats entry, so that function needs no address of its own.
inline constexpr const char *activate_save_slot = "_ZN14SessionManager16ActivateSaveSlotEb";        // SessionManager::ActivateSaveSlot(bool)
inline constexpr const char *toggle_cheat = "_ZN12CheatManager11ToggleCheatEibP8SaveSlotbi";        // CheatManager::ToggleCheat(int,bool,SaveSlot*,bool,int)
inline constexpr const char *set_cheat_applied = "_ZN12CheatManager15SetCheatAppliedEibP8SaveSlot"; // CheatManager::SetCheatApplied(int,bool,SaveSlot*)

// Per-stat level cap. Linux detours GetNewGameMaxLevelPlayer (the buy-gate's only live caller) to return
// the per-stat cap, with UpdateState wrapped to supply the cursor stat. Windows inlines the cap and the
// standalone UpdateState is dead code, so it hooks the per-frame LevelUpMenu::Update and presents capped
// stats as already-maxed so the inlined cap gate trips.
inline constexpr const char *level_up_menu_update_state = "_ZN11LevelUpMenu11UpdateStateEv"; // LevelUpMenu::UpdateState() [Linux]
inline constexpr const char *get_new_game_max_level_player =
    "_ZN10CombatData24GetNewGameMaxLevelPlayerEiiP8SaveSlot";                                          // CombatData::GetNewGameMaxLevelPlayer [Linux]
inline constexpr const char *level_up_menu_update = "_ZN11LevelUpMenu6UpdateEP20ycUpdateQueueContext"; // LevelUpMenu::Update(...) [Windows]

// Ability gating (issues #22/#33-#37). Each is the single chokepoint where the ability commits; the
// detours suppress it under AP gating. SetBurrowGround tests swim-vs-land through the
// native water accessor.
inline constexpr const char *player_set_burrow_ground = "_ZN6Player15SetBurrowGroundEv";
inline constexpr const char *player_rope_climb_start = "_ZN6Player14RopeClimbStartEP13GameComponentbb";
inline constexpr const char *bounce_plant_collide = "_ZN11BouncePlant11CollideWithER18PhysicsContactPair";
// Floating puffs bounce inline in CollideWith; ground/burrow-underable puffs launch out-of-line here
// (called from Player land/wall/update paths), so both chokepoints must be gated (issue #47).
inline constexpr const char *bounce_plant_launch = "_ZN11BouncePlant12BounceLaunchEP6Player";
// Neither BouncePlant entry launches anything; both stash a bounce target in Player+0x252c. OnBounce is its only consumer and the only writer
// of launch velocity, so it is the universal gate. Bone Beach bounce bushes are Breakables and reach it through a dispatch inlined into
// Player::Update / Player::SlideOutOfWall that skips both entries above (#168).
inline constexpr const char *player_on_bounce = "_ZN6Player8OnBounceEv";
inline constexpr const char *spring_bellows_collide = "_ZN13SpringBellows11CollideWithER18PhysicsContactPair";
// SpringBellows inherits PhysicsCollisionListener as a secondary base at +376 and its ctor stores that subobject in PhysicsComponent+0x118, so
// physics dispatch never reaches the entry above. Linux compiled that slot as a duplicate body with no adjustor jmp, so a detour on the exported
// symbol installs and never fires (#188); BouncePlant (base at +384) got a real thunk. Windows emits one body taking the adjusted this, so this
// entry is Linux-only.
inline constexpr const char *spring_bellows_collide_listener = "_ZThn376_N13SpringBellows11CollideWithER18PhysicsContactPair";
inline constexpr const char *player_pickup_carryable = "_ZN6Player30PickUpAnyNearbyCarryableObjectEbbb";
// #56: burrow-emerge commit. With carry disabled we suppress the emerge when a carryable is overhead so
// Mina stays burrowed beneath it (no native "duck under a carryable" exists). The overhead check reuses
// the game's own grab query, read-only, through the native AABB and carryable accessors.
inline constexpr const char *mina_on_burrow_jump = "_ZN4Mina12OnBurrowJumpEv";
inline constexpr const char *train_authority_on_npc_event = "_ZN14TrainAuthority10OnNPCEventEjP17InteractEventInfo";
// PawnShopNPC::OnNPCEvent: the pawn shop ("Pawnty") interaction dispatcher. It is its own class, so suppressing it affects no other shop.
inline constexpr const char *pawn_shop_on_npc_event = "_ZN11PawnShopNPC10OnNPCEventEjP17InteractEventInfo";

// HubFountain::Bulb::Update(float,bool): per-lamp visual; detoured to force lit (bulb index at this+0x10)
inline constexpr const char *hub_fountain_bulb_update = "_ZN11HubFountain4Bulb6UpdateEfb";

// TitleScreen::UpdateState(): owns the menu cursor wrap and the option dispatch. Hooked to keep
// the cursor off "Start Game" while disconnected.
inline constexpr const char *title_screen_update_state = "_ZN11TitleScreen11UpdateStateEv";
// TitleScreen::StartGame(): retail only sets the next substate. Suppressed while disconnected; allowed while connected, since that substate builds
// the profile-select menu the takeover drives.
inline constexpr const char *title_screen_start_game = "_ZN11TitleScreen9StartGameEv";

} // namespace mth::sym
