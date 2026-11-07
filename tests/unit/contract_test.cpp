// Client ID constants and slot_data parsing checked against the apworld's own tables (tests/fixtures/contract.json, see CONTRIBUTING.md).
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "mth/core/ap/ability_ids.hpp"
#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/ap/slot_data.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/core/game/modifiers.hpp"
#include "mth/core/rules/goal.hpp"
#include "mth/core/rules/kear_completion.hpp"

using nlohmann::json;

namespace
{
const json &fixture()
{
    static const json j = []
    {
        std::ifstream in(MTHAP_CONTRACT_FIXTURE);
        if (!in)
            FAIL("missing " MTHAP_CONTRACT_FIXTURE "; regenerate it with: python scripts/gen_contract_fixture.py ../world");
        return json::parse(in);
    }();
    return j;
}

std::int64_t world_item(const std::string &name)
{
    return fixture().at("items").at(name).get<std::int64_t>();
}

std::int64_t world_location(const std::string &name)
{
    return fixture().at("locations").at(name).get<std::int64_t>();
}

std::int64_t world_offset(const std::string &name)
{
    return fixture().at("offsets").at(name).get<std::int64_t>();
}

std::set<std::int64_t> world_item_ids(std::int64_t lo, std::int64_t hi)
{
    std::set<std::int64_t> ids;
    for (const auto &[name, id] : fixture().at("items").items())
        if (id.get<std::int64_t>() >= lo && id.get<std::int64_t>() < hi)
            ids.insert(id.get<std::int64_t>());
    return ids;
}

std::vector<mth::ReceivedItem> items(const std::vector<std::int64_t> &ids)
{
    std::vector<mth::ReceivedItem> v;
    int idx = 0;
    for (std::int64_t id : ids)
        v.push_back(mth::ReceivedItem{id, idx++, 1, 0});
    return v;
}

std::vector<std::int64_t> all_single_kears()
{
    return std::vector<std::int64_t>(std::begin(mth::kSingleKearItemIds), std::end(mth::kSingleKearItemIds));
}

std::vector<std::int64_t> all_area_kears()
{
    const auto ids = world_item_ids(mth::kAreaKearLockItemBase, mth::kAbilityItemBase);
    return std::vector<std::int64_t>(ids.begin(), ids.end());
}

// Every key parse_slot_data reads.
const std::set<std::string> kClientSlotKeys = {
    "ossex_start", "kear_rando",  "burrow_rando",        "swim_rando",     "rope_rando",        "puff_rando",        "spring_rando",
    "carry_rando", "train_rando", "train_pass_cost",     "death_link",     "max_stat_level",    "goal_config",       "goal_generators",
    "goal_bosses", "wallet_cap",  "mirror_switch_rando", "lit_generators", "broken_generators", "removed_locations",
};
} // namespace

TEST_CASE("item and location segment bases match the apworld offsets", "[contract]")
{
    CHECK(mth::kProgressiveItemBase == world_offset("ITEMS_OFFSET_PROGRESSIVES"));
    CHECK(mth::kKearBlockItemBase == world_offset("ITEMS_OFFSET_KEARS"));
    CHECK(mth::kAbilityItemBase == world_offset("ITEMS_OFFSET_ABILITIES"));
    CHECK(mth::kBlockerItemBase == world_offset("ITEMS_OFFSET_BLOCKERS"));
    CHECK(mth::kTrapItemBase == world_offset("ITEMS_OFFSET_TRAPS"));
    CHECK(mth::kBossLocBase == world_offset("LOCATIONS_OFFSET_BOSSES"));
    CHECK(mth::kBossLocBase >= mth::layout::kLocationCount);
}

