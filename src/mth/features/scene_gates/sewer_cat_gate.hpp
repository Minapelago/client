#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "mth/features/scene_gates/scene_walk.hpp"

namespace mth
{

// Makes the sewer-cat fetch vendor ("Panino") non-interactible while should_disable() is true (#88). Vanilla, he takes an order for an item still in the world
// and later spawns the real pickup, handing over an item AP never routed.
//
// Found by scene walk because his OnNPCEvent is neither exposed by the mod API nor signature-carved, so a detour resolves to nothing on Windows. Only the
// prompt is suppressed: his UpdateState still delivers orders already in the save.
//
// The walk repeats because he is reconstructed on each appearance. No game pointer is held across ticks.
class SewerCatGate
{
  public:
    explicit SewerCatGate(std::function<bool()> should_disable);

    void tick();             // game thread; walks nothing unless should_disable()
    void on_world_destroy(); // re-walk on the next tick

  private:
    void disable_vendor(void *behavior, void *sibling_interact, bool sibling_unique);

    std::function<bool()> should_disable_;
    bool warned_no_interact_{false}; // found him, could not attribute an interact component to him
    SceneWalker walker_{"panino", "the fetch vendor", " (#88)"};
};

} // namespace mth
