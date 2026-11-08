#pragma once

#include <array>

namespace mth
{

class App;

// Dedicated AP connect window: Server/Slot/Password fields and a Connect<->Disconnect
// button. Reads connection truth from App each frame; owns no connection state.
class LoginWindow
{
  public:
    explicit LoginWindow(App &app);

    LoginWindow(const LoginWindow &) = delete;
    LoginWindow &operator=(const LoginWindow &) = delete;

    void draw(bool login_open);

  private:
    App &app_;
    std::array<char, 256> server_{}; // "host:port"
    std::array<char, 128> slot_{};
    std::array<char, 128> password_{};
};

} // namespace mth