TEST_CASE("every apworld item id decodes to something the client grants", "[contract]")
{
    for (const auto &[name, value] : fixture().at("items").items())
    {
        const auto id = value.get<std::int64_t>();
        INFO(name << " = " << id);
        if (id < mth::kProgressiveItemBase)
        {
            CHECK(mth::is_vanilla_game_item(id));
            CHECK(mth::is_valid_item_type(mth::game_item_type(id)));
            CHECK(mth::game_item_type(id) == id);
            CHECK(mth::ap_item_id(mth::game_item_type(id)) == id);
        }
        else if (id < mth::kKearBlockItemBase)
            CHECK((mth::is_weapon_item(id) || mth::is_stat_cap_item(id) || mth::is_fishing_rod_item(id) || mth::is_map_item(id) || mth::is_wallet_item(id)));
        else if (id < mth::kAreaKearLockItemBase)
            CHECK(mth::is_kear_block_item(id));
        else if (id < mth::kAbilityItemBase)
            CHECK(mth::is_area_lock_item(id));
        else if (id < mth::kBlockerItemBase)
            CHECK(id - mth::kAbilityItemBase < mth::kAbilityCount);
        else if (id < mth::kTrapItemBase)
            CHECK(mth::is_astral_platform_item(id));
        else
        {
            REQUIRE(mth::is_trap_item(id));
            CHECK(mth::trap_for_modifier(mth::trap_modifier_index(id)) != nullptr);
        }
    }
}

TEST_CASE("named apworld items carry the client's ids", "[contract]")
{
    struct Row
    {
        const char *name;
        std::int64_t id;
    };
    const Row rows[] = {
        {"Kear", mth::ap_item_id(mth::kUniversalKearItemType)},
        {"Joule Box", mth::ap_item_id(mth::kUpgradeItemBase + 0)},
        {"Health Rose", mth::ap_item_id(mth::kUpgradeItemBase + 1)},
        {"Spark Container", mth::ap_item_id(mth::kUpgradeItemBase + 2)},
        {"Healing Vial", mth::ap_item_id(mth::kUpgradeItemBase + mth::kVialUpgradeIndex)},
        {"Trinket Bag", mth::ap_item_id(mth::kUpgradeItemBase + 4)},
        {"Train Pass", mth::ap_item_id(mth::kTrainPassItemType)},
        {"Bayou Ticket", mth::ap_item_id(mth::kTrainTicketItemTypeBase + 1)},
        {"Septemburg Ticket", mth::ap_item_id(mth::kTrainTicketItemTypeBase + 2)},
        {"Bone Beach Ticket", mth::ap_item_id(mth::kTrainTicketItemTypeBase + 3)},
        {"Coltrane Peak Ticket", mth::ap_item_id(mth::kTrainTicketItemTypeBase + 4)},
        {"Nightstar", mth::kProgWeaponBase + 0},
        {"Blaststrike Maul", mth::kProgWeaponBase + 1},
        {"Whisper and Vesper", mth::kProgWeaponBase + 2},
        {"Battery Buster", mth::kProgWeaponBase + 3},
        {"Guardian Casket", mth::kProgWeaponBase + 4},
        {"Progressive Attack Cap", mth::kProgStatCapBase + 0},
        {"Progressive Defense Cap", mth::kProgStatCapBase + 1},
        {"Progressive Sidearm Cap", mth::kProgStatCapBase + 2},
        {"Progressive Bone Up Cap", mth::kProgStatCapAllId},
        {"Progressive Fishing Rod", mth::kProgFishingRodId},
        {"Progressive World Map", mth::kMapItem},
        {"Progressive Bone Wallet", mth::kProgWalletId},
        {"Burrow", mth::ability_item_id(mth::Ability::Burrow)},
        {"Swim", mth::ability_item_id(mth::Ability::Swim)},
        {"Climb", mth::ability_item_id(mth::Ability::RopeClimb)},
        {"Bounce", mth::ability_item_id(mth::Ability::BouncePuff)},
        {"Spring", mth::ability_item_id(mth::Ability::BounceSpring)},
        {"Carry", mth::ability_item_id(mth::Ability::Carry)},
    };
    for (const Row &row : rows)
    {
        INFO(row.name);
        CHECK(world_item(row.name) == row.id);
    }
    for (const auto &[name, id] : fixture().at("items").items())
    {
        INFO(name);
        CHECK(mth::is_vanilla_kear_item(id.get<std::int64_t>()) == (name == "Kear"));
    }
}

