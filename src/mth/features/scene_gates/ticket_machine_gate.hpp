#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mth/features/scene_gates/scene_walk.hpp"

namespace mth
{

// Makes the station's train-pass donation machine non-interactible (#162); vanilla it dispenses the generic Train Pass for 10000 bones. Applied when the seed
// has no machine location, and after the check is sent when it does, since a suppressed grant leaves the machine's collected bit false and it would re-dispense
// forever. With no hook available, this walks the scene graph and sets the interact component's disable byte. The machine stays visible and solid. The walk
// repeats on a slow cadence so room loads, late spawns and recycled world pointers self-correct. No game pointer is held across ticks.
class TicketMachineGate
{
  public:
    void tick();             // game thread, World::Update pre-window; the caller decides when suppression applies
    void on_world_destroy(); // re-walk on the next tick

  private:
    SceneWalker walker_{"train", "the donation machine", " (#162)"};
};

} // namespace mth
