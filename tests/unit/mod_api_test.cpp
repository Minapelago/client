#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "mocks/fake_game.hpp"
#include "mod/mod_api.hpp"
#include "pal/pal_module.hpp"

namespace
{
using Log = std::vector<std::string>;

// A published range that holds every non-null address, so the fake's entries pass the .text check.
constexpr pal::TextRange kWholeAddressSpace{1, std::numeric_limits<std::size_t>::max() - 1};

constexpr std::uint32_t kRevisionWithoutAppendedApi = 148905;
constexpr std::uint32_t kRevisionWithAppendedApi = 200000;

int g_object = 0;
void *const kObject = &g_object;
float g_box[6]{};

static_assert(sizeof(MinaModAPI) == 3416, "new MinaModAPI entry: add a gate row");

struct Entry
{
    const char *name;
    bool appended;
    bool (*reachable)();
};

// Every gated wrapper, with a call that only succeeds when the entry behind it was read and called.
const Entry kEntries[] = {
    {"save", false, [] { return mod::save_api_available(); }},
    {"entity walk", false, [] { return mod::entity_walk_api_available(); }},
    {"weak pointers", false, [] { return mod::weak_ptr_api_available(); }},
    {"WorldQueueDestroyEntity", true, [] { return mod::queue_destroy_entity(kObject, kObject, false); }},
    {"ItemsSetItemCollected", true, [] { return mod::set_item_collected(0, true, nullptr, nullptr); }},
    {"TextComponentSetColor", true, [] { return mod::set_text_color(kObject, 0u); }},
    {"TextComponentSetText", true, [] { return mod::set_text(kObject, "Golden Kear"); }},
    {"TextComponentGetText", true, [] { return mod::text_of(kObject) != nullptr; }},
    {"palette", true, [] { return mod::clone_palette(kObject) != nullptr; }},
    {"WaterListenerIsInDeepWater", true, [] { return mod::water_is_in_deep_water(kObject, false); }},
    {"PhysicsComponentGetAABB", true, [] { return mod::physics_get_aabb(kObject, g_box, false, 0u); }},
    {"CarryManagerGetClosestCarryableObject", true, [] { return mod::closest_carryable(kObject, g_box, 0, 1.0f, nullptr, 0u) != nullptr; }},
    {"PlayerUpdateStats", true, [] { return mod::player_update_stats(); }},
    {"WorldGetEntityList", true, [] { return mod::world_entity_list(kObject, "KeyBlock", nullptr, 0) != 0; }},
    {"GetSymAddr", true, [] { return mod::sym_addr("s_rItems") != nullptr; }},
    {"CheatManagerActivateSaveCheats", true, [] { return mod::cheat_manager_activate_save_cheats(); }},
    {"CheatManagerSetCheatApplied", true, [] { return mod::cheat_manager_set_cheat_applied(0, true); }},
    {"CheatManagerIsCheatApplied", true, [] { return mod::cheat_manager_is_cheat_applied(0); }},
};

// The Worlds each World hook was called with, in order.
struct WorldHookCalls
{
    std::vector<void *> updates;
    std::vector<void *> ends;
    std::vector<void *> destroys;
};
WorldHookCalls g_world_calls;

// Locations the client's IsItemCollected override was asked about, standing in for the game-thread state it reads.
std::vector<int> g_override_queries;

void install_world_hooks()
{
    g_world_calls = {};
    REQUIRE(mod::install_world_update_hook([](void *world) { g_world_calls.updates.push_back(world); }));
    REQUIRE(mod::install_world_update_end_hook([](void *world) { g_world_calls.ends.push_back(world); }));
    REQUIRE(mod::install_world_destroy_hook([](void *world) { g_world_calls.destroys.push_back(world); }));
}
} // namespace

