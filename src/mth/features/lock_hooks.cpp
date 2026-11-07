#include "mth/features/lock_hooks.hpp"

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

#include "mod/mod_api.hpp"
#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/ap/ap_state.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/features/game_table_access.hpp"
#include "mth/features/name_key.hpp"
#include "pal/pal_game.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"

namespace
{

mth::LockRegistry *g_locks = nullptr;
bool g_seed_logged = false;

std::set<int> g_logged_lock_slots;  // identity log dedup (game-thread only)
std::set<int> g_logged_chain_slots; // identity log dedup for KeyBlockChain (game-thread only)

// Resolve a live KeyBlock's effective s_rItemCollection slot (the warp-resolved id the unlock bit
// uses). KeyBlock+0x2d0 is only cached for "PairLock" locks; otherwise it is -1 and the game
// name-scans. Mirrors the SetSaveUnlocked fallback. Returns -1 if genuinely unmatched.
// out_key (optional): receives the derived name-hash compare key (0 on the cached fast path),
// for diagnosing an unresolved (-1) lock in one in-game pass (key==0 => entity chain broke).
[[nodiscard]] int resolve_effective_slot(void *self, std::uint64_t *out_key = nullptr)
{
    if (out_key != nullptr)
        *out_key = 0;
    if (!mth::tables::collection_resolved() || self == nullptr)
        return -1;

    // Fast path: PairLock locks have the ctor-cached, already-warp-resolved slot.
    const int cached = pal::at<int>(self, mth::layout::kKeyBlockSlotOff);
    if (cached >= 0)
        return cached;

    // Derive the name-hash compare key from the entity (SetSaveUnlocked b4a81b..b4a849).
    const std::uint64_t key = mth::component_name_key(self);
    if (out_key != nullptr)
        *out_key = key;
    return mth::tables::collection_slot_for_name_key(key);
}

// A lock already spawned solid when its slot was removed won't self-open (the unlock bit is only
// read at spawn). Remove the block live; seed_removed_locks has set the persistent bit so the chain
// "all-opened" door still fires and the lock re-spawns open on room re-entry. QueueDestroy is idempotent,
// and it only queues, so it cannot mutate the entity list being walked.
void open_key_block(void *self)
{
    if (g_locks == nullptr)
        return;

    std::uint64_t key = 0;
    const int slot = resolve_effective_slot(self, &key);
    if (g_logged_lock_slots.insert(slot).second)
        pal::logf(pal::LogLevel::Debug, "KeyBlock slot=%d (raw +0x2d0=%d key=0x%llx)", slot, pal::at<int>(self, mth::layout::kKeyBlockSlotOff),
                  static_cast<unsigned long long>(key));

    if (slot < 0 || !mod::queue_destroy_available() || !g_locks->is_removed(slot))
        return;

    void *ent = pal::at<void *>(self, mth::layout::kComponentEntityOff);
    void *world = ent != nullptr ? pal::at<void *>(ent, mth::layout::kEntityWorldOff) : nullptr;
    if (ent != nullptr && world != nullptr)
        mod::queue_destroy_entity(world, ent, false);
}

// Resolve a live KeyBlockChain's effective slot. A chain has no cached +0x2d0; its identity is the
// SpawnPoint it gates (KeyBlockChain+0x1c0), whose name-key drives the same s_rItemCollection scan +
// warp remap the chain ctor uses to self-gate. Returns -1 if no SpawnPoint / unmatched.
[[nodiscard]] int resolve_chain_slot(void *self)
{
    if (!mth::tables::collection_resolved() || self == nullptr)
        return -1;

    void *sp = pal::at<void *>(self, mth::layout::kChainSpawnPointOff);
    const std::uint64_t key = mth::object_name_key(sp); // 0 for a chain that gates no SpawnPoint
    if (key == 0)
        return -1;
    return mth::tables::collection_slot_for_name_key(key);
}

// An already-spawned chain reads its unlock bit only at ctor time, so drive the state machine to the
// open/kill state for a removed slot; seed_removed_locks handles re-entry.
void open_chain(void *base)
{
    if (g_locks == nullptr)
        return;

    const int slot = resolve_chain_slot(base);
    if (g_logged_chain_slots.insert(slot).second)
        pal::logf(pal::LogLevel::Debug, "KeyBlockChain slot=%d (cur state=%d)", slot, pal::at<int>(base, mth::layout::kChainStateCurOff));

    if (slot < 0 || !g_locks->is_removed(slot))
        return;

    // Already opening/killed? leave the native transition alone (idempotent).
    if (pal::at<int>(base, mth::layout::kChainStateCurOff) == mth::layout::kChainOpenState)
        return;

    pal::at<int>(base, mth::layout::kChainStateReqOff) = mth::layout::kChainOpenState;
    pal::at<unsigned char>(base, mth::layout::kChainStatePendingOff) = 1;
}

// No room holds anywhere near this many of one type, so a larger count means the list is not what we
// think it is. Walk nothing rather than read that many pointers.
constexpr std::size_t kEntityListCap = 4096;
bool g_list_cap_logged = false;

// Enumerate one of the world's per-type entity lists. The entries are the components themselves, i.e. the
// same pointer the old ::Update detour received as `this`.
void for_each_entity(void *world, const char *list, void (*fn)(void *))
{
    const std::size_t count = mod::world_entity_list(world, list, nullptr, 0);
    if (count == 0)
        return;
    if (count > kEntityListCap)
    {
        if (!g_list_cap_logged)
        {
            g_list_cap_logged = true;
            pal::logf(pal::LogLevel::Warn, "locks: \"%s\" reported %zu entities; refusing to walk it", list, count);
        }
        return;
    }

    static std::vector<void *> buf; // game-thread only; keeps its capacity between ticks
    buf.assign(count, nullptr);
    // The fill call re-counts, so take the smaller of the two: a spawn between the calls must not
    // make us read past what was actually written.
    const std::size_t got = mod::world_entity_list(world, list, buf.data(), buf.size());
    const std::size_t n = got < count ? got : count;
    for (std::size_t i = 0; i < n; ++i)
        if (buf[i] != nullptr)
            fn(buf[i]);
}

std::set<int> g_logged_chest_slots; // identity log dedup (game-thread only)

// One MM_WeakPtr* per live Chest, never a raw Chest*: a chest can be freed between two sweeps and the
// weak pointer is the only thing that reports it. Game-thread only.
std::vector<void *> g_chests;

// A room holds a handful of chests, so nearing this means the registry is not being drained. Stop
// growing rather than leak a handle per construct.
constexpr std::size_t kMaxTrackedChests = 512;
bool g_overflow_logged = false;

// Resolve a live locked Chest's effective s_rItemCollection slot. A chest has no cached slot; its
// identity is the SpawnPoint name-key reached through its entity ref, scanned against s_rItemCollection +
// warp-remapped exactly as the chest ctor's own gate does. -1 if unmatched.
[[nodiscard]] int resolve_chest_slot(void *self)
{
    if (!mth::tables::collection_resolved() || self == nullptr)
        return -1;

    const std::uint64_t key = mth::component_name_key(self);
    if (key == 0)
        return -1;
    return mth::tables::collection_slot_for_name_key(key);
}

// Clear the locked flag for a registered slot so the chest opens with no kear; the ctor only reads the
// unlock bit at spawn, so an already-spawned chest needs this. seed_removed_locks handles re-entry; the
// clear is idempotent.
void unlock_chest(void *base)
{
    if (g_locks == nullptr)
        return;

    auto &locked = *reinterpret_cast<std::uint8_t *>(static_cast<char *>(base) + mth::layout::kChestLockedFlagOff);
    if (locked == 0)
        return; // not a locked chest (or already cleared) -> nothing to do

    const int slot = resolve_chest_slot(base);
    if (g_logged_chest_slots.insert(slot).second)
        pal::logf(pal::LogLevel::Debug, "locked Chest slot=%d", slot);

    if (slot < 0 || !g_locks->is_removed(slot))
        return;

    locked = 0;
    pal::logf(pal::LogLevel::Info, "chest: cleared kear-lock on slot=%d (registered for removal)", slot);
}

// Native ChestConstruct callback: the ctx hands back the real Chest*, so there is no per-platform
// subobject fixup and no per-frame detour.
void on_chest_construct(void *chest)
{
    void *weak = mod::weak_ptr_create(chest);
    if (weak == nullptr)
        return;
    if (g_chests.size() >= kMaxTrackedChests)
    {
        mod::weak_ptr_destroy(weak);
        if (!g_overflow_logged)
        {
            g_overflow_logged = true;
            pal::logf(pal::LogLevel::Warn, "chest: tracking %zu chests; ignoring further ones", g_chests.size());
        }
        return;
    }
    g_chests.push_back(weak);
}

void drop_all_chests()
{
    for (void *weak : g_chests)
        mod::weak_ptr_destroy(weak);
    g_chests.clear();
}

} // namespace

