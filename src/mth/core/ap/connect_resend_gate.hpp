#pragma once

namespace mth
{

// Reconnect-resend latch: fire() returns true once per connection, on the first call where `connected` holds, and re-arms when `connected` drops.
// Pure; each SeedSession owns one.
class ConnectResendGate
{
  public:
    bool fire(bool connected)
    {
        if (!connected)
        {
            latched_ = false;
            return false;
        }
        if (latched_)
            return false;
        latched_ = true;
        return true;
    }

  private:
    bool latched_{false};
};

} // namespace mth
