#include <cstdint>
#include <set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "mth/core/rules/kear_completion.hpp"

namespace
{
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

// AreaKears in the apworld, contiguous from its area kear base.
std::vector<std::int64_t> all_area_kears()
{
    std::vector<std::int64_t> v;
    for (std::int64_t id = 2500; id <= 2516; ++id)
        v.push_back(id);
    return v;
}
} // namespace

// Universal Kears are fungible and all share one id, so vanilla mode counts receipts.
TEST_CASE("have_all_kears counts vanilla kear receipts", "[kear]")
{
    const std::int64_t kear = mth::ap_item_id(mth::kUniversalKearItemType);
    REQUIRE_FALSE(mth::have_all_kears(mth::KearMode::Vanilla, items(std::vector<std::int64_t>(39, kear))));
    REQUIRE(mth::have_all_kears(mth::KearMode::Vanilla, items(std::vector<std::int64_t>(40, kear))));
    REQUIRE(mth::have_all_kears(mth::KearMode::Vanilla, items(std::vector<std::int64_t>(41, kear))));
}

TEST_CASE("have_all_kears needs every area kear id", "[kear]")
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
TEST_CASE("have_all_kears ignores unrelated items", "[kear]")
{
    auto ids = all_single_kears();
    ids.push_back(2306);
    ids.push_back(mth::ap_item_id(9));
    REQUIRE(mth::have_all_kears(mth::KearMode::ApItems, items(ids)));

    REQUIRE_FALSE(mth::have_all_kears(mth::KearMode::ApItems, items({2306, mth::ap_item_id(9), 2500})));
}

// Mirrored from the apworld pools (worlds/mina_the_hollower/data/items/kears.py); a change there must fail here.
TEST_CASE("kear set sizes match the apworld pools", "[kear]")
{
    REQUIRE(mth::kSingleKearCount == 39);
    REQUIRE(mth::kSingleKearRequired == 30);
}

// Both kear sets are derived from kAreaLocks. These are the apworld's SingleKears and AreaKears ids,
// so a table edit that changes either set fails here.
TEST_CASE("kear sets derived from kAreaLocks match the apworld ids", "[kear]")
{
    const std::set<std::int64_t> single_expected = {2019, 2020, 2021, 2022, 2033, 2055, 2069, 2096, 2109, 2115, 2127, 2141, 2151,
                                                    2152, 2157, 2222, 2224, 2225, 2227, 2244, 2246, 2247, 2260, 2262, 2277, 2278,
                                                    2283, 2284, 2285, 2304, 2307, 2318, 2320, 2321, 2336, 2337, 2338, 2358, 2359};
    const std::set<std::int64_t> single(std::begin(mth::kSingleKearItemIds), std::end(mth::kSingleKearItemIds));
    REQUIRE(single.size() == mth::kSingleKearItemIds.size());
    REQUIRE(single == single_expected);

    std::set<std::int64_t> area;
    for (const mth::AreaLock &lock : mth::kAreaLocks)
        area.insert(lock.area_item_id);
    const auto area_ids = all_area_kears();
    REQUIRE(area == std::set<std::int64_t>(area_ids.begin(), area_ids.end()));
}

// HasAllKears() clears at 30 of the 39 single kears, so the gate must fire short of the full set.
TEST_CASE("have_all_kears needs 30 distinct single kears", "[kear]")
{
    const auto ids = all_single_kears();
    REQUIRE(mth::have_all_kears(mth::KearMode::ApItems, items(ids)));

    const std::vector<std::int64_t> exact(ids.begin(), ids.begin() + mth::kSingleKearRequired);
    REQUIRE(mth::have_all_kears(mth::KearMode::ApItems, items(exact)));

    auto missing = exact;
    missing.pop_back();
    REQUIRE_FALSE(mth::have_all_kears(mth::KearMode::ApItems, items(missing)));

    // A duplicate must not stand in for the absent one: this mode counts distinct ids, not receipts.
    missing.push_back(exact.front());
    REQUIRE(missing.size() == exact.size());
    REQUIRE_FALSE(mth::have_all_kears(mth::KearMode::ApItems, items(missing)));
}
