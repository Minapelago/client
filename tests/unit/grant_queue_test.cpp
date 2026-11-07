#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "mth/core/ap/grant_queue.hpp"

namespace
{
struct Recorder
{
    std::vector<std::string> log;
    void apply(int item_type)
    {
        log.push_back("apply " + std::to_string(item_type));
    }
    void ack(int receipt)
    {
        log.push_back("ack " + std::to_string(receipt));
    }
};

void drain(mth::GrantQueue &queue, Recorder &rec, bool ready)
{
    queue.drain([ready] { return ready; }, [&](int item_type) { rec.apply(item_type); }, [&](int receipt) { rec.ack(receipt); });
}
} // namespace

TEST_CASE("GrantQueue acks a grant only after applying it, and keeps it while the game is not ready", "[grant_queue]")
{
    mth::GrantQueue queue;
    Recorder rec;

    queue.push(41, 0);
    drain(queue, rec, false);
    REQUIRE(rec.log.empty());

    queue.push(42, 1);
    drain(queue, rec, true);
    REQUIRE(rec.log == std::vector<std::string>{"apply 41", "ack 0", "apply 42", "ack 1"});
    REQUIRE(queue.clear() == 0);
}

TEST_CASE("GrantQueue requeues a not-ready batch ahead of grants pushed during the drain", "[grant_queue]")
{
    mth::GrantQueue queue;
    Recorder rec;

    queue.push(41, 0);
    queue.drain(
        [&]
        {
            queue.push(43, 2);
            return false;
        },
        [&](int item_type) { rec.apply(item_type); }, [&](int receipt) { rec.ack(receipt); });
    REQUIRE(rec.log.empty());

    drain(queue, rec, true);
    REQUIRE(rec.log == std::vector<std::string>{"apply 41", "ack 0", "apply 43", "ack 2"});
}