TEST_CASE("astral switch items decode to the game's own color order", "[contract]")
{
    struct Row
    {
        const char *name;
        int game_color;
    };
    const Row rows[] = {
        {"Green Astral Switch", 1}, {"Red Astral Switch", 4}, {"Blue Astral Switch", 2}, {"Yellow Astral Switch", 0}, {"Purple Astral Switch", 3},
    };
    for (const Row &row : rows)
    {
        INFO(row.name);
        REQUIRE(mth::is_astral_platform_item(world_item(row.name)));
        CHECK(mth::astral_platform_color(world_item(row.name)) == row.game_color);
    }
}

TEST_CASE("ap_ids: the trap segment is 1000 wide from kTrapItemBase", "[contract][trap]")
{
    REQUIRE(mth::is_trap_item(mth::kTrapItemBase));
    REQUIRE(mth::is_trap_item(mth::kTrapItemBase + 204));
    REQUIRE(mth::is_trap_item(mth::kTrapItemBase + 999));
    REQUIRE_FALSE(mth::is_trap_item(mth::kTrapItemBase - 1));
    REQUIRE_FALSE(mth::is_trap_item(mth::kTrapItemBase + 1000));
    REQUIRE_FALSE(mth::is_trap_item(mth::ap_item_id(9)));
}

TEST_CASE("every apworld location is a collection slot or a boss slot", "[contract]")
{
    for (const auto &[name, value] : fixture().at("locations").items())
    {
        const auto id = value.get<std::int64_t>();
        INFO(name << " = " << id);
        if (id < mth::kBossLocBase)
            CHECK(id < mth::layout::kLocationCount);
        else
            CHECK(id <= mth::boss_location_slot(mth::kMaxBossIndex));
    }
}

TEST_CASE("named apworld locations carry the client's slots", "[contract]")
{
    CHECK(world_location("OS Kear Institute Kear Completion") == mth::ap_loc_id(mth::kKearCompletionLocIdx));

    std::set<std::int64_t> legovich;
    for (const auto &[name, id] : fixture().at("locations").items())
        if (name.starts_with("OS Legovich's Arms "))
            legovich.insert(id.get<std::int64_t>());
    CHECK(std::ranges::includes(std::set<std::int64_t>(std::begin(mth::kLegovichLocations), std::end(mth::kLegovichLocations)), legovich));

    // The donation machine's Train Pass is refused client-side because the apworld has no location for it (#162).
    for (const auto &[name, id] : fixture().at("locations").items())
        CHECK(id.get<std::int64_t>() != mth::ap_loc_id(mth::kTrainPassLocIdx));
}

TEST_CASE("apworld generator indexes map onto SaveSlot 0x290 bits", "[contract][goal]")
{
    // BossComponent::SetGeneratorFixed sets these bits per generator (#146).
    struct Row
    {
        const char *name;
        int save_bit;
    };
    const Row rows[] = {
        {"Nox's Bayou", 2}, {"Queensbury Crypt", 1}, {"Septemburg", 3}, {"Bone Beach", 4}, {"Coltrane Peak", 5}, {"Astral Orrery", 6},
    };
    for (const auto &[name, index] : fixture().at("generator_indexes").items())
    {
        INFO(name);
        CHECK(std::any_of(std::begin(rows), std::end(rows), [&](const Row &row) { return name == row.name; }));
    }
    for (const Row &row : rows)
    {
        INFO(row.name);
        const int index = fixture().at("generator_indexes").at(row.name).get<int>();
        CHECK(mth::broken_generator_mask({index}) == (std::uint64_t{1} << row.save_bit));
    }
    REQUIRE(mth::broken_generator_mask({0, 1, 2}) == 0b1110u);
    REQUIRE(mth::broken_generator_mask({}) == 0u);
    REQUIRE(mth::broken_generator_mask({0, 0}) == (std::uint64_t{1} << 2));
    REQUIRE(mth::broken_generator_mask({-1, 6, 64, 999}) == 0u);
}

