// Characterization of parse_slot_data: what the client makes of a server slot_data blob, degenerate
// corners included. Malformed input fails closed by design.
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "mocks/capturing_log.hpp"
#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/ap/slot_data.hpp"
#include "mth/core/rules/goal.hpp"
#include "pal/pal_log.hpp"

using nlohmann::json;

namespace
{

using mth::test::CapturingLog;

// RAII around the process-wide log seam so a throwing REQUIRE cannot leave it dangling.
class LogCapture
{
  public:
    LogCapture()
    {
        pal::set_default_log(&sink_);
    }
    ~LogCapture()
    {
        pal::set_default_log(nullptr);
    }
    LogCapture(const LogCapture &) = delete;
    LogCapture &operator=(const LogCapture &) = delete;

    const CapturingLog &sink() const
    {
        return sink_;
    }

  private:
    CapturingLog sink_;
};

} // namespace

TEST_CASE("parse_slot_data: an absent or empty blob yields every default", "[slot_data]")
{
    for (const json &blob : {json{}, json::object()})
    {
        INFO(blob.dump());
        const mth::SlotDataConfig cfg = mth::parse_slot_data(blob);

        REQUIRE_FALSE(cfg.ossex_start);
        REQUIRE(cfg.kear_mode == mth::KearMode::ApItems);
        REQUIRE_FALSE(cfg.burrow_rando);
        REQUIRE_FALSE(cfg.swim_rando);
        REQUIRE_FALSE(cfg.rope_rando);
        REQUIRE_FALSE(cfg.puff_rando);
        REQUIRE_FALSE(cfg.spring_rando);
        REQUIRE_FALSE(cfg.carry_rando);
        REQUIRE(cfg.train_pass_cost == mth::kTrainPassCostDefault);
        REQUIRE_FALSE(cfg.deathlink);
        REQUIRE(cfg.max_stat_level == 99);
        REQUIRE(cfg.goal_config == 0);
        REQUIRE(cfg.goal_generators == 99);
        REQUIRE(cfg.goal_bosses == 99);
        REQUIRE(cfg.broken_generator_mask == mth::kAllGeneratorsMask);
        REQUIRE_FALSE(cfg.wallet_cap);
        // A seed that says nothing about the shortcut switches gets vanilla ones, since it cannot grant any.
        REQUIRE_FALSE(cfg.mirror_switch_rando);
        REQUIRE(cfg.lit_generator_lamp_mask == 0);
        REQUIRE(cfg.removed_locations.empty());
    }
}

TEST_CASE("parse_slot_data: a non-object blob does not throw and fails closed on the goal", "[slot_data]")
{
    for (const json &blob : {json::array({1, 2}), json("nope"), json(7), json(true)})
    {
        INFO(blob.dump());
        mth::SlotDataConfig cfg;
        REQUIRE_NOTHROW(cfg = mth::parse_slot_data(blob));
        REQUIRE(cfg.goal_generators == 99);
        REQUIRE(cfg.goal_bosses == 99);
        REQUIRE(cfg.broken_generator_mask == mth::kAllGeneratorsMask);
        REQUIRE(cfg.removed_locations.empty());
    }
}

TEST_CASE("parse_slot_data: the ability flags read as 0/non-0", "[slot_data]")
{
    const json data = {{"ossex_start", 1}, {"burrow_rando", 1}, {"swim_rando", 0}, {"rope_rando", 1}, {"puff_rando", 0},        {"spring_rando", 1},
                       {"carry_rando", 1}, {"train_rando", 0},  {"death_link", 1}, {"wallet_cap", 1}, {"train_pass_cost", 250}, {"mirror_switch_rando", 1}};
    const mth::SlotDataConfig cfg = mth::parse_slot_data(data);

    REQUIRE(cfg.ossex_start);
    REQUIRE(cfg.burrow_rando);
    REQUIRE_FALSE(cfg.swim_rando);
    REQUIRE(cfg.rope_rando);
    REQUIRE_FALSE(cfg.puff_rando);
    REQUIRE(cfg.spring_rando);
    REQUIRE(cfg.carry_rando);
    REQUIRE_FALSE(cfg.train_rando);
    REQUIRE(cfg.deathlink);
    REQUIRE(cfg.wallet_cap);
    REQUIRE(cfg.train_pass_cost == 250);
    REQUIRE(cfg.mirror_switch_rando);
}

