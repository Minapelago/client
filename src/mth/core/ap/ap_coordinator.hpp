#pragma once

#include <functional>
#include <string>
#include <vector>

namespace mth
{

class IApLink;
class ApState;
struct BannerSegment;
struct ScoutInfo;

// Folds transport events into AP state on the game thread. Testable with a fake link.
class ApCoordinator
{
  public:
    // Callbacks run on the game thread from tick(). on_session_reset: an ApConnected or ApDisconnected was drained (e.g. clear the scout cache).
    // on_session_end: the link reports a different (seed, slot), just before that connection's ApConnected applies; never on a same seed+slot reconnect.
    // on_death gets the bounce's source (who died) and cause.
    ApCoordinator(IApLink &link, ApState &state, std::function<void(const std::string &source, const std::string &cause)> on_death = {},
                  std::function<void(const std::vector<BannerSegment> &)> on_broadcast = {}, std::function<void(const std::vector<ScoutInfo> &)> on_scout = {},
                  std::function<void()> on_session_reset = {}, std::function<void()> on_session_end = {});

    void tick();

  private:
    IApLink &link_;
    ApState &state_;
    std::function<void(const std::string &source, const std::string &cause)> on_death_;
    std::function<void(const std::vector<BannerSegment> &)> on_broadcast_;
    std::function<void(const std::vector<ScoutInfo> &)> on_scout_;
    std::function<void()> on_session_reset_;
    std::function<void()> on_session_end_;
};

} // namespace mth
