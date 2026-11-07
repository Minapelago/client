#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "mth/core/ap/ap_state.hpp"
#include "mth/core/ap/inbound_granter.hpp"
#include "mth/core/ap/item_granter_interface.hpp"
#include "mth/core/ap/rando_bridge.hpp"
#include "mth/core/game/modifiers.hpp"
#include "mth/core/rules/trap_state.hpp"
#include "mth/core/save/ap_save_state.hpp"

namespace
{
struct FakeGranter : mth::IItemGranter
{
    std::vector<int> granted;  // itemTypes accepted
    std::vector<int> in_queue; // receipts accepted but not yet applied (defer mode)
    bool ok = true;
    bool defer = false; // model the real granter's queue: accept now, apply on a later drain

    bool grant(int item_type, int receipt) override
    {
        if (!ok)
            return false;
        granted.push_back(item_type);
        if (defer)
            in_queue.push_back(receipt);
        else
            notify_applied(receipt);
        return true;
    }

    void discard_pending() override
    {
        in_queue.clear();
    }

    // the drain window: everything queued actually lands now
    void apply_queued()
    {
        std::vector<int> batch;
        batch.swap(in_queue);
        for (int receipt : batch)
            notify_applied(receipt);
    }
};

// Nothing in the granter's logic reads or writes the statefile, so these cases only need the format.
mth::ApSaveState detached()
{
    return mth::ApSaveState([] { return std::nullopt; }, [](std::string_view) {});
}

// Backed by a string, for a case that has to rebuild the state and still see what was staged.
mth::ApSaveState backed_by(std::string &text)
{
    return mth::ApSaveState([&text]() -> std::optional<std::string> { return text; }, [&text](std::string_view t) { text = std::string(t); });
}

mth::ApItemReceived recv(std::int64_t item_id, int index)
{
    mth::ApItemReceived e;
    e.item.item_id = item_id;
    e.item.index = index;
    return e;
}
} // namespace

TEST_CASE("InboundGranter grants new items once and dedups by index", "[inbound]")
{
    mth::ApState state;
    mth::ApSaveState save = detached();
    FakeGranter granter;
    mth::InboundGranter inbound(granter, state, save);

    state.apply(recv(mth::ap_item_id(5), 0));
    state.apply(recv(mth::ap_item_id(9), 1));
    inbound.tick();
    REQUIRE(granter.granted == std::vector<int>{5, 9});

    inbound.tick();
    REQUIRE(granter.granted.size() == 2);

    state.apply(recv(mth::ap_item_id(2), 2));
    inbound.tick();
    REQUIRE(granter.granted == std::vector<int>{5, 9, 2});
}

TEST_CASE("InboundGranter does not mark on failure and retries", "[inbound]")
{
    mth::ApState state;
    mth::ApSaveState save = detached();
    FakeGranter granter;
    granter.ok = false;
    mth::InboundGranter inbound(granter, state, save);

    state.apply(recv(mth::ap_item_id(5), 0));
    inbound.tick();
    REQUIRE(granter.granted.empty());
    REQUIRE_FALSE(save.is_granted(0));

    granter.ok = true;
    inbound.tick();
    REQUIRE(granter.granted == std::vector<int>{5});
    REQUIRE(save.is_granted(0));
}

// #130: in vanilla kear mode a received Universal Kear (id 63) must grant a usable key. It skips the itemType-grant path (OnPickupDone with slot=-1
// aliases every kear onto bit 63); each new receipt fires the injected key-credit effect once, durable per index.
TEST_CASE("InboundGranter credits vanilla kears instead of granting itemType 63", "[inbound]")
{
    mth::ApState state;
    state.apply(mth::ApConnected{.slot_data = "{}", .player_slot = 1, .config = {.kear_mode = mth::KearMode::Vanilla}});
    mth::ApSaveState save = detached();
    FakeGranter granter;
    int credits = 0;
    mth::InboundGranter inbound(granter, state, save,
                                [&credits]
                                {
                                    ++credits;
                                    return true;
                                });

    state.apply(recv(mth::ap_item_id(63), 0));
    state.apply(recv(mth::ap_item_id(63), 1));
    inbound.tick();
    REQUIRE(credits == 2);
    REQUIRE(granter.granted.empty());
    REQUIRE(save.is_granted(0));
    REQUIRE(save.is_granted(1));

    inbound.tick(); // resend/next tick: already credited, no double
    REQUIRE(credits == 2);
}