TEST_CASE("apworld goal option values map onto the client's goal constants", "[contract][goal]")
{
    const std::map<std::string, int> client = {
        {"radiantmanorgenerator", mth::kGoalFinish}, {"fixgenerators", mth::kGoalGenerators}, {"killbosses", mth::kGoalBosses}};
    for (const auto &[name, value] : fixture().at("goal_options").items())
    {
        INFO(name << " = " << value);
        const auto it = client.find(name);
        if (it == client.end())
            FAIL_CHECK("apworld goal " << name << " has no client goal: handle it in goal_met and add it to the map here");
        else
            CHECK(value.get<int>() == it->second);
    }
}

TEST_CASE("the apworld's kear items are kears the client counts, and enough to complete", "[contract][kear]")
{
    const std::set<std::int64_t> single(std::begin(mth::kSingleKearItemIds), std::end(mth::kSingleKearItemIds));
    REQUIRE(single.size() == mth::kSingleKearItemIds.size());
    const auto world_single = world_item_ids(mth::kKearBlockItemBase, mth::kAreaKearLockItemBase);
    CHECK(std::ranges::includes(single, world_single));
    CHECK(world_single.size() >= static_cast<std::size_t>(mth::kSingleKearRequired));

    // Area mode completes only on every kAreaLocks item, so each one has to exist in the apworld.
    std::set<std::int64_t> area;
    for (const mth::AreaLock &lock : mth::kAreaLocks)
        area.insert(lock.area_item_id);
    CHECK(area == world_item_ids(mth::kAreaKearLockItemBase, mth::kAbilityItemBase));

    // HasAllKears() in the apworld's data/rules/state_rules.py clears at 30 single kears.
    CHECK(mth::kSingleKearRequired == 30);
}

// Universal Kears are fungible and all share one id, so vanilla mode counts receipts.
TEST_CASE("have_all_kears counts vanilla kear receipts", "[contract][kear]")
{
    const std::int64_t kear = world_item("Kear");
    REQUIRE_FALSE(mth::have_all_kears(mth::KearMode::Vanilla, items(std::vector<std::int64_t>(39, kear))));
    REQUIRE(mth::have_all_kears(mth::KearMode::Vanilla, items(std::vector<std::int64_t>(40, kear))));
    REQUIRE(mth::have_all_kears(mth::KearMode::Vanilla, items(std::vector<std::int64_t>(41, kear))));
}

TEST_CASE("have_all_kears needs every area kear id", "[contract][kear]")
{
    const auto ids = all_area_kears();
    REQUIRE(mth::have_all_kears(mth::KearMode::AreaApItems, items(ids)));

    auto missing = ids;
    missing.erase(missing.begin());
    REQUIRE_FALSE(mth::have_all_kears(mth::KearMode::AreaApItems, items(missing)));

    missing.push_back(ids.back());
    REQUIRE(missing.size() == ids.size());
    REQUIRE_FALSE(mth::have_all_kears(mth::KearMode::AreaApItems, items(missing)));
}

// 2306 is the second engine lock the 2304 item removes; it never arrives as an item of its own.
TEST_CASE("have_all_kears ignores unrelated items", "[contract][kear]")
{
    auto ids = all_single_kears();
    ids.push_back(mth::kMMSecondDoubleKearBlockID);
    ids.push_back(mth::ap_item_id(9));
    REQUIRE(mth::have_all_kears(mth::KearMode::ApItems, items(ids)));

    REQUIRE_FALSE(mth::have_all_kears(mth::KearMode::ApItems, items({mth::kMMSecondDoubleKearBlockID, mth::ap_item_id(9), all_area_kears().front()})));
}

