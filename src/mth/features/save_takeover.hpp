#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "mth/core/save/ap_save_bundle.hpp"
#include "mth/core/save/takeover_state.hpp"
#include "mth/features/scene_gates/scene_walk.hpp"

namespace mth
{

// Runs the game on a mod-owned save without the player picking a file: the mod stages its save into a vanilla slot and drives the profile-select menu into its
// launch state (mth::layout::kProfileMenuLaunchState). Vanilla save writes stop when AP connects, so saveData.yc and its Steam Cloud copy stay untouched; our
// own persistence flushes on the game's save cadence.
class SaveTakeover
{
  public:
    using IdentityFn = std::function<std::pair<std::string, std::string>()>; // (seed, slot)

    // The store outlives this: App owns it and declares it ahead of the hook manager.
    SaveTakeover(ApSaveBundleStore &store, IdentityFn identity);

    SaveTakeover(const SaveTakeover &) = delete;
    SaveTakeover &operator=(const SaveTakeover &) = delete;

    // Claims the launch. The vanilla StartGame must then run, since its substate builds the profile-select menu this drives. false means block the launch.
    bool begin();

    void tick();
    void on_game_save_requested();

    // Finds and drives the live profile-select menu in the menu world's scene graph, once per World::Update end (world may be null). Re-found each pass because
    // the intro cinematic owns its lifetime.
    void on_world_update_end(void *world);

    // True from claim until settled. The dev console's save-write toggle refuses while this holds, or the mod-owned session would leak into the player's
    // vanilla saveData.yc.
    [[nodiscard]] bool takeover_active() const
    {
        return !takeover_settled(step_) || step_ == TakeoverStep::Running;
    }

    [[nodiscard]] std::vector<std::string> status_lines() const;

  private:
    void update_input_block();
    void drive_profile_menu(void *menu);
    void flush();
    bool stage_save();
    void fail(const char *why);
    void set_step(TakeoverStep next);

    ApSaveBundleStore &store_;
    IdentityFn identity_;
    TakeoverStep step_{TakeoverStep::Idle};
    int frames_in_step_{0};
    SceneWalker walker_{"takeover", "the profile menu", "", "menu-world scene walk"};

    bool warned_input_inert_{false};
    InputBlockState input_block_;
    std::string seed_;
    std::string slot_;
};

// The mod's own save directory: a "saves" subdirectory of the same base pal::log_dir() resolves to.
std::filesystem::path mod_save_dir();

} // namespace mth
