#pragma once

#include "mth/core/rules/ap_gate.hpp"

namespace mth
{

// Static half of the gate's inputs; App adds liveness. Each probe logs its result. Call after pal::init_hook_engine() and before any mod write to game
// memory, or this validates the patch tables::repurpose_dummy_item() made.
[[nodiscard]] GateInputs run_static_gate_probes(unsigned int game_revision);

} // namespace mth
