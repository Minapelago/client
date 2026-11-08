#pragma once

#include <string>
#include <vector>

#include "mth/core/rules/progression.hpp"

namespace mth
{
class ApState;

// twin: mth/core/rules/progression.hpp (pure cap policy).
// Owns the level-cap PAL hook and the StatCapState policy. Installs whenever the PAL has the feature; the provider returns the vanilla cap until enforcement is
// live (AP session or offline test mode). Game thread only.
class LevelCapHooks
{
  public:
    LevelCapHooks();
    LevelCapHooks(const LevelCapHooks &) = delete;
    LevelCapHooks &operator=(const LevelCapHooks &) = delete;

    void recompute(const ApState &state);                  // refresh caps from received items
    void set_counts(int attack, int defense, int sidearm); // offline test override
    void set_enforce_live(bool on);                        // off leaves vanilla caps

    // Bone-up menu annotation: the level to display as the cap for `stat`, or <0 to leave the
    // vanilla text alone (not enforcing, or not one of the real stats). Game thread.
    [[nodiscard]] int display_cap_for(int stat) const;

    [[nodiscard]] std::vector<std::string> status_lines() const;

  private:
    int provide(int stat, int vanilla_cap); // PAL callback (game thread)

    StatCapState caps_;
    int max_stat_level_{99}; // slot_data ceiling for the real stats; replaces the game's native max (game thread)
    bool enforce_live_{false};
};

} // namespace mth