TEST_CASE("parse_slot_data: any non-zero number or true sets a flag, and fractions truncate", "[slot_data]")
{
    REQUIRE(mth::parse_slot_data(json{{"ossex_start", 2}}).ossex_start);
    REQUIRE(mth::parse_slot_data(json{{"ossex_start", -1}}).ossex_start);
    REQUIRE(mth::parse_slot_data(json{{"ossex_start", true}}).ossex_start);
    REQUIRE_FALSE(mth::parse_slot_data(json{{"ossex_start", false}}).ossex_start);
    REQUIRE(mth::parse_slot_data(json{{"train_pass_cost", 250.9}}).train_pass_cost == 250);
    REQUIRE(mth::parse_slot_data(json{{"max_stat_level", 42.7}}).max_stat_level == 42);
}

TEST_CASE("parse_slot_data: kear_rando maps to a mode, unknown values to ApItems", "[slot_data]")
{
    REQUIRE(mth::parse_slot_data(json{{"kear_rando", 0}}).kear_mode == mth::KearMode::Vanilla);
    REQUIRE(mth::parse_slot_data(json{{"kear_rando", 1}}).kear_mode == mth::KearMode::ApItems);
    REQUIRE(mth::parse_slot_data(json{{"kear_rando", 2}}).kear_mode == mth::KearMode::AreaApItems);
    REQUIRE(mth::parse_slot_data(json{{"kear_rando", 7}}).kear_mode == mth::KearMode::ApItems);
    REQUIRE(mth::parse_slot_data(json{{"kear_rando", -3}}).kear_mode == mth::KearMode::ApItems);
}

TEST_CASE("parse_slot_data: max_stat_level is clamped to 10..99", "[slot_data]")
{
    REQUIRE(mth::parse_slot_data(json{{"max_stat_level", 42}}).max_stat_level == 42);
    REQUIRE(mth::parse_slot_data(json{{"max_stat_level", 0}}).max_stat_level == 10);
    REQUIRE(mth::parse_slot_data(json{{"max_stat_level", -5}}).max_stat_level == 10);
    REQUIRE(mth::parse_slot_data(json{{"max_stat_level", 1000}}).max_stat_level == 99);
}

TEST_CASE("parse_slot_data: goal fields pass through unvalidated", "[slot_data]")
{
    const json data = {{"goal_config", 2}, {"goal_generators", 4}, {"goal_bosses", 6}};
    const mth::SlotDataConfig cfg = mth::parse_slot_data(data);
    REQUIRE(cfg.goal_config == 2);
    REQUIRE(cfg.goal_generators == 4);
    REQUIRE(cfg.goal_bosses == 6);

    // No range check: a goal_config the tracker does not know, or a count nothing can reach, lands as written.
    const mth::SlotDataConfig odd = mth::parse_slot_data(json{{"goal_config", 99}, {"goal_generators", -1}, {"goal_bosses", 500}});
    REQUIRE(odd.goal_config == 99);
    REQUIRE(odd.goal_generators == -1);
    REQUIRE(odd.goal_bosses == 500);
}

TEST_CASE("parse_slot_data: goal counts default high when the key is absent", "[slot_data]")
{
    // Defaulting high fails closed: goal completion is irreversible and releases this slot's items,
    // so an unsatisfiable goal beats one a missing key could complete spuriously.
    const mth::SlotDataConfig cfg = mth::parse_slot_data(json{{"goal_config", 1}});
    REQUIRE(cfg.goal_config == 1);
    REQUIRE(cfg.goal_generators == 99);
    REQUIRE(cfg.goal_bosses == 99);
}

