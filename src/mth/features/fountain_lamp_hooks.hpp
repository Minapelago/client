#pragma once

#include <cstdint>

namespace mth
{

// HubFountain::Bulb::Update detour that forces chosen Ossex fountain lamps lit (visual only; never writes SaveSlot+0x290). Lamp i is forced when bit i is set
// in either mask: the slot_data mask from the game-thread tick, or the sticky console override, which works offline.
void install_fountain_lamp_hook();
void remove_fountain_lamp_hook();
void set_fountain_slot_mask(std::uint32_t mask);    // game-thread tick
void set_fountain_console_mask(std::uint32_t mask); // any thread (the console runs on the render thread)

} // namespace mth