TEST_CASE("InboundGranter retries a vanilla kear credit that is not ready", "[inbound]")
{
    mth::ApState state;
    state.apply(mth::ApConnected{.slot_data = "{}", .player_slot = 1, .config = {.kear_mode = mth::KearMode::Vanilla}});
    mth::ApSaveState save = detached();
    FakeGranter granter;
    bool ready = false;
    int credits = 0;
    mth::InboundGranter inbound(granter, state, save,
                                [&]
                                {
                                    if (!ready)
                                        return false;
                                    ++credits;
                                    return true;
                                });

    state.apply(recv(mth::ap_item_id(63), 0));
    inbound.tick();
    REQUIRE(credits == 0);
    REQUIRE_FALSE(save.is_granted(0));

    ready = true;
    inbound.tick();
    REQUIRE(credits == 1);
    REQUIRE(save.is_granted(0));
}

// In the AP-item kear modes the pool never carries id 63, so the credit effect stays dormant (the fix is vanilla-only).
TEST_CASE("InboundGranter does not credit kears outside vanilla mode", "[inbound]")
{
    mth::ApState state;
    state.apply(mth::ApConnected{.slot_data = "{}", .player_slot = 1, .config = {.kear_mode = mth::KearMode::ApItems}});
    mth::ApSaveState save = detached();
    FakeGranter granter;
    int credits = 0;
    mth::InboundGranter inbound(granter, state, save,
                                [&credits]
                                {
                                    ++credits;
                                    return true;
                                });

    state.apply(recv(mth::ap_item_id(63), 0));
    inbound.tick();
    REQUIRE(credits == 0);
    REQUIRE(granter.granted == std::vector<int>{63});
}

TEST_CASE("InboundGranter skips categories it cannot grant", "[inbound]")
{
    mth::ApState state;
    mth::ApSaveState save = detached();
    FakeGranter granter;
    // Mirrors TrapHooks::arm's first branch, the only check that rejects a trap-segment id no trap row claims; without it every index is Unavailable.
    mth::InboundGranter inbound(granter, state, save, {},
                                [](int idx) { return mth::trap_for_modifier(idx) != nullptr ? mth::TrapArm::Armed : mth::TrapArm::Unavailable; });

    state.apply(recv(mth::kProgStatCapBase + 0, 0));   // stat-cap: StatCapState's job -> skipped
    state.apply(recv(mth::kKearBlockItemBase + 1, 1)); // lock removal -> skipped
    state.apply(recv(mth::kTrapItemBase + 7, 2));      // trap segment, but modifier 7 is not a trap -> consumed
    state.apply(recv(mth::ap_item_id(9), 3));          // vanilla item -> granted
    inbound.tick();
    REQUIRE(granter.granted == std::vector<int>{9});
    REQUIRE(save.is_granted(2));
}

// The Nth receipt of a progressive chain grants that chain's tier-N itemType.
TEST_CASE("InboundGranter translates progressive weapons and the fishing rod to tiered itemTypes", "[inbound]")
{
    mth::ApState state;
    mth::ApSaveState save = detached();
    FakeGranter granter;
    mth::InboundGranter inbound(granter, state, save);

    // Whip x3 is itemTypes 2, 3, 4; Casket x1 is 14; Fishing Rod x3 is 87, 88, 89.
    state.apply(recv(mth::kProgWeaponBase, 0));
    state.apply(recv(mth::kProgFishingRodId, 1));
    state.apply(recv(mth::kProgWeaponBase, 2));
    state.apply(recv(mth::kProgWeaponBase + 4, 3));
    state.apply(recv(mth::kProgFishingRodId, 4));
    state.apply(recv(mth::kProgWeaponBase, 5));
    state.apply(recv(mth::kProgFishingRodId, 6));
    inbound.tick();
    REQUIRE(granter.granted == std::vector<int>{2, 87, 3, 14, 88, 4, 89});

    granter.granted.clear();
    inbound.tick();
    REQUIRE(granter.granted.empty());
}

