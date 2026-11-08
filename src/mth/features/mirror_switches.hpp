#pragma once

#include <cstdint>

namespace mth
{

class ApState;

// Mirrors End shortcut switches as AP items (#28): each of the five opens its locked room and raises its platforms once its item arrives. A switch follows one
// int in its world's SwitchManager, so assigning the granted set each pass grants what AP sent and reverts manual hits, like the train-line clamp.
void tick_mirror_switches(const ApState &state, bool slot_ok);

// Room teardown: each room is its own World and its switches die with it, so the held pointers are dropped here.
void mirror_switches_on_world_destroy();

// Dev probe: log every rainbow switch in the room, with the level-data fields that tell them apart.
// Console verb `switches`; the next tick_mirror_switches pass does the walk.
void request_switch_probe();

// Offline test: drives the switches without an AP session or the bound-save check. The clamp writes the save, so use a scratch save.
void set_mirror_switches_override_flag(bool on);

} // namespace mth
