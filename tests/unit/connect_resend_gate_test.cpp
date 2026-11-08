#include <catch2/catch_test_macros.hpp>

#include "mth/core/ap/connect_resend_gate.hpp"

TEST_CASE("connect_resend_gate: fires once per connection", "[mth][resend]")
{
    mth::ConnectResendGate g;
    REQUIRE(g.fire(true));
    REQUIRE_FALSE(g.fire(true));
    REQUIRE_FALSE(g.fire(true));
}

TEST_CASE("connect_resend_gate: never fires while disconnected", "[mth][resend]")
{
    mth::ConnectResendGate g;
    REQUIRE_FALSE(g.fire(false));
    REQUIRE_FALSE(g.fire(false));
}

TEST_CASE("connect_resend_gate: re-arms and fires again on reconnect", "[mth][resend]")
{
    mth::ConnectResendGate g;
    REQUIRE(g.fire(true));
    REQUIRE_FALSE(g.fire(true));
    REQUIRE_FALSE(g.fire(false));
    REQUIRE(g.fire(true));
}