TEST_CASE("have_all_kears needs 30 distinct single kears", "[contract][kear]")
{
    const auto ids = all_single_kears();
    REQUIRE(mth::have_all_kears(mth::KearMode::ApItems, items(ids)));

    const std::vector<std::int64_t> exact(ids.begin(), ids.begin() + mth::kSingleKearRequired);
    REQUIRE(mth::have_all_kears(mth::KearMode::ApItems, items(exact)));

    auto missing = exact;
    missing.pop_back();
    REQUIRE_FALSE(mth::have_all_kears(mth::KearMode::ApItems, items(missing)));

    // This mode counts distinct ids, so a duplicate cannot stand in for the absent one.
    missing.push_back(exact.front());
    REQUIRE(missing.size() == exact.size());
    REQUIRE_FALSE(mth::have_all_kears(mth::KearMode::ApItems, items(missing)));
}

TEST_CASE("the client reads every slot_data key the apworld sends", "[contract][slot_data]")
{
    // Keys parse_slot_data skips by design: starting_weapon arrives as a precollected item, cap items count in either bone_up_cap mode, and sem_ver
    // is informational.
    const std::set<std::string> ignored = {"bone_up_cap", "sem_ver", "starting_weapon"};

    for (const auto &[variant, blob] : fixture().at("slot_data").items())
    {
        INFO(variant);
        for (const auto &[key, _] : blob.items())
            if (!kClientSlotKeys.contains(key) && !ignored.contains(key))
                FAIL_CHECK("apworld sends " << key << ": read it in parse_slot_data, or add it to the ignored set here with a reason");
        for (const std::string &key : ignored)
            if (!blob.contains(key))
                FAIL_CHECK("apworld no longer sends " << key << ": remove it from the ignored set here");
    }
}

TEST_CASE("parse_slot_data maps the apworld's real slot_data blobs", "[contract][slot_data]")
{
    for (const auto &[variant, blob] : fixture().at("slot_data").items())
    {
        INFO(variant);
        const mth::SlotDataConfig cfg = mth::parse_slot_data(blob);

        CHECK(cfg.ossex_start == blob.at("ossex_start").get<bool>());
        CHECK(cfg.kear_mode == static_cast<mth::KearMode>(blob.at("kear_rando").get<int>()));
        CHECK(cfg.burrow_rando == (blob.at("burrow_rando").get<int>() != 0));
        CHECK(cfg.swim_rando == (blob.at("swim_rando").get<int>() != 0));
        CHECK(cfg.rope_rando == (blob.at("rope_rando").get<int>() != 0));
        CHECK(cfg.puff_rando == (blob.at("puff_rando").get<int>() != 0));
        CHECK(cfg.spring_rando == (blob.at("spring_rando").get<int>() != 0));
        CHECK(cfg.carry_rando == (blob.at("carry_rando").get<int>() != 0));
        CHECK(cfg.train_rando == (blob.value("train_rando", 1) != 0));
        CHECK(cfg.train_pass_cost == blob.value("train_pass_cost", mth::kTrainPassCostDefault));
        CHECK(cfg.deathlink == (blob.at("death_link").get<int>() != 0));
        CHECK(cfg.max_stat_level == blob.at("max_stat_level").get<int>());
        CHECK(cfg.goal_config == blob.at("goal_config").get<int>());
        CHECK(cfg.goal_generators == blob.at("goal_generators").get<int>());
        CHECK(cfg.wallet_cap == blob.at("wallet_cap").get<bool>());
        CHECK(cfg.mirror_switch_rando == (blob.at("mirror_switch_rando").get<int>() != 0));
        CHECK(cfg.broken_generator_mask == mth::broken_generator_mask(blob.at("broken_generators").get<std::vector<int>>()));
        CHECK(cfg.lit_generator_lamp_mask == mth::lit_mask_from_indices(blob.at("lit_generators").get<std::vector<int>>()));
        CHECK(cfg.removed_locations == blob.at("removed_locations").get<std::vector<std::int64_t>>());

        // The apworld sends goal_bosses 0 while its bosses goal is disabled; under that goal 0 would complete on the first tick.
        CHECK(cfg.goal_config != mth::kGoalBosses);
        CHECK(cfg.goal_bosses == 0);
    }
}
