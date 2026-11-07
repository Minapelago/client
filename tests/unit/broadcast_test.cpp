#include <string>

#include <catch2/catch_test_macros.hpp>

#include "mth/core/ap/ap_events.hpp"
#include "mth/core/ap/broadcast.hpp"

using mth::banner_color;
using mth::BannerQueue;
using mth::broadcast_relevant;
using mth::deathlink_banner_text;

TEST_CASE("broadcast_relevant: an item message shows when we sent, received or found it, on our team", "[mth][broadcast]")
{
    // our team 0, our slot 3
    REQUIRE(broadcast_relevant("ItemSend", 0, 3, 0, 3, std::nullopt, std::nullopt));
    REQUIRE(broadcast_relevant("ItemSend", 0, 3, 0, std::nullopt, 3, std::nullopt));
    REQUIRE(broadcast_relevant("ItemSend", 0, 3, std::nullopt, std::nullopt, 3, std::nullopt));
    REQUIRE(broadcast_relevant("ItemSend", 0, 3, 0, 9, 3, std::nullopt));
    // A check we sent: the item goes to slot 9 but its finder (item.player) is us.
    REQUIRE(broadcast_relevant("ItemSend", 0, 3, std::nullopt, std::nullopt, 9, 3));

    REQUIRE_FALSE(broadcast_relevant("ItemSend", 0, 3, 1, std::nullopt, 9, 3));
    REQUIRE_FALSE(broadcast_relevant("ItemSend", 0, 3, 0, 5, 7, 8));
    REQUIRE_FALSE(broadcast_relevant("ItemSend", 0, 3, 1, 3, std::nullopt, 3));
    REQUIRE_FALSE(broadcast_relevant("Hint", 0, 3, std::nullopt, std::nullopt, 9, std::nullopt));
    REQUIRE_FALSE(broadcast_relevant("Join", 0, 3, 0, 9, std::nullopt, std::nullopt));
    REQUIRE_FALSE(broadcast_relevant("ItemSend", 0, 3, std::nullopt, std::nullopt, std::nullopt, std::nullopt));
}

TEST_CASE("broadcast_relevant: announcements pass the slot gate", "[mth][broadcast]")
{
    // A /say from the server operator: no team, no slot, no item, so the slot gate has nothing to match.
    REQUIRE(broadcast_relevant("ServerChat", 0, 3, std::nullopt, std::nullopt, std::nullopt, std::nullopt));
    // Player chat carries the sender's slot; someone else's message is still shown.
    REQUIRE(broadcast_relevant("Chat", 0, 3, 0, 9, std::nullopt, std::nullopt));
    REQUIRE(broadcast_relevant("Countdown", 0, 3, std::nullopt, std::nullopt, std::nullopt, std::nullopt));
    REQUIRE(broadcast_relevant("CommandResult", 0, 3, std::nullopt, std::nullopt, std::nullopt, std::nullopt));
    // Team still filters when the message carries one.
    REQUIRE_FALSE(broadcast_relevant("Chat", 0, 3, 1, 9, std::nullopt, std::nullopt));
}

TEST_CASE("banner_color: type/flags select distinct AP colors", "[mth][broadcast]")
{
    const auto progression = banner_color("item_id", "", 1u, 0u, false); // FLAG_ADVANCEMENT
    const auto useful = banner_color("item_id", "", 2u, 0u, false);      // FLAG_NEVER_EXCLUDE
    const auto trap = banner_color("item_id", "", 4u, 0u, false);        // FLAG_TRAP
    const auto filler = banner_color("item_id", "", 0u, 0u, false);
    REQUIRE(progression != filler);
    REQUIRE(useful != filler);
    REQUIRE(trap != filler);
    REQUIRE(progression != useful);

    // player_id self vs other differ
    REQUIRE(banner_color("player_id", "", 0u, 0u, true) != banner_color("player_id", "", 0u, 0u, false));

    // an explicit node color overrides the type-derived color
    REQUIRE(banner_color("item_id", "red", 1u, 0u, false) == banner_color("text", "red", 0u, 0u, false));

    // every color is fully opaque (alpha byte set); the fade scales it later
    REQUIRE((filler >> 24) == 0xFFu);
}

TEST_CASE("BannerQueue: a queued message waits until a visible slot frees, in order", "[mth][broadcast]")
{
    constexpr double step = BannerQueue::kPromoteIntervalSeconds;
    constexpr double life = BannerQueue::kHoldSeconds + BannerQueue::kFadeSeconds;

    BannerQueue q;
    for (int i = 0; i < BannerQueue::kMaxVisible; ++i)
        q.push({{"filler", 0xFFFFFFFFu}});
    q.push({{"overflow", 0xFFFFFFFFu}}); // one past the cap -> must wait

    // Fill every slot, one per interval; the first filler starts at 0.0 and the last at step*(kMaxVisible-1).
    for (int i = 0; i < BannerQueue::kMaxVisible; ++i)
        REQUIRE_FALSE(q.update(step * i).empty());
    REQUIRE(static_cast<int>(q.update(step * BannerQueue::kMaxVisible).size()) == BannerQueue::kMaxVisible);

    // The first filler fades out at `life`, freeing a slot for the overflow message.
    const auto after = q.update(life);
    REQUIRE(static_cast<int>(after.size()) == BannerQueue::kMaxVisible);
    REQUIRE(after.back().segments[0].text == "overflow");

    REQUIRE(q.update(life * 2.0).empty());
}

TEST_CASE("deathlink_banner_text: names the sender and keeps the cause as written", "[mth][broadcast][deathlink]")
{
    // A cause is already a full sentence naming the sender.
    REQUIRE(deathlink_banner_text("Amaterasu", "Amaterasu was crushed by a spike trap") == "Amaterasu was crushed by a spike trap");
    REQUIRE(deathlink_banner_text("", "somebody blew up") == "somebody blew up");

    const std::string line = deathlink_banner_text("Amaterasu", "");
    REQUIRE(line.find("Amaterasu") != std::string::npos);
    REQUIRE(line != "Amaterasu");
    REQUIRE_FALSE(deathlink_banner_text("", "").empty());
}

TEST_CASE("format_scout_desc: names another player's recipient and game, and neither for our own slot", "[scout]")
{
    const std::string other = mth::format_scout_desc(mth::ScoutInfo{1, "Sword", "Alice", "Some Game", 0u, false});
    REQUIRE(other.find("Alice") != std::string::npos);
    REQUIRE(other.find("Some Game") != std::string::npos);

    // A self-scout that fell through to the other-player branch would advertise "for Mina (Mina the Hollower)" in the shop.
    const std::string self = mth::format_scout_desc(mth::ScoutInfo{1, "Sword", "Mina", "Mina the Hollower", 0u, true});
    REQUIRE(self.find("Mina") == std::string::npos);
}
