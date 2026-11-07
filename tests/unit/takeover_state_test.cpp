#include <catch2/catch_test_macros.hpp>

#include "mth/core/save/takeover_state.hpp"

using mth::next_takeover_step;
using mth::TakeoverInputs;
using mth::TakeoverStep;

namespace
{
// Ten minutes at 60 fps, far past any wait the takeover should need.
constexpr int kStuckFrames = 60 * 60 * 10;
} // namespace

TEST_CASE("a pending step fails closed without the save api, and when its wait is stuck", "[takeover]")
{
    TakeoverInputs no_api;
    no_api.save_api_ready = false;
    no_api.gameplay_entered = true;
    REQUIRE(next_takeover_step(TakeoverStep::AwaitingMenu, no_api) == TakeoverStep::Failed);
    REQUIRE(next_takeover_step(TakeoverStep::Launching, no_api) == TakeoverStep::Failed);

    TakeoverInputs stuck;
    stuck.save_api_ready = true;
    stuck.frames_in_step = kStuckFrames;
    REQUIRE(next_takeover_step(TakeoverStep::AwaitingMenu, stuck) == TakeoverStep::Failed);
    REQUIRE(next_takeover_step(TakeoverStep::Launching, stuck) == TakeoverStep::Failed);
}

TEST_CASE("the menu step waits for the profile-menu callback, the launch step for gameplay", "[takeover]")
{
    TakeoverInputs in;
    in.save_api_ready = true;
    in.gameplay_entered = true; // must not shortcut past the menu
    REQUIRE(next_takeover_step(TakeoverStep::AwaitingMenu, in) == TakeoverStep::AwaitingMenu);

    in.gameplay_entered = false;
    REQUIRE(next_takeover_step(TakeoverStep::Launching, in) == TakeoverStep::Launching);
    in.gameplay_entered = true;
    REQUIRE(next_takeover_step(TakeoverStep::Launching, in) == TakeoverStep::Running);
}

TEST_CASE("settled steps are stable, even without the save api", "[takeover]")
{
    TakeoverInputs in;
    in.save_api_ready = false;
    for (const auto step : {TakeoverStep::Idle, TakeoverStep::Running, TakeoverStep::Failed})
    {
        REQUIRE(mth::takeover_settled(step));
        REQUIRE(next_takeover_step(step, in) == step);
    }
    REQUIRE_FALSE(mth::takeover_settled(TakeoverStep::AwaitingMenu));
    REQUIRE_FALSE(mth::takeover_settled(TakeoverStep::Launching));
}

TEST_CASE("input is blocked while the takeover drives the menu, and reaches the game once it runs", "[takeover]")
{
    mth::InputBlockState block;
    REQUIRE(block.advance(TakeoverStep::AwaitingMenu, false));
    REQUIRE(block.advance(TakeoverStep::Launching, true));
    REQUIRE(block.blocked());

    REQUIRE_FALSE(block.advance(TakeoverStep::Running, true));
    REQUIRE_FALSE(block.blocked());
    REQUIRE_FALSE(block.advance(TakeoverStep::Idle, true));

    // A failed takeover keeps swallowing input only while the menu is the live gamestate.
    mth::InputBlockState failed;
    REQUIRE(failed.advance(TakeoverStep::Failed, true));
    REQUIRE_FALSE(failed.advance(TakeoverStep::Failed, false));
}

TEST_CASE("a block that never clears is eventually released for good, until a fresh claim", "[takeover]")
{
    mth::InputBlockState block;
    block.rearm();
    bool released = false;
    for (int i = 0; i < kStuckFrames && !released; ++i)
        released = !block.advance(TakeoverStep::AwaitingMenu, false);
    REQUIRE(released);

    // The steps still want the block; only the spent budget holds it off, and it must stay off.
    for (const auto step : {TakeoverStep::AwaitingMenu, TakeoverStep::Launching, TakeoverStep::Failed})
        REQUIRE_FALSE(block.advance(step, true));
    REQUIRE_FALSE(block.blocked());

    block.rearm();
    REQUIRE(block.advance(TakeoverStep::AwaitingMenu, false));
}

TEST_CASE("flush_refusal: a commit needs all three conditions, and each one alone can stop it", "[takeover]")
{
    // Three necessary conditions, so the order the guards run in stays free. #152 is the gameplay one: outside gameplay the live slot is the cleared
    // title slot, and committing it destroys the run.
    for (mth::TakeoverStep step :
         {mth::TakeoverStep::Idle, mth::TakeoverStep::AwaitingMenu, mth::TakeoverStep::Launching, mth::TakeoverStep::Running, mth::TakeoverStep::Failed})
        for (bool gameplay : {false, true})
            for (bool same_seed : {false, true})
                for (bool same_slot : {false, true})
                {
                    const mth::FlushInputs in{step, gameplay, "seed", "slot", same_seed ? "seed" : "other", same_slot ? "slot" : "other"};
                    INFO("step=" << mth::takeover_step_name(step) << " gameplay=" << gameplay << " seed=" << in.current_seed << " slot=" << in.current_slot);
                    if (step != mth::TakeoverStep::Running || !gameplay || !same_seed || !same_slot)
                        REQUIRE(mth::flush_refusal(in) != nullptr);
                }
}

TEST_CASE("flush_refusal: a running takeover in gameplay commits its own session's blob", "[takeover]")
{
    // Otherwise the invariants above are satisfied by refusing everything.
    REQUIRE(mth::flush_refusal(mth::FlushInputs{mth::TakeoverStep::Running, true, "seed", "slot", "seed", "slot"}) == nullptr);
}