TEST_CASE("parse_slot_data: an absent broken_generators counts every generator", "[slot_data]")
{
    REQUIRE(mth::parse_slot_data(json::object()).broken_generator_mask == mth::kAllGeneratorsMask);
    // A non-array value is treated as absent, not as an empty list.
    REQUIRE(mth::parse_slot_data(json{{"broken_generators", 3}}).broken_generator_mask == mth::kAllGeneratorsMask);
    REQUIRE(mth::parse_slot_data(json{{"broken_generators", nullptr}}).broken_generator_mask == mth::kAllGeneratorsMask);
}

TEST_CASE("parse_slot_data: an empty broken_generators counts no generator", "[slot_data]")
{
    // Only listed generators count toward the goal (#141), so an empty list lists nothing. Distinct
    // from absent, which counts every generator.
    REQUIRE(mth::parse_slot_data(json{{"broken_generators", json::array()}}).broken_generator_mask == 0);
}

TEST_CASE("parse_slot_data: out-of-range and non-integer broken_generators are dropped", "[slot_data]")
{
    LogCapture log;
    const mth::SlotDataConfig cfg = mth::parse_slot_data(json{{"broken_generators", json::array({0, 6, -1, "1", 2.5, nullptr})}});

    REQUIRE(cfg.broken_generator_mask == (std::uint64_t{1} << 2));
    REQUIRE(log.sink().saw(pal::LogLevel::Warn, "broken_generators"));
}

TEST_CASE("parse_slot_data: lit_generators folds indices into a lamp mask", "[slot_data]")
{
    REQUIRE(mth::parse_slot_data(json{{"lit_generators", json::array({0, 2})}}).lit_generator_lamp_mask == 0x5);
    REQUIRE(mth::parse_slot_data(json{{"lit_generators", json::array()}}).lit_generator_lamp_mask == 0);
    REQUIRE(mth::parse_slot_data(json{{"lit_generators", 4}}).lit_generator_lamp_mask == 0);
    // 6 is the Radiant Manor prime lamp: a real index in the game, out of range for this option.
    REQUIRE(mth::parse_slot_data(json{{"lit_generators", json::array({1, 6, -2})}}).lit_generator_lamp_mask == 0x2);
}

TEST_CASE("parse_slot_data: removed_locations keeps every integer id and drops the rest", "[slot_data]")
{
    const mth::SlotDataConfig cfg = mth::parse_slot_data(json{{"removed_locations", json::array({500, 100, "2", 3.5, nullptr, 300})}});

    REQUIRE(std::set<std::int64_t>(cfg.removed_locations.begin(), cfg.removed_locations.end()) == std::set<std::int64_t>{100, 300, 500});
    REQUIRE(cfg.removed_locations.size() == 3);
}

TEST_CASE("parse_slot_data: an absent or non-array removed_locations prunes nothing", "[slot_data]")
{
    REQUIRE(mth::parse_slot_data(json::object()).removed_locations.empty());
    REQUIRE(mth::parse_slot_data(json{{"removed_locations", json::array()}}).removed_locations.empty());
    REQUIRE(mth::parse_slot_data(json{{"removed_locations", 12}}).removed_locations.empty());
}

TEST_CASE("parse_slot_data: a key of the wrong JSON type throws", "[slot_data]")
{
    // A wrong-typed key throws out of the parse so a seed never runs on configuration the apworld did not specify.
    REQUIRE_THROWS_AS(mth::parse_slot_data(json{{"ossex_start", "yes"}}), nlohmann::json::type_error);
    REQUIRE_THROWS_AS(mth::parse_slot_data(json{{"goal_generators", "4"}}), nlohmann::json::type_error);
    REQUIRE_THROWS_AS(mth::parse_slot_data(json{{"train_pass_cost", json::array({1})}}), nlohmann::json::type_error);
}

TEST_CASE("parse_slot_data: unknown keys are ignored", "[slot_data]")
{
    const mth::SlotDataConfig cfg = mth::parse_slot_data(json{{"future_option", 5}, {"ossex_start", 1}});
    REQUIRE(cfg.ossex_start);
    // train_rando defaults on for any object blob, since current seeds shuffle the tickets without sending the key.
    REQUIRE(cfg.train_rando);
}
