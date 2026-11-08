#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "mth/core/console/command_queue.hpp"

TEST_CASE("CommandQueue runs commands posted from another thread once each, in order, inside drain", "[command_queue]")
{
    constexpr int kCount = 2000;
    mth::CommandQueue queue;
    std::vector<int> ran;
    bool in_drain = false;
    int outside_drain = 0;
    std::atomic<bool> posted_all{false};

    std::thread poster(
        [&]
        {
            for (int i = 0; i < kCount; ++i)
                queue.post(
                    [&, i]
                    {
                        if (!in_drain)
                            ++outside_drain;
                        ran.push_back(i);
                    });
            posted_all = true;
        });

    auto drain = [&]
    {
        in_drain = true;
        queue.drain();
        in_drain = false;
    };
    while (!posted_all)
        drain();
    poster.join();
    drain();

    REQUIRE(outside_drain == 0);
    REQUIRE(ran.size() == static_cast<std::size_t>(kCount));
    for (int i = 0; i < kCount; ++i)
        REQUIRE(ran[static_cast<std::size_t>(i)] == i);
}

TEST_CASE("CommandQueue defers a command posted during a drain to the next drain", "[command_queue]")
{
    mth::CommandQueue queue;
    std::vector<int> ran;
    queue.post(
        [&]
        {
            ran.push_back(1);
            queue.post([&] { ran.push_back(2); });
        });

    queue.drain();
    REQUIRE(ran == std::vector<int>{1});
    queue.drain();
    REQUIRE(ran == std::vector<int>{1, 2});
}