// An entry appended after the running build's API struct lies past its end, so the revision is checked before the field is read.
TEST_CASE("mod: appended entries are reached only on a build new enough to carry them", "[mod]")
{
    mth::test::FakeGame game;
    pal::set_game_text_range(kWholeAddressSpace);
    game.applied_cheats.insert(0);

    for (const std::uint32_t revision : {kRevisionWithoutAppendedApi, kRevisionWithAppendedApi})
    {
        game.revision = revision;
        for (const Entry &e : kEntries)
        {
            CAPTURE(revision, e.name);
            CHECK(e.reachable() == (!e.appended || revision == kRevisionWithAppendedApi));
        }
    }
}

// An entry read past a shorter struct can be garbage that still looks like a pointer; only an address inside the game's .text is called.
TEST_CASE("mod: an entry outside the game's .text range is never called", "[mod]")
{
    mth::test::FakeGame game;
    game.revision = kRevisionWithAppendedApi;

    pal::set_game_text_range(pal::TextRange{});
    REQUIRE(mod::game_revision() == kRevisionWithAppendedApi);
    REQUIRE_FALSE(pal::in_game_text(nullptr));
    REQUIRE_FALSE(mod::queue_destroy_available());

    pal::set_game_text_range(pal::TextRange{0x1000, 0x10});
    REQUIRE(mod::game_revision() == 0);
    REQUIRE(mod::active_save_slot() == -1);
    REQUIRE_FALSE(mod::player_die());
    REQUIRE(game.log.empty());

    constexpr std::uintptr_t base = 0x400000;
    constexpr std::size_t size = 0x1000;
    pal::set_game_text_range(pal::TextRange{base, size});
    REQUIRE(pal::in_game_text(reinterpret_cast<const void *>(base)));
    REQUIRE(pal::in_game_text(reinterpret_cast<const void *>(base + size - 1)));
    REQUIRE_FALSE(pal::in_game_text(reinterpret_cast<const void *>(base - 1)));
    REQUIRE_FALSE(pal::in_game_text(reinterpret_cast<const void *>(base + size)));
    REQUIRE_FALSE(pal::in_game_text(nullptr));
}

TEST_CASE("mod: a hook install fails when the API is missing or refuses it", "[mod]")
{
    mth::test::FakeGame game;
    const auto install_all = []
    {
        return std::vector<bool>{
            mod::install_item_collected_hook(nullptr),   mod::install_world_update_hook(nullptr),      mod::install_world_update_end_hook(nullptr),
            mod::install_world_destroy_hook(nullptr),    mod::install_items_on_pickup_hook(nullptr),   mod::install_items_on_pickup_done_hook(nullptr),
            mod::install_pickup_on_pickup_hook(nullptr), mod::install_shop_item_refresh_hook(nullptr), mod::install_chest_construct_hook(nullptr),
            mod::set_fixed_update_delta_hook(nullptr),   mod::install_input_suppress_hooks(),
        };
    };
    const std::vector<bool> all = install_all();
    mod::remove_all_hooks();
    REQUIRE(all == std::vector<bool>(all.size(), true));
    const std::vector<bool> none(all.size(), false);

    game.install_fails = true;
    REQUIRE(install_all() == none);

    game.api.InstallHook = nullptr;
    REQUIRE(install_all() == none);

    mod::set_api(nullptr);
    REQUIRE(install_all() == none);
}

TEST_CASE("mod: save access fails closed", "[mod][save]")
{
    mth::test::FakeGame game;
    game.active_save_slot = 3;
    REQUIRE(mod::active_save_slot() == 3);
    // The game reports one past the last slot when none is bound.
    game.active_save_slot = 10;
    REQUIRE(mod::active_save_slot() == -1);
    game.active_save_slot = -5;
    REQUIRE(mod::active_save_slot() == -1);

    REQUIRE_FALSE(mod::set_active_save_slot_contents(nullptr));
    REQUIRE_FALSE(mod::set_save_slot_contents(1, nullptr));
    REQUIRE(game.log.empty());

    mod::set_api(nullptr);
    REQUIRE_FALSE(mod::save_api_available());
    REQUIRE(mod::active_save_slot() == -1);
    REQUIRE(mod::active_save_slot_contents().empty());
    REQUIRE_FALSE(mod::set_active_save_slot_contents("[YCD Version: 1]"));
    REQUIRE_FALSE(mod::set_save_slot_contents(1, "[YCD Version: 1]"));
    REQUIRE_FALSE(mod::save_write_enabled());
}