namespace mth
{

LockHooks::LockHooks()
{
    g_locks = &locks_;
    tables::resolve();

    if (!mod::queue_destroy_available())
        pal::logf(pal::LogLevel::Warn, "LockHooks: WorldQueueDestroyEntity unavailable; live lock removal disabled");
    if (!mod::weak_ptr_api_available())
        pal::logf(pal::LogLevel::Warn, "chest: weak pointers unavailable; kear-lock clearing disabled");
    mod::install_chest_construct_hook(&on_chest_construct);
}

LockHooks::~LockHooks()
{
    mod::remove_chest_construct_hook(); // stop the registration callback before the registry goes away
    drop_all_chests();
    g_logged_chest_slots.clear();
    g_overflow_logged = false;
    g_logged_lock_slots.clear();
    g_logged_chain_slots.clear();
    g_locks = nullptr; // the sweep handlers null-check it
    g_seed_logged = false;
    g_list_cap_logged = false;
}

LockRegistry &LockHooks::locks()
{
    return locks_;
}

// Open every already-spawned lock whose slot is registered for removal. Walks the world's own per-type
// entity lists instead of detouring each class's update, so it covers both platforms with no signature.
// Then walks the live chests, clearing the lock on the registered ones and dropping whatever the game
// freed; that half runs with no live player too, so freed handles do not pile up.
void LockHooks::sweep()
{
    // No live player means no room to open anything in.
    if (void *world = mod::player_world(); world != nullptr)
    {
        for_each_entity(world, "KeyBlock", &open_key_block);
        for_each_entity(world, "KeyBlockChain", &open_chain);
    }

    for (std::size_t i = 0; i < g_chests.size();)
    {
        void *chest = mod::weak_ptr_get(g_chests[i]);
        if (chest == nullptr)
        {
            mod::weak_ptr_destroy(g_chests[i]);
            g_chests[i] = g_chests.back(); // order does not matter; swap-erase keeps the sweep O(n)
            g_chests.pop_back();
            continue;
        }
        unlock_chest(chest);
        ++i;
    }
}

void LockHooks::on_world_destroy()
{
    drop_all_chests();
}

void LockHooks::seed_kear_blocks(const ApState &state)
{
    for (const auto &it : state.received_items())
    {
        if (is_kear_block_item(it.item_id))
        {
            locks_.set_removed(kear_block_engine_id(it.item_id));
            if (it.item_id == kMMFirstDoubleKearBlockID)
                locks_.set_removed(kear_block_engine_id(kMMSecondDoubleKearBlockID));
        }
        if (is_area_lock_item(it.item_id))
        {
            for (const AreaLock &lock : kAreaLocks)
                if (lock.area_item_id == it.item_id)
                    locks_.set_removed(kear_block_engine_id(lock.lock_item_id));
        }
    }
}

// Set the native unlock bit for every removed lock so the KeyBlock ctor gate spawns it open.
// Idempotent; runs each tick in the pre-World::Update window. Replicates KeyBlock::SetSaveUnlocked's math.
void LockHooks::seed_removed_locks()
{
    if (!tables::collection_resolved())
        return;
    void *saveslot = pal::active_save_slot();
    if (saveslot == nullptr)
        return; // no active save (e.g. title/menus)

    const std::vector<int> slots = locks_.removed_slots();
    if (slots.empty())
        return;

    auto &field = pal::at<std::uint64_t>(saveslot, layout::kSaveBlockUnlockOff);
    for (int slot : slots)
    {
        if (slot < 0 || slot >= layout::kLocationCount)
            continue;
        // 0xff is the table's "no unlock bit" sentinel and shipping rows do use it; shifting by it
        // is UB and on x86 wraps to bit 63, silently unlocking an unrelated lock in the save.
        const std::uint8_t bit = tables::collection_bit_index(slot);
        if (!tables::collection_bit_usable(bit))
        {
            pal::logf(pal::LogLevel::Warn, "locks: slot %d has no usable unlock bit (idx=%u); not seeding", slot, static_cast<unsigned>(bit));
            continue;
        }
        field |= (std::uint64_t{1} << bit);
    }

    if (!g_seed_logged)
    {
        g_seed_logged = true;
        pal::logf(pal::LogLevel::Info, "locks: seeded %zu removed lock(s) into SaveSlot unlock bitfield", slots.size());
    }
}

} // namespace mth
