#include "mth/features/scene_gates/ticket_machine_gate.hpp"

#include <cstddef>
#include <cstdint>

#include "mod/mod_api.hpp"
#include "mth/core/game/component_types.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/features/scene_gates/scene_walk.hpp"
#include "pal/pal_log.hpp"

namespace
{

using mth::looks_like_component;

// Disables the machine's interact component, logging only the tick that changed it.
void disable_machine(void *machine)
{
    void *ic = *reinterpret_cast<void **>(static_cast<char *>(machine) + mth::layout::kTicketMachineInteractOff);
    if (!looks_like_component(ic) || !mod::component_isa(ic, mth::rtti::kInteractComponent))
    {
        pal::logf(pal::LogLevel::Warn, "train: donation machine %p has no interact component at +%#zx; left live (#162)", machine,
                  static_cast<std::size_t>(mth::layout::kTicketMachineInteractOff));
        return;
    }
    if (mth::disable_interact(ic))
        pal::logf(pal::LogLevel::Info, "train: donation machine %p disabled (interact %p) (#162)", machine, ic);
}

} // namespace

namespace mth
{

void TicketMachineGate::tick()
{
    void *root = walker_.due_root();
    if (root == nullptr)
        return;

    const SceneWalk walk = walker_.for_each(root, rtti::kTicketMachine, [&](void *c) { disable_machine(c); });
    walker_.report(walk);
}

void TicketMachineGate::on_world_destroy()
{
    walker_.reset_cadence();
}

} // namespace mth
