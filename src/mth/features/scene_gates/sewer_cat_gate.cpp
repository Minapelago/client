#include "mth/features/scene_gates/sewer_cat_gate.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "mod/mod_api.hpp"
#include "mth/core/game/component_types.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/features/scene_gates/scene_walk.hpp"
#include "pal/pal_log.hpp"

namespace
{

using mth::looks_like_component;

// The game's ownership link, and the only path that names THIS vendor's component. Both offsets are
// Linux-derived, so both hops are validated.
[[nodiscard]] void *interact_via_owner(void *behavior)
{
    void *owner = *reinterpret_cast<void **>(static_cast<char *>(behavior) + mth::layout::kSewerCatEntityOff);
    if (!looks_like_component(owner))
        return nullptr;
    void *ic = *reinterpret_cast<void **>(static_cast<char *>(owner) + mth::layout::kNpcEntityInteractOff);
    if (!looks_like_component(ic) || !mod::component_isa(ic, mth::rtti::kInteractComponent))
        return nullptr;
    return ic;
}

} // namespace

namespace mth
{

SewerCatGate::SewerCatGate(std::function<bool()> should_disable) : should_disable_(std::move(should_disable))
{
}

// The owner link decides. The sibling is a fallback for a moved owner offset, usable only when the child
// list held exactly one interact component: nothing binds a sibling to a particular NPC, so guessing with
// two NPCs on one entity disables the wrong one and then hides it, every later walk finding the byte
// already set. The winning path is logged because a sibling win means the owner offset needs re-checking.
// Writes only on a 0->1 transition, so the log marks the tick that changed something.
void SewerCatGate::disable_vendor(void *behavior, void *sibling_interact, bool sibling_unique)
{
    const char *via = "owner";
    void *ic = interact_via_owner(behavior);
    if (ic == nullptr && sibling_unique)
    {
        ic = sibling_interact;
        via = "sibling";
    }
    if (ic == nullptr)
    {
        if (!warned_no_interact_)
        {
            warned_no_interact_ = true;
            pal::logf(pal::LogLevel::Warn, "panino: fetch vendor %p has no unambiguous interact component; left live (#88)", behavior);
        }
        return;
    }
    if (disable_interact(ic))
        pal::logf(pal::LogLevel::Info, "panino: fetch vendor %p disabled (interact %p via %s) (#88)", behavior, ic, via);
}

void SewerCatGate::tick()
{
    if (!should_disable_ || !should_disable_())
        return;
    void *root = walker_.due_root();
    if (root == nullptr)
        return;

    // Behaviour and interact component are docked as siblings, so one node's children yield both. The
    // count is what makes the sibling usable: one under this entity can only be his, several cannot be
    // told apart.
    const SceneWalk walk = walker_.walk(root,
                                        [&](std::span<void *const> children)
                                        {
                                            void *vendor = nullptr;
                                            void *interact = nullptr;
                                            std::size_t interact_count = 0;
                                            for (void *c : children)
                                            {
                                                if (mod::component_isa(c, rtti::kNpcBehaviorSewerCat))
                                                    vendor = c;
                                                else if (mod::component_isa(c, rtti::kInteractComponent))
                                                {
                                                    interact = c;
                                                    ++interact_count;
                                                }
                                            }
                                            if (vendor == nullptr)
                                                return true;
                                            disable_vendor(vendor, interact, interact_count == 1);
                                            return false; // one dedicated instance per world; nothing left to find
                                        });
    if (walk.stopped_by_visitor)
        return;
    walker_.report(walk);
}

void SewerCatGate::on_world_destroy()
{
    walker_.reset_cadence();
}

} // namespace mth
