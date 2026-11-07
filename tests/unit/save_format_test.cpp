#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "mth/core/save/ap_save_state.hpp"
#include "mth/core/save/ap_save_store.hpp"

namespace
{
// A state with no backing store: the format is what these cases exercise.
mth::ApSaveState detached()
{
    return mth::ApSaveState([] { return std::nullopt; }, [](std::string_view) {});
}
} // namespace

TEST_CASE("a statefile written by earlier builds still loads", "[ap_save_state]")
{
    mth::ApSaveState s([] { return std::optional<std::string>("c 1\nc 2\ng 7\ns 3\n"); }, [](std::string_view) {});
    REQUIRE(s.checked() == std::set<int>{1, 2});
    REQUIRE(s.is_granted(7));
    REQUIRE_FALSE(s.is_granted(1));
    REQUIRE(s.game_slot() == 3);
}

TEST_CASE("state round-trips through serialize and deserialize", "[ap_save_state]")
{
    auto a = detached();
    a.mark_checked(10);
    a.mark_granted(20);
    a.set_game_slot(1);

    auto b = detached();
    b.deserialize(a.serialize());
    REQUIRE(b.is_checked(10));
    REQUIRE(b.is_granted(20));
    REQUIRE(b.game_slot() == 1);

    auto no_slot = detached();
    no_slot.mark_checked(4);
    auto c = detached();
    c.deserialize(no_slot.serialize());
    REQUIRE(c.is_checked(4));
    REQUIRE(c.game_slot() == -1);
}

TEST_CASE("state tolerates a truncated trailing record", "[ap_save_state]")
{
    auto s = detached();
    s.deserialize("c 1\ng 2\nc "); // interrupted write
    REQUIRE(s.is_checked(1));
    REQUIRE(s.is_granted(2));
}

TEST_CASE("stage hands the store text that loads back into the same state", "[ap_save_state]")
{
    std::string written;
    mth::ApSaveState s([] { return std::nullopt; }, [&written](std::string_view t) { written = std::string(t); });
    s.mark_checked(9);
    s.mark_granted(4);
    s.stage();

    mth::ApSaveState reloaded([&written] { return std::optional<std::string>(written); }, [](std::string_view) {});
    REQUIRE(reloaded.is_checked(9));
    REQUIRE(reloaded.is_granted(4));
}

// Earlier builds wrote these names, so a changed hash or sanitizer strands every existing AP save.
TEST_CASE("save filename combines the sanitized seed and slot with a hash of the raw parts", "[savestore]")
{
    REQUIRE(mth::ap_save_filename("SEED123", "Mina") == "ap_SEED123_Mina_c28d5524018af663.ycsave");
    REQUIRE(mth::ap_save_filename("SEED 1", "my slot") == "ap_SEED_1_my_slot_7db9e5f646585915.ycsave");

    REQUIRE(mth::sanitize_save_key_part("a/b\\c") == "a_b_c");
    REQUIRE(mth::sanitize_save_key_part("..") == "__");
    REQUIRE(mth::sanitize_save_key_part("") == "unnamed");
}

TEST_CASE("save filename hash avoids sanitization collisions", "[savestore]")
{
    // "a b" and "a_b" sanitize identically, but the raw key parts differ.
    REQUIRE(mth::ap_save_filename("a b", "x") != mth::ap_save_filename("a_b", "x"));
}
