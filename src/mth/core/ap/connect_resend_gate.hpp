#pragma once

namespace mth
{

// Reconnect-resend latch: fire() returns true once per connection, on the first call where `connected` and `inbound_ready` both hold
// (in either order), and re-arms when `connected` drops. Pure; App owns one.
class ConnectResendGate
{
  public:
    bool fire(bool connected, bool inbound_ready)
    {
        if (!connected)
        {
            latched_ = false;
            return false;
        }
        if (latched_ || !inbound_ready)
            return false;
        latched_ = true;
        return true;
    }

  private:
    bool latched_{false};
};

} // namespace mth
