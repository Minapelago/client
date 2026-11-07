#include <catch2/catch_test_macros.hpp>

#include "mth/core/ap/connect_resend_gate.hpp"

TEST_CASE("connect_resend_gate: fires once per connection when connected+ready", "[mth][resend]")
{
    mth::ConnectResendGate g;
    REQUIRE(g.fire(true, true));
    REQUIRE_FALSE(g.fire(true, true));
    REQUIRE_FALSE(g.fire(true, true));
}

TEST_CASE("connect_resend_gate: waits for inbound_ready", "[mth][resend]")
{
    mth::ConnectResendGate g;
    REQUIRE_FALSE(g.fire(true, false));
    REQUIRE_FALSE(g.fire(true, false));
    REQUIRE(g.fire(true, true));
    REQUIRE_FALSE(g.fire(true, true));
}

TEST_CASE("connect_resend_gate: never fires while disconnected", "[mth][resend]")
{
    mth::ConnectResendGate g;
    REQUIRE_FALSE(g.fire(false, true));
    REQUIRE_FALSE(g.fire(false, false));
}

TEST_CASE("connect_resend_gate: re-arms and fires again on reconnect", "[mth][resend]")
{
    mth::ConnectResendGate g;
    REQUIRE(g.fire(true, true));
    REQUIRE_FALSE(g.fire(true, true));
    REQUIRE_FALSE(g.fire(false, true));
    REQUIRE(g.fire(true, true));
}
