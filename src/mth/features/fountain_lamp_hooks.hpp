#pragma once

#include <cstdint>

namespace mth
{

// The HubFountain::Bulb::Update detour that forces chosen Ossex fountain lamps lit (visual only; never
// writes SaveSlot+0x290). A lamp is forced lit when its bit (bit i => lamp i) is set in either mask: the
// slot_data mask the game-thread tick publishes, or the dev console override, which is sticky and works
// offline.
void install_fountain_lamp_hook();
void remove_fountain_lamp_hook();
void set_fountain_slot_mask(std::uint32_t mask);    // game-thread tick
void set_fountain_console_mask(std::uint32_t mask); // any thread (the console runs on the render thread)

} // namespace mth