// A negative collection index would set an unrelated bit in the live save.
TEST_CASE("mod: set_item_collected refuses a negative index", "[mod][save]")
{
    mth::test::FakeGame game;
    pal::set_game_text_range(kWholeAddressSpace);
    game.revision = kRevisionWithAppendedApi;

    REQUIRE_FALSE(mod::set_item_collected(-1, true, nullptr, nullptr));
    REQUIRE(mod::set_item_collected(37, true, nullptr, nullptr));
    REQUIRE(game.log == Log{"item 37 collected"});
}

// The cheat array ends at 0xfd, and the write lands in the live save.
TEST_CASE("mod: a cheat write outside 0..0xfd never reaches the game", "[mod][save]")
{
    mth::test::FakeGame game;
    pal::set_game_text_range(kWholeAddressSpace);
    game.revision = kRevisionWithAppendedApi;

    REQUIRE_FALSE(mod::cheat_manager_set_cheat_applied(-1, true));
    REQUIRE_FALSE(mod::cheat_manager_set_cheat_applied(0xfe, true));
    REQUIRE(mod::cheat_manager_set_cheat_applied(0xfd, true));
    REQUIRE(game.log == Log{"cheat 253 on"});
}

TEST_CASE("mod: a cheat query outside 0..0xfd never reaches the game", "[mod][save]")
{
    mth::test::FakeGame game;
    pal::set_game_text_range(kWholeAddressSpace);
    game.revision = kRevisionWithAppendedApi;
    game.applied_cheats = {-1, 0xfd, 0xfe};

    REQUIRE_FALSE(mod::cheat_manager_is_cheat_applied(-1));
    REQUIRE_FALSE(mod::cheat_manager_is_cheat_applied(0xfe));
    REQUIRE(mod::cheat_manager_is_cheat_applied(0xfd));
    REQUIRE_FALSE(mod::cheat_manager_is_cheat_applied(100));
}

TEST_CASE("mod: the World hooks pass on the World they fire for", "[mod][world]")
{
    mth::test::FakeGame game;
    install_world_hooks();

    game.tick();
    void *const level = game.gamestate_world();
    REQUIRE(std::count(g_world_calls.updates.begin(), g_world_calls.updates.end(), level) == 1);
    REQUIRE(std::set<void *>(g_world_calls.updates.begin(), g_world_calls.updates.end()).size() == g_world_calls.updates.size());
    REQUIRE(g_world_calls.ends == g_world_calls.updates);

    game.transition();
    REQUIRE(g_world_calls.destroys == std::vector<void *>{level});
}

// SaveSlot::Upgrade asks from the game's save thread while it migrates an old save. That thread must get the save's own bits and leave the client's
// game-thread state alone.
TEST_CASE("mod: an IsItemCollected query off the game thread gets the vanilla answer", "[mod][save]")
{
    mth::test::FakeGame game;
    g_override_queries.clear();
    REQUIRE(mod::install_item_collected_hook(
        [](int loc_idx, bool)
        {
            g_override_queries.push_back(loc_idx);
            return 1;
        }));

    std::optional<bool> from_save_thread{true};
    std::thread([&] { from_save_thread = game.mod_item_collected(30); }).join();
    REQUIRE_FALSE(from_save_thread.has_value());
    REQUIRE(g_override_queries.empty());

    REQUIRE(game.mod_item_collected(30) == true);
    mod::remove_all_hooks();
}