TEST_CASE("InboundGranter consumes a progressive fishing rod beyond its top tier without granting", "[inbound]")
{
    mth::ApState state;
    mth::ApSaveState save = detached();
    FakeGranter granter;
    mth::InboundGranter inbound(granter, state, save);

    state.apply(recv(mth::kProgFishingRodId, 0));
    state.apply(recv(mth::kProgFishingRodId, 1));
    state.apply(recv(mth::kProgFishingRodId, 2));
    state.apply(recv(mth::kProgFishingRodId, 3)); // 4th: beyond tier 3 -> consumed, not granted, no retry
    inbound.tick();
    REQUIRE(granter.granted == std::vector<int>{87, 88, 89});
    REQUIRE(save.is_granted(3));
}

TEST_CASE("InboundGranter retries a progressive item at its correct tier after a failure", "[inbound]")
{
    struct Chain
    {
        std::int64_t item_id;
        std::vector<int> tiers;
    };
    for (const Chain &chain : {Chain{mth::kProgWeaponBase, {2, 3}}, Chain{mth::kProgFishingRodId, {87, 88}}})
    {
        INFO(chain.item_id);
        mth::ApState state;
        mth::ApSaveState save = detached();
        FakeGranter granter;
        granter.ok = false;
        mth::InboundGranter inbound(granter, state, save);

        state.apply(recv(chain.item_id, 0));
        state.apply(recv(chain.item_id, 1));
        inbound.tick();
        REQUIRE(granter.granted.empty());
        REQUIRE_FALSE(save.is_granted(0));

        granter.ok = true;
        inbound.tick();
        REQUIRE(granter.granted == chain.tiers);
    }
}

// #175: the real granter only queues on grant(); the item lands later in the engine's update window. Persisting "granted" on acceptance loses the
// item for good if that window never comes, and handing the in-flight receipt over again grants it twice.
TEST_CASE("InboundGranter persists a queued receipt only once it is applied, and never re-queues it", "[inbound]")
{
    mth::ApState state;
    mth::ApSaveState save = detached();
    FakeGranter granter;
    granter.defer = true;
    mth::InboundGranter inbound(granter, state, save);

    state.apply(recv(mth::ap_item_id(5), 0));
    inbound.tick();
    inbound.tick();
    REQUIRE(granter.granted == std::vector<int>{5});
    REQUIRE_FALSE(save.is_granted(0));

    granter.apply_queued();
    REQUIRE(save.is_granted(0));
    inbound.tick();
    REQUIRE(granter.granted == std::vector<int>{5});
}

// #175 loss window: the player quits with the batch still queued. Nothing was persisted, so a fresh granter over the same save file hands the item
// out again.
TEST_CASE("InboundGranter retries a receipt that was queued but never applied", "[inbound]")
{
    mth::ApState state;
    state.apply(recv(mth::ap_item_id(5), 0));

    std::string statefile; // stands in for the file: the only case here where state outlives the object

    {
        mth::ApSaveState save = backed_by(statefile);
        FakeGranter granter;
        granter.defer = true;
        mth::InboundGranter inbound(granter, state, save);
        inbound.tick();
        REQUIRE(granter.granted == std::vector<int>{5});
        REQUIRE_FALSE(save.is_granted(0));
    } // process exit: the queue dies with it

    // relaunch: the server replays the same stream against the persisted state
    mth::ApSaveState reloaded = backed_by(statefile);
    REQUIRE_FALSE(reloaded.is_granted(0));
    FakeGranter granter;
    mth::InboundGranter inbound(granter, state, reloaded);
    inbound.tick();
    REQUIRE(granter.granted == std::vector<int>{5});
    REQUIRE(reloaded.is_granted(0));
}

// A session change drops the queue; those receipts are retried against the new save, not acked into it.
TEST_CASE("InboundGranter re-queues receipts dropped by a session change", "[inbound]")
{
    mth::ApState state;
    mth::ApSaveState save = detached();
    FakeGranter granter;
    granter.defer = true;
    state.apply(recv(mth::ap_item_id(5), 0));

    {
        mth::InboundGranter inbound(granter, state, save);
        inbound.tick();
        REQUIRE(granter.granted == std::vector<int>{5});
    }
    granter.discard_pending(); // what App::clear_session_state does
    granter.granted.clear();

    mth::InboundGranter rebuilt(granter, state, save); // the next session, against its own save
    rebuilt.tick();
    REQUIRE(granter.granted == std::vector<int>{5});
    REQUIRE_FALSE(save.is_granted(0));
}

// The granter outlives its InboundGranter and holds the applied sink. If a replacement is built before the incumbent is torn down, the incumbent's
// teardown must disarm only its own sink, or grants apply with nothing marked and re-apply next launch.
TEST_CASE("InboundGranter teardown does not disarm a successor's applied sink", "[inbound]")
{
    mth::ApState state;
    mth::ApSaveState save = detached();
    FakeGranter granter;
    granter.defer = true;
    state.apply(recv(mth::ap_item_id(5), 0));

    auto first = std::make_unique<mth::InboundGranter>(granter, state, save);
    auto second = std::make_unique<mth::InboundGranter>(granter, state, save); // built before...
    first.reset();                                                             // ...the incumbent goes

    second->tick();
    REQUIRE(granter.granted == std::vector<int>{5});
    granter.apply_queued();
    REQUIRE(save.is_granted(0));
}

TEST_CASE("InboundGranter arms a trap once and marks it granted", "[inbound][trap]")
{
    mth::ApState state;
    mth::ApSaveState save = detached();
    FakeGranter granter;
    std::vector<int> armed;
    mth::InboundGranter inbound(granter, state, save, {},
                                [&armed](int idx)
                                {
                                    armed.push_back(idx);
                                    return mth::TrapArm::Armed;
                                });

    state.apply(recv(mth::kTrapItemBase + 204, 0)); // Mirror Mode
    state.apply(recv(mth::ap_item_id(9), 1));
    inbound.tick();
    REQUIRE(armed == std::vector<int>{204});
    REQUIRE(granter.granted == std::vector<int>{9});
    REQUIRE(save.is_granted(0));

    inbound.tick();
    REQUIRE(armed == std::vector<int>{204});
}

// A trap stuck on NotReady is retried later and must not hold up the receipts behind it.
TEST_CASE("InboundGranter retries a trap that is not ready without blocking later items", "[inbound][trap]")
{
    mth::ApState state;
    mth::ApSaveState save = detached();
    FakeGranter granter;
    std::vector<int> armed;
    mth::TrapArm result = mth::TrapArm::NotReady;
    mth::InboundGranter inbound(granter, state, save, {},
                                [&](int idx)
                                {
                                    if (result == mth::TrapArm::Armed)
                                        armed.push_back(idx);
                                    return result;
                                });

    state.apply(recv(mth::kTrapItemBase + 204, 0));
    state.apply(recv(mth::ap_item_id(9), 1));
    inbound.tick();
    REQUIRE(granter.granted == std::vector<int>{9});
    REQUIRE_FALSE(save.is_granted(0));

    result = mth::TrapArm::Armed;
    inbound.tick();
    inbound.tick();
    REQUIRE(armed == std::vector<int>{204});
    REQUIRE(save.is_granted(0));
}

TEST_CASE("InboundGranter consumes a trap the player already enabled", "[inbound][trap]")
{
    mth::ApState state;
    mth::ApSaveState save = detached();
    FakeGranter granter;
    mth::InboundGranter inbound(granter, state, save, {}, [](int) { return mth::TrapArm::Skipped; });

    state.apply(recv(mth::kTrapItemBase + 204, 0));
    inbound.tick();
    REQUIRE(save.is_granted(0));
}
